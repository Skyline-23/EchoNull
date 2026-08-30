cmake_minimum_required(VERSION 3.24)

project(EchoNull VERSION 0.1.2 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "EchoNull currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(ECHONULL_BUILD_TESTS "Build EchoNull unit tests" ON)
option(ECHONULL_EMBED_NVAFX "Pack the NvAFX runtime and model into EchoNullPlugin.dll" ON)
set(AFX_SDK_ROOT "" CACHE PATH "Root of the NVIDIA Audio Effects SDK")
set(ECHONULL_NVAFX_ARCHITECTURE "blackwell" CACHE STRING
  "NvAFX model architecture to embed (turing, ampere, ada, or blackwell)")

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
set(ECHONULL_HAS_NVAFX 0)
if(ECHONULL_NVAFX_INCLUDE_DIR)
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
  src/infrastructure/nvidia/nvafx_api.cpp
  src/infrastructure/nvidia/nvafx_aec.cpp
  src/infrastructure/nvidia/nvafx_denoiser.cpp
  src/infrastructure/nvidia/packaged_runtime.cpp
  src/infrastructure/text_encoding.cpp
  src/infrastructure/windows/device_manager.cpp
  src/infrastructure/windows/equalizer_apo.cpp
  src/infrastructure/windows/telemetry_bus.cpp
  src/infrastructure/windows/wasapi_format.cpp
  src/infrastructure/windows/wasapi_loopback.cpp)

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
endif()

if(ECHONULL_HAS_NVAFX AND ECHONULL_EMBED_NVAFX)
  file(GLOB_RECURSE ECHONULL_NVAFX_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${AFX_SDK_ROOT}/bin/*.dll"
    "${AFX_SDK_ROOT}/features/nvafx*/bin/*.dll")
  set(ECHONULL_AEC_MODEL
    "${AFX_SDK_ROOT}/features/nvafxaec/models/${ECHONULL_NVAFX_ARCHITECTURE}/aec_48k.trtpkg")
  set(ECHONULL_NOISE_MODEL
    "${AFX_SDK_ROOT}/features/nvafxdenoiser/models/${ECHONULL_NVAFX_ARCHITECTURE}/denoiser_48k.trtpkg")
  set(ECHONULL_NVAFX_LICENSES
    "${AFX_SDK_ROOT}/license/NVIDIA-Software-License-Agreement-2025.05.05.pdf"
    "${AFX_SDK_ROOT}/license/product-specific-terms-for-nvidia-ai-products-2025.05.05.pdf"
    "${AFX_SDK_ROOT}/features/nvafxaec/license/NVIDIA-Models-Community-License-2025-04-15.pdf")
  if(NOT ECHONULL_NVAFX_RUNTIME_DLLS)
    message(FATAL_ERROR "No NvAFX runtime DLLs were found below AFX_SDK_ROOT.")
  endif()
  if(NOT EXISTS "${ECHONULL_AEC_MODEL}")
    message(FATAL_ERROR
      "The ${ECHONULL_NVAFX_ARCHITECTURE} 48 kHz AEC model is missing: ${ECHONULL_AEC_MODEL}")
  endif()

  add_executable(echonull_bundle_packer tools/bundle_packer.cpp)
  target_include_directories(echonull_bundle_packer PRIVATE src)
  target_compile_definitions(echonull_bundle_packer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(echonull_bundle_packer PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()
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
if(ECHONULL_HAS_NVAFX AND ECHONULL_EMBED_NVAFX)
  add_dependencies(echonull_plugin echonull_bundle_packer)
  set(ECHONULL_BUNDLE_ARGUMENTS)
  foreach(runtime_dll IN LISTS ECHONULL_NVAFX_RUNTIME_DLLS)
    get_filename_component(runtime_name "${runtime_dll}" NAME)
    list(APPEND ECHONULL_BUNDLE_ARGUMENTS "${runtime_name}" "${runtime_dll}")
  endforeach()
  list(APPEND ECHONULL_BUNDLE_ARGUMENTS "aec_48k.trtpkg" "${ECHONULL_AEC_MODEL}")
  if(EXISTS "${ECHONULL_NOISE_MODEL}")
    list(APPEND ECHONULL_BUNDLE_ARGUMENTS
      "denoiser_48k.trtpkg" "${ECHONULL_NOISE_MODEL}")
  else()
    message(WARNING
      "The 48 kHz Denoiser model is not installed. Noise Removal will report MODEL MISSING.")
  endif()
  foreach(license_file IN LISTS ECHONULL_NVAFX_LICENSES)
    if(EXISTS "${license_file}")
      get_filename_component(license_name "${license_file}" NAME)
      list(APPEND ECHONULL_BUNDLE_ARGUMENTS "${license_name}" "${license_file}")
    endif()
  endforeach()
  add_custom_command(TARGET echonull_plugin POST_BUILD
    COMMAND "$<TARGET_FILE:echonull_bundle_packer>"
      "$<TARGET_FILE:echonull_plugin>" ${ECHONULL_BUNDLE_ARGUMENTS}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:echonull_plugin>" "${CMAKE_SOURCE_DIR}/EchoNullPlugin.dll"
    COMMENT "Packing NvAFX runtime and models into EchoNullPlugin.dll"
    COMMAND_EXPAND_LISTS
    VERBATIM)
endif()
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
  add_executable(echonull_wasapi_loopback_smoke
    tests/wasapi_loopback_smoke.cpp)
  target_link_libraries(echonull_wasapi_loopback_smoke
    PRIVATE echonull_infrastructure)
  if(MSVC)
    target_compile_options(echonull_wasapi_loopback_smoke
      PRIVATE /W4 /permissive- /utf-8 /EHsc)
  endif()
  add_test(NAME echonull_architecture_boundaries
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_SOURCE_DIR}/tests/architecture_boundaries.ps1"
      -SourceRoot "${CMAKE_SOURCE_DIR}/src")
endif()
