cmake_minimum_required(VERSION 3.16)
project(homevideo_device CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HV_BUILD_TESTS "Build the device unit tests" ON)
set(HV_PLATFORM "desktop" CACHE STRING "Target platform: desktop or v831")

find_package(Threads REQUIRED)

# Everything that does not touch hardware. Built for both targets.
add_library(hv_core STATIC
  src/log.cpp
  src/clock.cpp
  src/config.cpp
  src/recorder.cpp
  src/spool.cpp
  src/hub_client.cpp
  src/events.cpp
  src/health.cpp
  src/uploader.cpp
  src/pipeline.cpp
  src/supervisor.cpp
  src/app.cpp
  src/platform/desktop_camera.cpp
  src/platform/desktop_encoder.cpp
  src/platform/desktop_detector.cpp
  src/platform/v831_camera.cpp
  src/platform/v831_encoder.cpp
  src/platform/v831_detector.cpp
  src/platform/factory.cpp
)

target_include_directories(hv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hv_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM so vendored headers do not emit warnings under -Wall -Wextra.
target_include_directories(hv_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_options(hv_core PRIVATE -Wall -Wextra)
target_link_libraries(hv_core PUBLIC Threads::Threads)

if(HV_PLATFORM STREQUAL "v831")
  target_compile_definitions(hv_core PUBLIC HV_PLATFORM_V831)
endif()

if(HV_BUILD_TESTS)
  enable_testing()
  file(GLOB HV_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
  add_executable(hv_tests ${HV_TEST_SOURCES})
  target_link_libraries(hv_tests PRIVATE hv_core)
  target_include_directories(hv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(hv_tests PRIVATE -Wall -Wextra)
  add_test(NAME hv_tests COMMAND hv_tests)
endif()

add_executable(hv_device src/main.cpp)
target_link_libraries(hv_device PRIVATE hv_core)
target_compile_options(hv_device PRIVATE -Wall -Wextra)

# The end-to-end test drives the real Python hub, so it stays opt-in: the unit
# tests above must remain runnable with nothing but a C++ compiler.
option(HV_E2E "Register the device<->hub end-to-end test (needs the hub venv)" OFF)
if(HV_BUILD_TESTS AND HV_E2E)
  add_test(NAME device_hub_e2e
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/e2e_with_hub.sh)
  set_tests_properties(device_hub_e2e PROPERTIES TIMEOUT 240)
endif()
