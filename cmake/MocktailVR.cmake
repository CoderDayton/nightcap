# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

include_guard(GLOBAL)

# AUTO builds OpenXR support when the third_party/OpenXR-SDK submodule is
# checked out and skips it otherwise, so source packages that do not fetch
# submodules still build. ON requires the submodule; OFF never builds it.
set(MOCKTAIL_ENABLE_VR "AUTO" CACHE STRING
  "Build experimental OpenXR support: AUTO, ON or OFF")
set_property(CACHE MOCKTAIL_ENABLE_VR PROPERTY STRINGS AUTO ON OFF)
set(_mocktail_openxr_sdk "${CMAKE_SOURCE_DIR}/third_party/OpenXR-SDK/CMakeLists.txt")
string(TOUPPER "${MOCKTAIL_ENABLE_VR}" _mocktail_vr_request)
if(_mocktail_vr_request STREQUAL "AUTO")
  if(EXISTS "${_mocktail_openxr_sdk}" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MOCKTAIL_ENABLE_VR ON)
  else()
    message(STATUS "OpenXR SDK submodule not found; building without VR "
      "(git submodule update --init third_party/OpenXR-SDK to enable it)")
    set(MOCKTAIL_ENABLE_VR OFF)
  endif()
elseif(MOCKTAIL_ENABLE_VR)
  set(MOCKTAIL_ENABLE_VR ON)
else()
  set(MOCKTAIL_ENABLE_VR OFF)
endif()

add_library(mocktail_vr STATIC
  src/vr/diagnostics.cc
  src/vr/headset_detection.cc
  src/vr/openxr_backend_mode.cc
  src/vr/vr_perf.cc
  src/vr/vr_mirror.cc
  src/vr/xr_controller.cc
  src/vr/roblox_vr_device_bridge.cc
  src/vr/virtual_keyboard.cc)
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
  if(NOT EXISTS "${_mocktail_openxr_sdk}")
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
    src/vr/openxr_backend.cc src/vr/gles_transport.cc src/vr/xr_actions.cc
    src/vr/virtual_keyboard_raster.cc)
  # The in-headset keyboard draws its labels with the same SDL3_ttf and
  # fontconfig stack as the TextBox overlay.
  find_package(SDL3 3.4 REQUIRED CONFIG)
  find_package(SDL3_ttf REQUIRED CONFIG)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  target_link_libraries(mocktail_vr PRIVATE SDL3_ttf::SDL3_ttf SDL3::SDL3
    PkgConfig::FONTCONFIG)
  # Error unwinding is confined to these native session implementations; their
  # public entry points catch errors before returning to the guest runtime.
  set_source_files_properties(src/vr/openxr_preview.cc src/vr/openxr_backend.cc
    src/vr/xr_actions.cc
    PROPERTIES COMPILE_OPTIONS -fexceptions)
  target_compile_definitions(mocktail_vr PRIVATE XR_USE_GRAPHICS_API_VULKAN)
  option(MOCKTAIL_VR_DIAGNOSTICS
    "Compile VR diagnostics: scripted head pose, eye evidence dumps" OFF)
  if(MOCKTAIL_VR_DIAGNOSTICS)
    target_compile_definitions(mocktail_vr PRIVATE MOCKTAIL_VR_DIAGNOSTICS=1)
  endif()
  set_source_files_properties(src/vr/openxr_backend.cc PROPERTIES COMPILE_DEFINITIONS "XR_USE_GRAPHICS_API_OPENGL_ES;XR_USE_PLATFORM_EGL")
  target_include_directories(mocktail_vr PRIVATE ${MOCKTAIL_EGL_INCLUDE_DIR} ${MOCKTAIL_GLES3_INCLUDE_DIR})
  # SMAA's area and search lookup textures for vr.anti_aliasing: smaa.
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/smaa/Textures/AreaTex.h")
    message(FATAL_ERROR
      "Initialize SMAA: git submodule update --init third_party/smaa")
  endif()
  target_include_directories(mocktail_vr PRIVATE "${CMAKE_SOURCE_DIR}/third_party/smaa")
  target_link_libraries(mocktail_vr PRIVATE OpenXR::openxr_loader Vulkan::Headers ${CMAKE_DL_LIBS})
else()
  target_sources(mocktail_vr PRIVATE src/vr/openxr_disabled.cc
    src/vr/xr_actions_disabled.cc)
endif()

if(MOCKTAIL_ENABLE_VR)
  add_executable(mocktail_vr_probe src/vr/probe_main.cc)
  set_target_properties(mocktail_vr_probe PROPERTIES
    OUTPUT_NAME mocktail-vr-probe)
  target_link_libraries(mocktail_vr_probe PRIVATE Mocktail::VR)
  mocktail_apply_compile_options(mocktail_vr_probe)
  install(TARGETS mocktail_vr_probe
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
