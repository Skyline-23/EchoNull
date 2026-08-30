cmake_minimum_required(VERSION 3.24)

project(EchoNull VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "EchoNull currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ECHONULL_BUILD_TESTS "Build EchoNull unit tests" ON)
set(AFX_SDK_ROOT "" CACHE PATH "Root of the NVIDIA Audio Effects SDK")

if(NOT AFX_SDK_ROOT AND DEFINED ENV{AFX_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{AFX_SDK_ROOT}" AFX_SDK_ROOT)
endif()

find_path(ECHONULL_NVAFX_INCLUDE_DIR
  NAMES nvAudioEffects.h
  HINTS "${AFX_SDK_ROOT}/include"
  NO_DEFAULT_PATH)
find_library(ECHONULL_NVAFX_LIBRARY
  NAMES NVAudioEffects
  HINTS "${AFX_SDK_ROOT}/lib"
  NO_DEFAULT_PATH)

set(ECHONULL_HAS_NVAFX 0)
if(ECHONULL_NVAFX_INCLUDE_DIR AND ECHONULL_NVAFX_LIBRARY)
  set(ECHONULL_HAS_NVAFX 1)
  message(STATUS "NVIDIA AFX SDK found at ${AFX_SDK_ROOT}")
else()
  message(WARNING
    "NVIDIA AFX SDK was not found. Device listing and tests will build, but "
    "AEC run/validate commands will report that the SDK is unavailable. "
    "Configure with -DAFX_SDK_ROOT=<path> after installing AFX SDK 2.1 or newer.")
endif()

add_library(echonull_domain INTERFACE)
target_include_directories(echonull_domain INTERFACE src)

add_library(echonull_application
  src/application/delay_estimator.cpp
  src/application/erle_meter.cpp
  src/application/sample_queue.cpp
  src/application/timestamped_audio_buffer.cpp)

target_link_libraries(echonull_application PUBLIC echonull_domain)
target_include_directories(echonull_application PUBLIC src)
target_compile_definitions(echonull_application PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(echonull_application PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_library(echonull_infrastructure
  src/infrastructure/config.cpp
  src/infrastructure/wav.cpp
  src/infrastructure/windows/device_manager.cpp)

target_link_libraries(echonull_infrastructure PUBLIC echonull_domain PRIVATE ole32 uuid)
target_include_directories(echonull_infrastructure PUBLIC src)
target_compile_definitions(echonull_infrastructure PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(echonull_infrastructure PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_executable(echonull
  src/main.cpp
  src/infrastructure/windows/wasapi_capture.cpp
  src/infrastructure/windows/wasapi_renderer.cpp
  src/infrastructure/nvidia/nvafx_aec.cpp
  src/application/engine.cpp)

target_include_directories(echonull PRIVATE src)
target_compile_definitions(echonull PRIVATE ECHONULL_HAS_NVAFX=${ECHONULL_HAS_NVAFX})
target_link_libraries(echonull PRIVATE echonull_application echonull_infrastructure ole32 uuid avrt)

if(ECHONULL_HAS_NVAFX)
  target_include_directories(echonull PRIVATE "${ECHONULL_NVAFX_INCLUDE_DIR}")
  target_link_libraries(echonull PRIVATE "${ECHONULL_NVAFX_LIBRARY}")
endif()

if(MSVC)
  target_compile_options(echonull PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

if(ECHONULL_BUILD_TESTS)
  include(CTest)
  add_executable(echonull_tests
    tests/core_tests.cpp)
  target_link_libraries(echonull_tests PRIVATE echonull_application echonull_infrastructure)
  if(MSVC)
    target_compile_options(echonull_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()
  add_test(NAME echonull_core_tests COMMAND echonull_tests)
endif()
