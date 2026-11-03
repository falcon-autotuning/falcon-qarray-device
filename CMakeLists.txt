cmake_minimum_required(VERSION 3.20)
project(
  falcon-qarray-device
  VERSION 1.1.1
  DESCRIPTION "A simulation wrapper and instrument plugin for QArray quantum dot simulations."
  HOMEPAGE_URL "https://github.com/falcon-autotuning/falcon-qarray-device"
  LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
if(UNIX AND NOT APPLE)
  add_link_options("-Wl,--disable-new-dtags")
endif()

# ── Build Options ─────────────────────────────────────────────────────────────
option(BUILD_TESTS   "Build tests"                          ON)
option(BUILD_PLUGINS "Build instrument plugins"             ON)
option(USE_CCACHE    "Enable ccache or sccache if available" ON)
option(ENABLE_PCH    "Enable precompiled headers"           ON)

# ── Compiler cache ───────────────────────────────────────────────────────────
if(USE_CCACHE)
  find_program(SCCACHE_PROGRAM sccache)
  if(SCCACHE_PROGRAM)
    message(STATUS "Using sccache: ${SCCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   ${SCCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
  else()
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
      message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
      set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    else()
      message(STATUS "No ccache/sccache found — building normally.")
    endif()
  endif()
endif()

# ── Find Python ───────────────────────────────────────────────────────────────
if(DEFINED Python3_EXECUTABLE)
  message(STATUS "Python3_EXECUTABLE explicitly set to: ${Python3_EXECUTABLE}")
  set(Python3_FIND_STRATEGY LOCATION)
  set(Python3_FIND_REGISTRY NEVER)
  set(Python3_FIND_FRAMEWORK NEVER)
endif()

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
message(STATUS "Python3 interpreter : ${Python3_EXECUTABLE}")
message(STATUS "Python3 version     : ${Python3_VERSION}")
message(STATUS "Python3 include dirs: ${Python3_INCLUDE_DIRS}")

find_package(pybind11 CONFIG REQUIRED)
message(STATUS "Found pybind11: ${pybind11_VERSION}")

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c
    "import sys; print([p for p in sys.path if 'site-packages' in p][0])"
  OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _site_result
)
if(NOT _site_result EQUAL 0 OR PYTHON_SITE_PACKAGES STREQUAL "")
  message(WARNING "Could not detect Python site-packages dir from ${Python3_EXECUTABLE}")
  set(PYTHON_SITE_PACKAGES "")
endif()
message(STATUS "Python3 site-packages: ${PYTHON_SITE_PACKAGES}")

include(GNUInstallDirs)
set(PYTHON_MODULE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/falcon-qarray-device")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/PythonModulePath.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/PythonModulePath.hpp"
  @ONLY
)

# ── Core library: falcon-qarray-device ────────────────────────────────────────
add_library(falcon-qarray-device SHARED
  src/Device.cpp
)

target_include_directories(falcon-qarray-device
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(falcon-qarray-device
  PUBLIC
    pybind11::embed
)

target_compile_options(falcon-qarray-device PRIVATE -Wall -Wextra)

# ── Instrument Dependencies ──────────────────────────────────────────────────
find_package(cJSON CONFIG REQUIRED)
find_package(instrument-data CONFIG REQUIRED)
find_package(instrument-plugin-api CONFIG REQUIRED)
find_package(instrument-log CONFIG REQUIRED)

# ── Instrument Plugin ─────────────────────────────────────────────────────────
if(BUILD_PLUGINS)
  add_library(qarray_plugin MODULE
    plugin/plugin.cpp
  )

  target_include_directories(qarray_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  )

  target_link_libraries(qarray_plugin PRIVATE
    falcon-qarray-device
    instrument-plugin-api::plugin
    instrument-data::instrument-data
    instrument-log::instrument-log
    cjson
    pybind11::embed
  )

  set_target_properties(qarray_plugin PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "qarray_plugin"
  )

  install(TARGETS qarray_plugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/plugins
  )
endif()

# ── Tests & Embed Bundle ─────────────────────────────────────────────────────
if(BUILD_TESTS)
  enable_testing()

  find_package(GTest CONFIG REQUIRED)
  find_package(isa-test-utils CONFIG REQUIRED)
  find_package(cmocka CONFIG REQUIRED)

  # C++ GTest unit tests
  set(FALCON_DEV_PYTHON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")
  set(FALCON_DEV_SITE_PACKAGES "${PYTHON_SITE_PACKAGES}")

  add_executable(falcon-qarray-device_tests
    tests/test_device.cpp
  )
  target_include_directories(falcon-qarray-device_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(falcon-qarray-device_tests PRIVATE
    falcon-qarray-device
    GTest::gtest
    pybind11::embed
  )
  include(GoogleTest)
  gtest_discover_tests(falcon-qarray-device_tests
    PROPERTIES
      ENVIRONMENT
        "FALCON_QARRAY_PYTHON_PATH=${FALCON_DEV_PYTHON_PATH};FALCON_QARRAY_VENV_SITE_PACKAGES=${FALCON_DEV_SITE_PACKAGES}"
  )

  # ISA Test Utils Bundle & cmocka integration test
  if(BUILD_PLUGINS)
    file(GLOB_RECURSE ISA_FILES    "${CMAKE_SOURCE_DIR}/isa/*")
    file(GLOB_RECURSE CONFIG_FILES "${CMAKE_SOURCE_DIR}/tests/config/*")

    set(GENERATED_INCLUDE_DIR
      ${CMAKE_CURRENT_BINARY_DIR}/include/falcon-qarray-device)
    file(MAKE_DIRECTORY ${GENERATED_INCLUDE_DIR})

    set(EMBED_HEADER ${GENERATED_INCLUDE_DIR}/embedded_bundle.h)
    isa_test_utils_embed_bundle(
      OUTPUT ${EMBED_HEADER}
      ISA_FILES ${ISA_FILES}
      CONFIG_FILES ${CONFIG_FILES}
    )
    add_custom_target(embed_bundle ALL DEPENDS ${EMBED_HEADER})

    add_executable(qarray-instrument-test
      tests/test_plugin.c
    )
    add_dependencies(qarray-instrument-test embed_bundle qarray_plugin)

    target_compile_definitions(qarray-instrument-test PRIVATE
      BUILD_PLUGIN_PATH="$<TARGET_FILE:qarray_plugin>"
      TEST_DEVICE_CONFIG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/config/test-qarray-device.yml"
    )

    target_include_directories(qarray-instrument-test PRIVATE
      ${CMAKE_CURRENT_BINARY_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(qarray-instrument-test PRIVATE
      instrument-plugin-api::host
      instrument-plugin-api::plugin
      instrument-data::instrument-data
      isa-test-utils::isa-test-utils
      cmocka::cmocka
    )

    add_test(NAME qarray-instrument-test COMMAND qarray-instrument-test)

    set(PLUGIN_SEARCH_PATHS
      "${CMAKE_BINARY_DIR}"
      "${CMAKE_BINARY_DIR}/lib"
    )
    if(WIN32)
      set(PATH_SEPARATOR ";")
    else()
      set(PATH_SEPARATOR ":")
    endif()
    string(REPLACE ";" "${PATH_SEPARATOR}"
           PLUGIN_SEARCH_PATH_ENV "${PLUGIN_SEARCH_PATHS}")
    set(VCPKG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-linux-dynamic/lib")

    set_tests_properties(qarray-instrument-test PROPERTIES
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      ENVIRONMENT "PATH=${PLUGIN_SEARCH_PATH_ENV}${PATH_SEPARATOR}$ENV{PATH};LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}${PATH_SEPARATOR}${VCPKG_LIB_DIR}${PATH_SEPARATOR}$ENV{LD_LIBRARY_PATH};FALCON_QARRAY_PYTHON_PATH=${FALCON_DEV_PYTHON_PATH};FALCON_QARRAY_VENV_SITE_PACKAGES=${FALCON_DEV_SITE_PACKAGES}"
    )
  endif()
endif()

# ── Installation rules ────────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)

install(TARGETS falcon-qarray-device
  EXPORT   falcon-qarray-device-targets
  LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon-qarray-device
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
    PATTERN "*.hpp"
    PATTERN "*.h"
)

install(FILES src/device.py
  DESTINATION ${PYTHON_MODULE_INSTALL_DIR}
)

install(DIRECTORY isa/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/falcon-qarray-device/isa
  FILES_MATCHING PATTERN "*.yml"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/falcon-qarray-device-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

install(EXPORT falcon-qarray-device-targets
  FILE      falcon-qarray-device-targets.cmake
  NAMESPACE falcon::
  DESTINATION ${CMAKE_INSTALL_DATADIR}/falcon-qarray-device
)

configure_package_config_file(
  cmake/falcon-qarray-device-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/falcon-qarray-device-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_DATADIR}/falcon-qarray-device
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/falcon-qarray-device-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/falcon-qarray-device-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/falcon-qarray-device
)
