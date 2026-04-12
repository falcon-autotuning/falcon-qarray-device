find_package(GTest     CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLPLOT REQUIRED IMPORTED_TARGET plplot)

add_executable(falcon-qarray-device_tests
  test_device.cpp
  test_visual.cpp
)

target_include_directories(falcon-qarray-device_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(falcon-qarray-device_tests
  PRIVATE
    falcon-qarray-device
    GTest::gtest
    pybind11::embed
    PkgConfig::PLPLOT
)

include(GoogleTest)
gtest_discover_tests(falcon-qarray-device_tests
  PROPERTIES
    ENVIRONMENT
      "FALCON_QARRAY_PYTHON_PATH=${FALCON_DEV_PYTHON_PATH};FALCON_QARRAY_VENV_SITE_PACKAGES=${FALCON_DEV_SITE_PACKAGES}"
)
