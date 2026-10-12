# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

option(MOCKTAIL_ENABLE_VR "Build experimental native OpenXR support (VR branch default)" ON)

add_library(mocktail_vr STATIC
  src/vr/diagnostics.cc
  src/vr/openxr_backend_mode.cc
  src/vr/vr_perf.cc
  src/vr/vr_mirror.cc
  src/vr/xr_controller.cc
  src/vr/roblox_vr_device_bridge.cc)
add_library(Mocktail::VR ALIAS mocktail_vr)
target_include_directories(mocktail_vr PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(mocktail_vr PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(mocktail_vr PUBLIC Vulkan::Headers PRIVATE nlohmann_json::nlohmann_json)
# shm_open for the desktop eye mirror lives in librt on glibc.
find_library(MOCKTAIL_RT_LIBRARY rt)
if(MOCKTAIL_RT_LIBRARY)
  target_link_libraries(mocktail_vr PUBLIC ${MOCKTAIL_RT_LIBRARY})
endif()
mocktail_apply_compile_options(mocktail_vr)

if(MOCKTAIL_ENABLE_VR)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Mocktail OpenXR support currently targets Linux")
  endif()
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/OpenXR-SDK/CMakeLists.txt")
    message(FATAL_ERROR
      "Initialize OpenXR: git submodule update --init third_party/OpenXR-SDK")
  endif()

  # Scope upstream's generic option names to this dependency. Static linking
  # packages the pinned loader without requiring a matching system loader.
  function(mocktail_add_openxr_sdk)
    set(BUILD_LOADER ON)
    set(DYNAMIC_LOADER OFF)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_API_LAYERS OFF)
    set(BUILD_CONFORMANCE_TESTS OFF)
    set(BUILD_SDK_TESTS OFF)
    set(BUILD_FORCE_GENERATION OFF)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/OpenXR-SDK"
      "${CMAKE_BINARY_DIR}/third_party/OpenXR-SDK" EXCLUDE_FROM_ALL)
  endfunction()
  mocktail_add_openxr_sdk()
  target_sources(mocktail_vr PRIVATE src/vr/openxr_probe.cc src/vr/openxr_preview.cc
    src/vr/openxr_backend.cc src/vr/gles_transport.cc src/vr/xr_actions.cc)
  # Error unwinding is confined to these native session implementations; their
  # public entry points catch errors before returning to the guest runtime.
  set_source_files_properties(src/vr/openxr_preview.cc src/vr/openxr_backend.cc
    src/vr/xr_actions.cc
    PROPERTIES COMPILE_OPTIONS -fexceptions)
  target_compile_definitions(mocktail_vr PRIVATE XR_USE_GRAPHICS_API_VULKAN)
  set_source_files_properties(src/vr/openxr_backend.cc PROPERTIES COMPILE_DEFINITIONS "XR_USE_GRAPHICS_API_OPENGL_ES;XR_USE_PLATFORM_EGL")
  target_include_directories(mocktail_vr PRIVATE ${MOCKTAIL_EGL_INCLUDE_DIR} ${MOCKTAIL_GLES3_INCLUDE_DIR})
  target_link_libraries(mocktail_vr PRIVATE OpenXR::openxr_loader Vulkan::Headers ${CMAKE_DL_LIBS})
else()
  target_sources(mocktail_vr PRIVATE src/vr/openxr_disabled.cc
    src/vr/xr_actions_disabled.cc)
endif()

add_executable(mocktail_vr_probe src/vr/probe_main.cc)
set_target_properties(mocktail_vr_probe PROPERTIES OUTPUT_NAME mocktail-vr-probe)
target_link_libraries(mocktail_vr_probe PRIVATE Mocktail::VR)
mocktail_apply_compile_options(mocktail_vr_probe)
install(TARGETS mocktail_vr_probe RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
