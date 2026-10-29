#include "falcon-qarray-device/Device.hpp"
#include <cjson/cJSON.h>
#include <instrument-data.h>
#include <instrument-log/inst_logging.h>
#include <instrument-plugin.h>
#include <plugin-api.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
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

struct PluginState {
  std::mutex mutex;
  bool initialized = false;
  std::unique_ptr<py::scoped_interpreter> interp;
  std::unique_ptr<falcon::qarray::Device> device;
  std::string config_path;
  std::vector<std::string> gate_names;
  char instrument_name[PLUGIN_MAX_STRING_LEN] = {0};
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
    // Dynamically load libpython symbols globally so Python C-extensions (like math.so)
    // can resolve PyFloat_Type etc. when instrument-worker dynamically loads this plugin.
    dlopen("libpython3.12.so.1.0", RTLD_NOW | RTLD_GLOBAL);

    if (Py_IsInitialized() == 0) {
      g_plugin_state.interp = std::make_unique<py::scoped_interpreter>();
    }

    g_plugin_state.config_path = cfg_path;
    QARRAY_LOG_INFO("Initializing QArray device with config '%s'...", cfg_path.c_str());
    g_plugin_state.device = std::make_unique<falcon::qarray::Device>(cfg_path);
    g_plugin_state.gate_names = g_plugin_state.device->gate_names();
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
    std::strncpy(out.name, "measured_voltage", PLUGIN_MAX_STRING_LEN - 1);
    out.value.d_val = volt;
    plugin_response_push(resp, &out);
    return 0;
  }

  if (std::strcmp(cmd->command, "GET_SENSOR_SIGNAL") == 0) {
    const Variable *v_channel = get_param("sensor");
    if (!v_channel) {
      v_channel = get_param("channel");
    }
    // Read instantaneous sensor signal for current gate voltages
    auto gate_names = g_plugin_state.gate_names;
    std::string probe_gate = gate_names.empty() ? "P1" : gate_names[0];
    auto current_volts = g_plugin_state.device->voltages();
    double current_v = current_volts.count(probe_gate) ? current_volts.at(probe_gate) : 0.0;

    // 1D single point / tiny window around current voltage
    auto res = g_plugin_state.device->scan_1d(probe_gate, {current_v - 0.001, current_v + 0.001}, 2);
    double signal = 0.0;
    if (res.has_sensor && !res.sensor_output.empty()) {
      signal = res.sensor_output[0];
    }

    Variable out = {};
    out.type = PARAM_TYPE_DOUBLE;
    std::strncpy(out.name, "signal", PLUGIN_MAX_STRING_LEN - 1);
    out.value.d_val = signal;
    plugin_response_push(resp, &out);
    return 0;
  }

  if (std::strcmp(cmd->command, "SCAN_1D") == 0) {
    const Variable *v_channel = get_param("channel");
    const Variable *v_min = get_param("x_min");
    const Variable *v_max = get_param("x_max");
    const Variable *v_res = get_param("resolution");

    if (!v_channel || !v_min || !v_max || !v_res) {
      QARRAY_LOG_ERROR("SCAN_1D missing required parameters");
      return 1;
    }

    std::string gate = get_gate_name_for_channel(static_cast<size_t>(v_channel->value.i64_val));
    double min_v = v_min->value.d_val;
    double max_v = v_max->value.d_val;
    int resolution = static_cast<int>(v_res->value.i64_val);

    auto result = g_plugin_state.device->scan_1d(gate, {min_v, max_v}, resolution);
    std::vector<double> stream_data;
    if (result.has_sensor && !result.differentiated_signal.empty()) {
      stream_data = result.differentiated_signal;
    } else if (result.has_sensor && !result.sensor_output.empty()) {
      stream_data = result.sensor_output;
    } else {
      stream_data.assign(resolution, 0.0);
    }

    const char *buf_id = data_manager_create_buffer(
        "qarray", "SCAN_1D", INST_DATA_FLOAT64, stream_data.size(), stream_data.data());
    if (!buf_id) {
      QARRAY_LOG_ERROR("Failed to create data buffer for 1D scan");
      return 1;
    }

    Variable out = {};
    out.type = PARAM_TYPE_BUFFER;
    std::strncpy(out.name, "output_stream", PLUGIN_MAX_STRING_LEN - 1);
    std::strncpy(out.value.str_val, buf_id, PLUGIN_MAX_STRING_LEN - 1);
    plugin_response_push(resp, &out);
    return 0;
  }

  if (std::strcmp(cmd->command, "SCAN_2D") == 0) {
    const Variable *v_xgate = get_param("x_gate");
    const Variable *v_ygate = get_param("y_gate");
    const Variable *v_xmin = get_param("x_min");
    const Variable *v_xmax = get_param("x_max");
    const Variable *v_ymin = get_param("y_min");
    const Variable *v_ymax = get_param("y_max");
    const Variable *v_res = get_param("resolution");

    if (!v_xgate || !v_ygate || !v_xmin || !v_xmax || !v_ymin || !v_ymax || !v_res) {
      QARRAY_LOG_ERROR("SCAN_2D missing required parameters");
      return 1;
    }

    std::string x_gate = v_xgate->value.str_val;
    std::string y_gate = v_ygate->value.str_val;
    double x_min = v_xmin->value.d_val;
    double x_max = v_xmax->value.d_val;
    double y_min = v_ymin->value.d_val;
    double y_max = v_ymax->value.d_val;
    int resolution = static_cast<int>(v_res->value.i64_val);

    auto result = g_plugin_state.device->scan_2d(x_gate, y_gate, {x_min, x_max}, {y_min, y_max}, resolution);
    std::vector<double> stream_data;
    if (result.has_sensor && !result.differentiated_signal.empty()) {
      stream_data = result.differentiated_signal;
    } else if (result.has_sensor && !result.sensor_output.empty()) {
      stream_data = result.sensor_output;
    } else {
      stream_data.assign(resolution * resolution, 0.0);
    }

    const char *buf_id = data_manager_create_buffer(
        "qarray", "SCAN_2D", INST_DATA_FLOAT64, stream_data.size(), stream_data.data());
    if (!buf_id) {
      QARRAY_LOG_ERROR("Failed to create data buffer for 2D scan");
      return 1;
    }

    Variable out = {};
    out.type = PARAM_TYPE_BUFFER;
    std::strncpy(out.name, "output_stream", PLUGIN_MAX_STRING_LEN - 1);
    std::strncpy(out.value.str_val, buf_id, PLUGIN_MAX_STRING_LEN - 1);
    plugin_response_push(resp, &out);
    return 0;
  }

  if (std::strcmp(cmd->command, "RESET") == 0) {
    if (g_plugin_state.device) {
      for (const auto &gate : g_plugin_state.gate_names) {
        g_plugin_state.device->set_voltage(gate, 0.0);
      }
    }
    return 0;
  }

  QARRAY_LOG_WARN("Unknown command: {}", cmd->command);
  return 1;
}

INSTRUMENT_PLUGIN_API void plugin_shutdown(void) {
  std::lock_guard<std::mutex> lock(g_plugin_state.mutex);
  g_plugin_state.device.reset();
  g_plugin_state.interp.reset();
  g_plugin_state.initialized = false;
  QARRAY_LOG_INFO("QArray plugin shutdown complete");
}

} // extern "C"
