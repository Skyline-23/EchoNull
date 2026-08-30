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

include(FetchContent)
FetchContent_Declare(vst2sdk
  GIT_REPOSITORY https://github.com/Xaymar/vst2sdk.git
  GIT_TAG 339d4f31590bf77c0d0d248e09a380ac6285e069
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vst2sdk)

if(NOT AFX_SDK_ROOT AND DEFINED ENV{AFX_SDK_ROOT})
  file(TO_CMAKE_PATH "$ENV{AFX_SDK_ROOT}" AFX_SDK_ROOT)
endif()

find_path(ECHONULL_NVAFX_INCLUDE_DIR
  NAMES nvAudioEffects.h
  HINTS "${AFX_SDK_ROOT}/include"
  NO_DEFAULT_PATH)
find_path(ECHONULL_NVAFX_AEC_INCLUDE_DIR
  NAMES nvAFXAec.h
  HINTS "${AFX_SDK_ROOT}/features/nvafxaec/include"
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
    "NVIDIA AFX SDK was not found. The plug-in and tests will build, but "
    "AEC processing will stay in safe bypass mode. "
    "Configure with -DAFX_SDK_ROOT=<path> after installing AFX SDK 2.1 or newer.")
endif()

add_library(echonull_domain INTERFACE)
target_include_directories(echonull_domain INTERFACE src)

add_library(echonull_application
  src/application/delay_estimator.cpp
  src/application/streaming_resampler.cpp
  src/application/timestamped_audio_buffer.cpp)

target_link_libraries(echonull_application PUBLIC echonull_domain)
target_include_directories(echonull_application PUBLIC src)
target_compile_definitions(echonull_application PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(echonull_application PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_library(echonull_infrastructure
  src/infrastructure/config.cpp
  src/infrastructure/nvidia/nvafx_aec.cpp
  src/infrastructure/nvidia/nvafx_denoiser.cpp
  src/infrastructure/windows/device_manager.cpp
  src/infrastructure/windows/equalizer_apo.cpp
  src/infrastructure/windows/reference_bus.cpp
  src/infrastructure/windows/telemetry_bus.cpp)

target_link_libraries(echonull_infrastructure PUBLIC echonull_domain echonull_application
  PRIVATE ole32 uuid avrt advapi32)
target_include_directories(echonull_infrastructure PUBLIC src)
target_compile_definitions(echonull_infrastructure PUBLIC
  NOMINMAX WIN32_LEAN_AND_MEAN ECHONULL_HAS_NVAFX=${ECHONULL_HAS_NVAFX})
if(MSVC)
  target_compile_options(echonull_infrastructure PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

if(ECHONULL_HAS_NVAFX)
  target_include_directories(echonull_infrastructure PRIVATE "${ECHONULL_NVAFX_INCLUDE_DIR}")
  if(ECHONULL_NVAFX_AEC_INCLUDE_DIR)
    target_include_directories(echonull_infrastructure PRIVATE "${ECHONULL_NVAFX_AEC_INCLUDE_DIR}")
  endif()
  target_link_libraries(echonull_infrastructure PRIVATE "${ECHONULL_NVAFX_LIBRARY}")
endif()

if(ECHONULL_HAS_NVAFX)
  file(GLOB_RECURSE ECHONULL_NVAFX_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${AFX_SDK_ROOT}/bin/*.dll"
    "${AFX_SDK_ROOT}/features/nvafxaec/bin/*.dll")

  function(echonull_copy_nvafx_runtime target)
    foreach(runtime_dll IN LISTS ECHONULL_NVAFX_RUNTIME_DLLS)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${runtime_dll}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    endforeach()
  endfunction()
endif()

add_library(echonull_plugin MODULE
  src/plugin/plugin_editor.cpp
  src/plugin/plugin_processor.cpp
  src/plugin/vst2_plugin.cpp)
target_include_directories(echonull_plugin PRIVATE
  src
  "${vst2sdk_SOURCE_DIR}/include")
target_link_libraries(echonull_plugin PRIVATE echonull_infrastructure
  comctl32 gdi32 ole32 user32 uxtheme)
set_target_properties(echonull_plugin PROPERTIES
  OUTPUT_NAME "EchoNullPlugin"
  PREFIX "")
if(ECHONULL_HAS_NVAFX)
  echonull_copy_nvafx_runtime(echonull_plugin)
endif()
add_custom_command(TARGET echonull_plugin POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_SOURCE_DIR}/config/plugin.ini"
    "$<TARGET_FILE_DIR:echonull_plugin>/echonull.ini"
  VERBATIM)
if(MSVC)
  target_compile_options(echonull_plugin PRIVATE /W4 /permissive- /utf-8 /EHsc)
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
  add_executable(echonull_plugin_smoke_tests
    tests/plugin_smoke_tests.cpp)
  target_include_directories(echonull_plugin_smoke_tests PRIVATE
    "${vst2sdk_SOURCE_DIR}/include")
  add_dependencies(echonull_plugin_smoke_tests echonull_plugin)
  if(MSVC)
    target_compile_options(echonull_plugin_smoke_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()
  add_test(NAME echonull_plugin_smoke_tests
    COMMAND echonull_plugin_smoke_tests "$<TARGET_FILE:echonull_plugin>")
  add_test(NAME echonull_architecture_boundaries
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_SOURCE_DIR}/tests/architecture_boundaries.ps1"
      -SourceRoot "${CMAKE_SOURCE_DIR}/src")
  if(ECHONULL_HAS_NVAFX)
    echonull_copy_nvafx_runtime(echonull_tests)
  endif()
endif()
