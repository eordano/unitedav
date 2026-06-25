# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(UnitedAV LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(UAV_SANITIZE "none" CACHE STRING "Sanitizer: none|address|thread")
set_property(CACHE UAV_SANITIZE PROPERTY STRINGS none address thread)
if(UAV_SANITIZE STREQUAL "address")
  set(_uav_san_flags "-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(UAV_SANITIZE STREQUAL "thread")
  set(_uav_san_flags "-fsanitize=thread -fno-omit-frame-pointer -g")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
  add_compile_definitions(UAV_TSAN=1)
elseif(NOT UAV_SANITIZE STREQUAL "none")
  message(FATAL_ERROR "UAV_SANITIZE must be one of: none, address, thread (got '${UAV_SANITIZE}')")
endif()

option(UAV_ENABLE_GPU "Build the zero-copy GPU present path (Linux EGL/VAAPI/dmabuf, Windows D3D11)" OFF)

set(UAV_GPU_API "gl" CACHE STRING "Linux GPU backend: gl|vulkan")
set_property(CACHE UAV_GPU_API PROPERTY STRINGS gl vulkan)

set(UAV_UNITY_PLUGIN_API_DIR "" CACHE PATH
    "Path to a local Unity PluginAPI dir (IUnityInterface.h et al); not vendored.")

find_package(PkgConfig QUIET)
set(UAV_HAVE_FFMPEG OFF)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample)
  if(FFMPEG_FOUND)
    set(UAV_HAVE_FFMPEG ON)
  endif()
endif()

set(UAV_HAVE_GPU OFF)
set(UAV_GPU_BACKEND "none")
if(UAV_ENABLE_GPU)
  if(WIN32)
    if(NOT UAV_HAVE_FFMPEG)
      message(FATAL_ERROR "UAV_ENABLE_GPU requires FFmpeg (libav*) — not found via pkg-config.")
    else()
      set(UAV_HAVE_GPU ON)
      set(UAV_GPU_BACKEND "d3d11")
    endif()
  elseif(APPLE)
    if(NOT UAV_HAVE_FFMPEG)
      message(FATAL_ERROR "UAV_ENABLE_GPU requires FFmpeg (libav*) — not found via pkg-config.")
    else()
      set(UAV_HAVE_GPU ON)
      set(UAV_GPU_BACKEND "metal")
    endif()
  elseif(UNIX)
    if(NOT UAV_HAVE_FFMPEG)
      message(FATAL_ERROR "UAV_ENABLE_GPU requires FFmpeg (libav*) — not found via pkg-config.")
    elseif(NOT PkgConfig_FOUND)
      message(FATAL_ERROR "UAV_ENABLE_GPU requires pkg-config to locate EGL/GLES/GBM/DRM.")
    else()
      if(UAV_GPU_API STREQUAL "vulkan")
        pkg_check_modules(GPUVK IMPORTED_TARGET vulkan shaderc)
        if(GPUVK_FOUND)
          set(UAV_HAVE_GPU ON)
          set(UAV_GPU_BACKEND "vulkan")
        else()
          message(FATAL_ERROR "UAV_ENABLE_GPU(vulkan): missing one of vulkan/shaderc.")
        endif()
      else()
        pkg_check_modules(GPUGL IMPORTED_TARGET egl glesv2 gbm libdrm libva libva-drm)
        if(GPUGL_FOUND)
          set(UAV_HAVE_GPU ON)
          set(UAV_GPU_BACKEND "gl")
        else()
          message(FATAL_ERROR "UAV_ENABLE_GPU: missing one of egl/glesv2/gbm/libdrm/libva/libva-drm.")
        endif()
      endif()
    endif()
  else()
    message(STATUS "UAV_ENABLE_GPU: no GPU backend for this platform; ignoring.")
  endif()
endif()

set(UAV_GPU_SOURCES "")
set(UAV_HAVE_UNITY_PLUGIN_API OFF)
if(UAV_HAVE_GPU)
  if(UAV_GPU_BACKEND STREQUAL "gl")
    list(APPEND UAV_GPU_SOURCES src/gpu/egl_vaapi.cpp src/gpu/unity_render.cpp)
  elseif(UAV_GPU_BACKEND STREQUAL "vulkan")
    list(APPEND UAV_GPU_SOURCES src/gpu/vk_convert.cpp src/gpu/unity_render.cpp)
  elseif(UAV_GPU_BACKEND STREQUAL "d3d11")
    list(APPEND UAV_GPU_SOURCES src/gpu/d3d11_convert.cpp src/gpu/unity_render.cpp)
  elseif(UAV_GPU_BACKEND STREQUAL "metal")
    list(APPEND UAV_GPU_SOURCES src/gpu/metal_convert.mm src/gpu/unity_render.cpp)
    set_source_files_properties(src/gpu/metal_convert.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  endif()
  if(UAV_UNITY_PLUGIN_API_DIR)
    if(EXISTS "${UAV_UNITY_PLUGIN_API_DIR}/IUnityInterface.h")
      set(UAV_HAVE_UNITY_PLUGIN_API ON)
      message(STATUS "UAV: Unity PluginAPI headers found at ${UAV_UNITY_PLUGIN_API_DIR}")
    else()
      message(FATAL_ERROR
        "UAV_UNITY_PLUGIN_API_DIR='${UAV_UNITY_PLUGIN_API_DIR}' has no IUnityInterface.h. "
        "Point it at a local Unity install's PluginAPI directory.")
    endif()
  else()
    message(STATUS
      "UAV: UAV_UNITY_PLUGIN_API_DIR not set; unity_render.cpp builds as an inert "
      "stub. Set it to a local Unity PluginAPI dir to build the live plugin.")
  endif()
endif()

add_library(UnitedAV SHARED
  src/uav_api.cpp
  src/player.cpp
  src/decode/decoder.cpp
  src/gpu/hwaccel.cpp
  src/send/uav_send_api.cpp
  src/send/sender.cpp
  ${UAV_GPU_SOURCES}
)

if(UAV_HAVE_GPU)
  if(UAV_GPU_BACKEND STREQUAL "gl")
    target_link_libraries(UnitedAV PRIVATE PkgConfig::GPUGL)
  elseif(UAV_GPU_BACKEND STREQUAL "vulkan")
    target_link_libraries(UnitedAV PRIVATE PkgConfig::GPUVK)
  elseif(UAV_GPU_BACKEND STREQUAL "d3d11")
    target_link_libraries(UnitedAV PRIVATE d3d11 dxgi)
  elseif(UAV_GPU_BACKEND STREQUAL "metal")
    target_link_libraries(UnitedAV PRIVATE "-framework Metal" "-framework CoreVideo" "-framework Foundation")
  endif()
  target_compile_definitions(UnitedAV PRIVATE UAV_ENABLE_GPU=1)
  target_include_directories(UnitedAV PRIVATE src/gpu)
  if(UAV_HAVE_UNITY_PLUGIN_API)
    target_include_directories(UnitedAV PRIVATE "${UAV_UNITY_PLUGIN_API_DIR}")
    target_compile_definitions(UnitedAV PRIVATE UAV_HAVE_UNITY_PLUGIN_API=1)
  endif()
endif()

target_include_directories(UnitedAV PUBLIC include)

if(UAV_HAVE_FFMPEG)
  target_link_libraries(UnitedAV PRIVATE PkgConfig::FFMPEG)
  target_compile_definitions(UnitedAV PRIVATE UAV_HAVE_FFMPEG=1)
else()
  message(WARNING
    "FFmpeg dev libraries not found via pkg-config. Building API/stub only. "
    "Install libavformat/libavcodec/libavutil/libswscale/libswresample-dev, "
    "or pass -DFFMPEG_ROOT and wire include/lib dirs.")
endif()

set_target_properties(UnitedAV PROPERTIES
  OUTPUT_NAME "UnitedAV"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden)

include(GNUInstallDirs)
install(TARGETS UnitedAV
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
  include/unitedav.h
  include/unitedav_send.h
  include/unitedav_gpu.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(UAV_HAVE_FFMPEG)
  add_executable(uav_probe tools/uav_probe.cpp)
  target_include_directories(uav_probe PRIVATE include)
  target_link_libraries(uav_probe PRIVATE UnitedAV)
  find_package(Threads REQUIRED)
  target_link_libraries(uav_probe PRIVATE Threads::Threads)

  add_executable(uav_send_probe tools/uav_send_probe.cpp)
  target_include_directories(uav_send_probe PRIVATE include)
  target_link_libraries(uav_send_probe PRIVATE UnitedAV Threads::Threads)

  install(TARGETS uav_probe uav_send_probe
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(UAV_HAVE_GPU AND UAV_GPU_BACKEND STREQUAL "gl")
  add_executable(uav_gpu_probe
    tools/uav_gpu_probe.cpp
    src/gpu/egl_vaapi.cpp
    src/gpu/hwaccel.cpp)
  target_include_directories(uav_gpu_probe PRIVATE include src)
  target_compile_definitions(uav_gpu_probe PRIVATE UAV_ENABLE_GPU=1 UAV_HAVE_FFMPEG=1)
  target_link_libraries(uav_gpu_probe PRIVATE PkgConfig::FFMPEG PkgConfig::GPUGL Threads::Threads)
elseif(UAV_HAVE_GPU AND UAV_GPU_BACKEND STREQUAL "vulkan")
  add_executable(uav_vk_probe
    tools/uav_vk_probe.cpp
    src/gpu/vk_convert.cpp
    src/gpu/hwaccel.cpp)
  target_include_directories(uav_vk_probe PRIVATE include src)
  target_compile_definitions(uav_vk_probe PRIVATE UAV_ENABLE_GPU=1 UAV_HAVE_FFMPEG=1)
  target_link_libraries(uav_vk_probe PRIVATE PkgConfig::FFMPEG PkgConfig::GPUVK Threads::Threads)
elseif(UAV_HAVE_GPU AND UAV_GPU_BACKEND STREQUAL "d3d11")
  add_executable(uav_d3d11_probe
    tools/uav_d3d11_probe.cpp
    src/gpu/d3d11_convert.cpp
    src/gpu/hwaccel.cpp)
  target_include_directories(uav_d3d11_probe PRIVATE include src)
  target_compile_definitions(uav_d3d11_probe PRIVATE UAV_ENABLE_GPU=1 UAV_HAVE_FFMPEG=1)
  target_link_libraries(uav_d3d11_probe PRIVATE PkgConfig::FFMPEG d3d11 dxgi Threads::Threads)
elseif(UAV_HAVE_GPU AND UAV_GPU_BACKEND STREQUAL "metal")
  add_executable(uav_metal_probe
    tools/uav_metal_probe.cpp
    src/gpu/metal_convert.mm
    src/gpu/hwaccel.cpp)
  target_include_directories(uav_metal_probe PRIVATE include src)
  target_compile_definitions(uav_metal_probe PRIVATE UAV_ENABLE_GPU=1 UAV_HAVE_FFMPEG=1)
  target_link_libraries(uav_metal_probe PRIVATE PkgConfig::FFMPEG
    "-framework Metal" "-framework CoreVideo" "-framework Foundation" Threads::Threads)
endif()

option(UAV_BUILD_TESTS "Build Tier-1/2 native unit + oracle tests" ON)
option(UAV_FUZZ        "Build libFuzzer targets under native/tests/fuzz (needs clang)" OFF)

if(UAV_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  enable_testing()
  include(CTest)
  find_package(Threads REQUIRED)

  find_path(UAV_DOCTEST_INCLUDE_DIR doctest/doctest.h)
  if(NOT UAV_DOCTEST_INCLUDE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/third_party/doctest.h")
      set(UAV_DOCTEST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/third_party")
      set(UAV_DOCTEST_FLAT 1)
    endif()
  endif()

  file(GLOB UAV_UNIT_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  if(UAV_UNIT_SOURCES AND UAV_DOCTEST_INCLUDE_DIR)
    add_executable(uav_tests ${UAV_UNIT_SOURCES})
    target_include_directories(uav_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit
      ${UAV_DOCTEST_INCLUDE_DIR})
    target_link_libraries(uav_tests PRIVATE UnitedAV Threads::Threads)
    if(UAV_DOCTEST_FLAT)
      target_compile_definitions(uav_tests PRIVATE UAV_DOCTEST_FLAT=1)
    endif()
    if(UAV_HAVE_FFMPEG)
      target_compile_definitions(uav_tests PRIVATE UAV_HAVE_FFMPEG=1)
      target_link_libraries(uav_tests PRIVATE PkgConfig::FFMPEG)
    endif()
    add_test(NAME uav_tests COMMAND uav_tests)
    set_tests_properties(uav_tests PROPERTIES
      TIMEOUT 120 LABELS "tier1;unit"
      ENVIRONMENT "UAV_HWDECODE=none")
  elseif(UAV_UNIT_SOURCES AND NOT UAV_DOCTEST_INCLUDE_DIR)
    message(WARNING
      "native/tests/unit/*.cpp present but doctest header not found "
      "(install nixpkgs#doctest or vendor native/tests/third_party/doctest.h). "
      "Skipping uav_tests.")
  endif()

  file(GLOB UAV_ORACLE_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/*.cpp")
  if(UAV_ORACLE_SOURCES AND UAV_HAVE_FFMPEG)
    add_executable(uav_oracle ${UAV_ORACLE_SOURCES})
    target_include_directories(uav_oracle PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle)
    target_link_libraries(uav_oracle PRIVATE UnitedAV PkgConfig::FFMPEG Threads::Threads)
    target_compile_definitions(uav_oracle PRIVATE UAV_HAVE_FFMPEG=1)
    add_test(NAME oracle_selftest COMMAND uav_oracle --selftest)
    set_tests_properties(oracle_selftest PROPERTIES
      TIMEOUT 120 LABELS "tier1;oracle" SKIP_RETURN_CODE 77)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle/run_oracle.cmake)
    uav_register_oracle_tests()
  endif()

  if(UAV_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "UAV_FUZZ=ON requires clang (libFuzzer). "
        "Configure with -DCMAKE_CXX_COMPILER=clang++.")
    endif()
    file(GLOB UAV_FUZZ_SOURCES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/*.cpp")
    foreach(_fuzz_src ${UAV_FUZZ_SOURCES})
      get_filename_component(_fuzz_name "${_fuzz_src}" NAME_WE)
      add_executable(${_fuzz_name} "${_fuzz_src}")
      target_include_directories(${_fuzz_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
      target_compile_options(${_fuzz_name} PRIVATE -fsanitize=fuzzer,address -g -fno-omit-frame-pointer)
      target_link_options(${_fuzz_name} PRIVATE -fsanitize=fuzzer,address)
      target_link_libraries(${_fuzz_name} PRIVATE UnitedAV Threads::Threads)
      if(UAV_HAVE_FFMPEG)
        target_compile_definitions(${_fuzz_name} PRIVATE UAV_HAVE_FFMPEG=1)
      endif()
      set(_fuzz_corpus "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${_fuzz_name}")
      file(MAKE_DIRECTORY "${_fuzz_corpus}")
      add_test(NAME fuzz_${_fuzz_name}
               COMMAND ${_fuzz_name} -max_total_time=10 -rss_limit_mb=2048
                       "${_fuzz_corpus}")
      set_tests_properties(fuzz_${_fuzz_name} PROPERTIES
        TIMEOUT 60 LABELS "tier2;fuzz")
    endforeach()
  endif()
endif()

option(UAV_SKIP_PLUGIN_COPY "Do not copy the built plugin into unity/Runtime/Plugins" OFF)
if(NOT UAV_SKIP_PLUGIN_COPY)
  set(UAV_PLUGIN_OUT "${CMAKE_SOURCE_DIR}/../unity/Runtime/Plugins")
  add_custom_command(TARGET UnitedAV POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UAV_PLUGIN_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:UnitedAV>" "${UAV_PLUGIN_OUT}/"
    COMMENT "Copying UnitedAV plugin into unity/Runtime/Plugins")
endif()
