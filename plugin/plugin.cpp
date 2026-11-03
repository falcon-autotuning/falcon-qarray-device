#include "falcon-qarray-device/Device.hpp"
#include <cjson/cJSON.h>
#include <instrument-data.h>
#include <instrument-log/inst_logging.h>
#include <instrument-plugin.h>
#include <plugin-api.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <pybind11/embed.h>
#include <string>
#include <vector>

namespace py = pybind11;

static const char PLUGIN_NAME[] = "QArraySimulator";

#define QARRAY_LOG_INFO(fmt, ...) LOG_INFO("Plugin", PLUGIN_NAME, fmt, ##__VA_ARGS__)
#define QARRAY_LOG_DEBUG(fmt, ...) LOG_DEBUG("Plugin", PLUGIN_NAME, fmt, ##__VA_ARGS__)
#define QARRAY_LOG_WARN(fmt, ...) LOG_WARN("Plugin", PLUGIN_NAME, fmt, ##__VA_ARGS__)
#define QARRAY_LOG_ERROR(fmt, ...) LOG_ERROR("Plugin", PLUGIN_NAME, fmt, ##__VA_ARGS__)

namespace {

struct GateScanConfig {
  std::string gate_name;
  size_t channel = 0;
  double min_v = 0.0;
  double max_v = 0.0;
  int resolution = 0;
  int axis = 0;
};

struct PluginState {
  std::mutex mutex;
  bool initialized = false;
  std::unique_ptr<py::scoped_interpreter> interp;
  std::unique_ptr<falcon::qarray::Device> device;
  std::string config_path;
  std::vector<std::string> gate_names;
  char instrument_name[PLUGIN_MAX_STRING_LEN] = {0};
  std::deque<GateScanConfig> scan_history; // Maintains history of up to 2
};

static PluginState g_plugin_state;

std::string get_gate_name_for_channel(size_t channel_index) {
  // 1-based channel index
  if (channel_index == 0 || channel_index > g_plugin_state.gate_names.size()) {
    return "";
  }
  return g_plugin_state.gate_names[channel_index - 1];
}

} // namespace

extern "C" {

INSTRUMENT_PLUGIN_API PluginMetadata plugin_get_metadata(void) {
  PluginMetadata meta = {};
  meta.api_version = INSTRUMENT_PLUGIN_API_VERSION;
  std::strncpy(meta.name, "QArraySimulator", sizeof(meta.name) - 1);
  std::strncpy(meta.version, "1.0.0", sizeof(meta.version) - 1);
  std::strncpy(meta.protocol_type, "QArray", sizeof(meta.protocol_type) - 1);
  std::strncpy(meta.description,
               "QArray quantum dot simulation instrument plugin",
               sizeof(meta.description) - 1);
  return meta;
}

INSTRUMENT_PLUGIN_API uint8_t plugin_initialize(const PluginConfig *config) {
  std::lock_guard<std::mutex> lock(g_plugin_state.mutex);

  if (config != nullptr) {
    std::strncpy(g_plugin_state.instrument_name, config->instrument_name,
                 sizeof(g_plugin_state.instrument_name) - 1);
  }

  // Parse config_path from custom JSON if present, or custom string directly
  std::string cfg_path = "tests/config/test-qarray-device.yml";
  if (config != nullptr && config->custom[0] != '\0') {
    cJSON *json = cJSON_Parse(config->custom);
    if (json != nullptr) {
      cJSON *item = cJSON_GetObjectItem(json, "config_path");
      if (item != nullptr && cJSON_IsString(item) && item->valuestring != nullptr) {
        cfg_path = item->valuestring;
      }
      cJSON_Delete(json);
    } else {
      // If not JSON, use custom string directly
      cfg_path = config->custom;
    }
  }

  try {
    // Dynamically promote the libpython symbols already linked into this process to RTLD_GLOBAL,
    // so Python C-extensions (like math.so) can resolve PyFloat_Type etc. without crashing.
    Dl_info dlinfo;
    if (dladdr(reinterpret_cast<void *>(&Py_Initialize), &dlinfo) && dlinfo.dli_fname) {
      dlopen(dlinfo.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    }

    if (Py_IsInitialized() == 0) {
      g_plugin_state.interp = std::make_unique<py::scoped_interpreter>();
    }

    g_plugin_state.config_path = cfg_path;
    QARRAY_LOG_INFO("Initializing QArray device with config '%s'...", cfg_path.c_str());
    g_plugin_state.device = std::make_unique<falcon::qarray::Device>(cfg_path);
    g_plugin_state.gate_names = g_plugin_state.device->gate_names();
    g_plugin_state.scan_history.clear();
    g_plugin_state.initialized = true;

    QARRAY_LOG_INFO("QArray plugin initialized with config '%s', %zu gates found",
                    cfg_path.c_str(), g_plugin_state.gate_names.size());
    return 0;
  } catch (const py::error_already_set &e) {
    QARRAY_LOG_ERROR("Python exception during QArray device init: %s", e.what());
    return 1;
  } catch (const std::exception &e) {
    QARRAY_LOG_ERROR("Failed to initialize QArray device: %s", e.what());
    return 1;
  } catch (...) {
    QARRAY_LOG_ERROR("Unknown exception during QArray device init");
    return 1;
  }
}

static uint8_t execute_scan_internal(bool differential, PluginResponse *resp) {
  if (g_plugin_state.scan_history.empty()) {
    QARRAY_LOG_ERROR("Scan requested but no scan configuration set");
    return 1;
  }

  std::vector<double> stream_data;
  const char *scan_verb = differential ? "DIFFERENTIAL_SCAN" : "SCAN";

  if (g_plugin_state.scan_history.size() == 1) {
    // 1D scan on a single gate
    const auto &cfg = g_plugin_state.scan_history.back();
    auto result = g_plugin_state.device->scan_1d(cfg.gate_name, {cfg.min_v, cfg.max_v}, cfg.resolution);
    if (differential && result.has_sensor && !result.differentiated_signal.empty()) {
      stream_data = std::move(result.differentiated_signal);
    } else if (result.has_sensor && !result.sensor_output.empty()) {
      stream_data = std::move(result.sensor_output);
    } else {
      stream_data.assign(cfg.resolution, 0.0);
    }
  } else {
    // We have 2 configurations in history
    const auto &cfg1 = g_plugin_state.scan_history[0];
    const auto &cfg2 = g_plugin_state.scan_history[1];

    if (cfg1.axis != cfg2.axis) {
      // 2D Scan across 2 orthogonal axes (e.g. axis 0 and axis 1)
      const auto &x_cfg = (cfg1.axis == 0) ? cfg1 : cfg2;
      const auto &y_cfg = (cfg1.axis == 1) ? cfg1 : cfg2;
      int res = x_cfg.resolution; // Assuming resolution from x or max
      auto result = g_plugin_state.device->scan_2d(
          x_cfg.gate_name, y_cfg.gate_name,
          {x_cfg.min_v, x_cfg.max_v},
          {y_cfg.min_v, y_cfg.max_v},
          res);

      if (differential && result.has_sensor && !result.differentiated_signal.empty()) {
        stream_data = std::move(result.differentiated_signal);
      } else if (result.has_sensor && !result.sensor_output.empty()) {
        stream_data = std::move(result.sensor_output);
      } else {
        stream_data.assign(res * res, 0.0);
      }
    } else {
      // Ray Scan: both gates vary simultaneously along the same axis (e.g. axis 0)
      std::map<std::string, double> start_map;
      std::map<std::string, double> end_map;
      start_map[cfg1.gate_name] = cfg1.min_v;
      end_map[cfg1.gate_name] = cfg1.max_v;
      start_map[cfg2.gate_name] = cfg2.min_v;
      end_map[cfg2.gate_name] = cfg2.max_v;

      int res = cfg2.resolution > 0 ? cfg2.resolution : cfg1.resolution;
      auto result = g_plugin_state.device->scan_ray(start_map, end_map, res);

      if (differential && result.has_sensor && !result.differentiated_signal.empty()) {
        stream_data = std::move(result.differentiated_signal);
      } else if (result.has_sensor && !result.sensor_output.empty()) {
        stream_data = std::move(result.sensor_output);
      } else {
        stream_data.assign(res, 0.0);
      }
    }
  }

  const char *buf_id = data_manager_create_buffer(
      "qarray", scan_verb, INST_DATA_FLOAT64, stream_data.size(), stream_data.data());
  if (!buf_id) {
    QARRAY_LOG_ERROR("Failed to create data buffer for %s", scan_verb);
    return 1;
  }

  Variable out = {};
  out.type = PARAM_TYPE_BUFFER;
  std::strncpy(out.name, "output_stream", PLUGIN_MAX_STRING_LEN - 1);
  std::strncpy(out.value.str_val, buf_id, PLUGIN_MAX_STRING_LEN - 1);
  plugin_response_push(resp, &out);
  return 0;
}

INSTRUMENT_PLUGIN_API uint8_t plugin_execute_command(const PluginCommand *cmd,
                                                     PluginResponse *resp) {
  std::lock_guard<std::mutex> lock(g_plugin_state.mutex);

  if (!g_plugin_state.initialized || !g_plugin_state.device) {
    QARRAY_LOG_ERROR("Plugin executed while not initialized");
    return 1;
  }

  uint8_t p_count = param_storage_count(cmd->params);

  // Helper lambda to find param by name
  auto get_param = [&](const char *name) -> const Variable * {
    for (uint8_t i = 0; i < p_count; i++) {
      const Variable *var = param_storage_get(cmd->params, i);
      if (var && std::strncmp(var->name, name, PLUGIN_MAX_STRING_LEN) == 0) {
        return var;
      }
    }
    return nullptr;
  };

  if (std::strcmp(cmd->command, "SET_VOLTAGE") == 0) {
    const Variable *v_channel = get_param("source");
    if (!v_channel) {
      v_channel = get_param("channel");
    }
    const Variable *v_volt = get_param("voltage");
    if (!v_channel || !v_volt) {
      QARRAY_LOG_ERROR("SET_VOLTAGE missing channel (source) or voltage parameter");
      return 1;
    }
    int64_t ch = v_channel->value.i64_val;
    double volt = v_volt->value.d_val;
    std::string gate = get_gate_name_for_channel(static_cast<size_t>(ch));
    if (gate.empty()) {
      QARRAY_LOG_ERROR("Channel %ld out of bounds for gate list", ch);
      return 1;
    }
    g_plugin_state.device->set_voltage(gate, volt);
    QARRAY_LOG_DEBUG("SET_VOLTAGE gate %s (channel %ld) = %f", gate.c_str(), ch, volt);
    return 0;
  }

  if (std::strcmp(cmd->command, "GET_VOLTAGE") == 0) {
    const Variable *v_channel = get_param("source");
    if (!v_channel) {
      v_channel = get_param("channel");
    }
    if (!v_channel) {
      QARRAY_LOG_ERROR("GET_VOLTAGE missing channel (source) parameter");
      return 1;
    }
    int64_t ch = v_channel->value.i64_val;
    std::string gate = get_gate_name_for_channel(static_cast<size_t>(ch));
    if (gate.empty()) {
      QARRAY_LOG_ERROR("Channel %ld out of bounds for gate list", ch);
      return 1;
    }
    auto volts = g_plugin_state.device->voltages();
    double volt = volts.count(gate) ? volts.at(gate) : 0.0;

    Variable out = {};
    out.type = PARAM_TYPE_DOUBLE;
    std::strncpy(out.name, "voltage", PLUGIN_MAX_STRING_LEN - 1);
    out.value.d_val = volt;
    plugin_response_push(resp, &out);
    return 0;
  }

  if (std::strcmp(cmd->command, "CONFIGURE_SCAN") == 0) {
    const Variable *v_channel = get_param("source");
    if (!v_channel) {
      v_channel = get_param("channel");
    }
    const Variable *v_min = get_param("min");
    const Variable *v_max = get_param("max");
    const Variable *v_res = get_param("resolution");
    const Variable *v_axis = get_param("axis");

    if (!v_channel || !v_min || !v_max || !v_res) {
      QARRAY_LOG_ERROR("CONFIGURE_SCAN missing required parameters");
      return 1;
    }

    int64_t ch = v_channel->value.i64_val;
    std::string gate = get_gate_name_for_channel(static_cast<size_t>(ch));
    if (gate.empty()) {
      QARRAY_LOG_ERROR("Channel %ld out of bounds for gate list", ch);
      return 1;
    }

    GateScanConfig cfg;
    cfg.gate_name = gate;
    cfg.channel = static_cast<size_t>(ch);
    cfg.min_v = v_min->value.d_val;
    cfg.max_v = v_max->value.d_val;
    cfg.resolution = static_cast<int>(v_res->value.i64_val);
    cfg.axis = v_axis ? static_cast<int>(v_axis->value.i64_val) : 0;

    if (g_plugin_state.scan_history.size() >= 2) {
      g_plugin_state.scan_history.pop_front();
    }
    g_plugin_state.scan_history.push_back(cfg);
    QARRAY_LOG_INFO("CONFIGURE_SCAN for %s (ch %ld): [%f, %f], res=%d, axis=%d (history size=%zu)",
                    gate.c_str(), ch, cfg.min_v, cfg.max_v, cfg.resolution, cfg.axis,
                    g_plugin_state.scan_history.size());
    return 0;
  }

  if (std::strcmp(cmd->command, "DIFFERENTIAL_SCAN") == 0) {
    return execute_scan_internal(/*differential=*/true, resp);
  }

  if (std::strcmp(cmd->command, "SCAN") == 0) {
    return execute_scan_internal(/*differential=*/false, resp);
  }

  if (std::strcmp(cmd->command, "RESET") == 0) {
    if (g_plugin_state.device) {
      for (const auto &gate : g_plugin_state.gate_names) {
        g_plugin_state.device->set_voltage(gate, 0.0);
      }
    }
    g_plugin_state.scan_history.clear();
    return 0;
  }

  QARRAY_LOG_WARN("Unknown command: %s", cmd->command);
  return 1;
}

INSTRUMENT_PLUGIN_API void plugin_shutdown(void) {
  std::lock_guard<std::mutex> lock(g_plugin_state.mutex);
  g_plugin_state.scan_history.clear();
  g_plugin_state.device.reset();
  g_plugin_state.interp.reset();
  g_plugin_state.initialized = false;
  QARRAY_LOG_INFO("QArray plugin shutdown complete");
}

} // extern "C"
