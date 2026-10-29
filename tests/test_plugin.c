#include "falcon-qarray-device/embedded_bundle.h"
#include <cmocka.h>
#include <isa-test-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ISA_TEST_UTILS_EXPORT Path *path_new(const char *initial_path);

#define ISA_PATH "../../../isa/isa/qarray.yml"
#define PLUGIN_OBJ "libqarray_plugin.so"

static const char *kInstrumentName = "QARRAY";

typedef struct {
  EnvLocations env;
  Path *config_path;
  Path *plugin_path;
} TestContext;

static int global_group_setup(void **state) {
  TestContext *ctx = calloc(1, sizeof(TestContext));
  if (!ctx)
    return -1;

  ctx->env = prepare_environment(
      "__API_REF__", ISA_PATH,
      "__DEVICE_CONFIG_PATH__", TEST_DEVICE_CONFIG_PATH,
      NULL);

  ctx->config_path = path_clone(ctx->env.config_dir);
  path_push(ctx->config_path, "tests");
  path_push(ctx->config_path, "config");
  path_push(ctx->config_path, "my-qarray.yml");

  ctx->plugin_path = path_new(BUILD_PLUGIN_PATH);

  *state = ctx;
  return 0;
}

static int global_group_teardown(void **state) {
  TestContext *ctx = (TestContext *)*state;
  if (!ctx)
    return 0;
  cleanup_environment(&ctx->env);
  path_free(ctx->config_path);
  path_free(ctx->plugin_path);
  free(ctx);
  *state = NULL;
  return 0;
}

static int test_setup_server_fixture(void **state) {
  TestContext *ctx = (TestContext *)*state;
  assert_non_null((void *)ctx);
  start_server(TEST_LOG_TRACE);
  start_instrument(ctx->config_path, ctx->plugin_path, TEST_LOG_TRACE);
  return 0;
}

static int test_teardown_server_fixture(void **state) {
  (void)state;
  stop_instrument(kInstrumentName);
  stop_server();
  sleep(1);
  return 0;
}

static void test_startup(void **state) {
  (void)state;
  char *status = instrument_status(kInstrumentName);
  assert_non_null((void *)status);
  assert_string_not_equal(status, "");
  printf("%s status:\n%s\n", kInstrumentName, status);
  free(status);
}

static void test_set_and_get_voltage(void **state) {
  (void)state;
  const char *script =
      "function main(ctx)\n"
      "  ctx:log(\"Setting channel 1 voltage to 0.42V\")\n"
      "  local c1 = instrument_call_stack.new({\n"
      "    instrument = \"QARRAY\",\n"
      "    command = \"SET_VOLTAGE\",\n"
      "    channel_group = \"source\",\n"
      "    channel = 1,\n"
      "  })\n"
      "  ctx:call(c1, 0.42)\n"
      "  local c2 = instrument_call_stack.new({\n"
      "    instrument = \"QARRAY\",\n"
      "    command = \"GET_VOLTAGE\",\n"
      "    channel_group = \"source\",\n"
      "    channel = 1,\n"
      "  })\n"
      "  local res = ctx:call(c2)\n"
      "  ctx:log(string.format(\"Measured voltage: %f\", res[1]:value()))\n"
      "  assert(math.abs(res[1]:value() - 0.42) < 1e-4, \"Voltage mismatch\")\n"
      "end\n";

  Map *map = map_new();
  const Result *output = perform_measurement(script, map);

  assert_non_null(output);
  assert_string_equal(output->status, "success");

  free_result(output);
  map_free(map);
}

static void test_get_sensor_signal(void **state) {
  (void)state;
  const char *script =
      "function main(ctx)\n"
      "  ctx:log(\"Reading sensor signal\")\n"
      "  local c1 = instrument_call_stack.new({\n"
      "    instrument = \"QARRAY\",\n"
      "    command = \"GET_SENSOR_SIGNAL\",\n"
      "    channel_group = \"sensor\",\n"
      "    channel = 1,\n"
      "  })\n"
      "  local res = ctx:call(c1)\n"
      "  ctx:log(string.format(\"Sensor signal: %f\", res[1]:value()))\n"
      "end\n";

  Map *map = map_new();
  const Result *output = perform_measurement(script, map);

  assert_non_null(output);
  assert_string_equal(output->status, "success");

  free_result(output);
  map_free(map);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const struct CMUnitTest tests[] = {
      cmocka_unit_test_setup_teardown(test_startup, test_setup_server_fixture,
                                      test_teardown_server_fixture),
      cmocka_unit_test_setup_teardown(test_set_and_get_voltage, test_setup_server_fixture,
                                      test_teardown_server_fixture),
      cmocka_unit_test_setup_teardown(test_get_sensor_signal, test_setup_server_fixture,
                                      test_teardown_server_fixture),
  };

  return cmocka_run_group_tests(tests, global_group_setup,
                                global_group_teardown);
}
