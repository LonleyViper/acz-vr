cmake_minimum_required(VERSION 3.20)
project(acz-vr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Offline only: require the executor-hydrated SDK or an explicit existing cache.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Never download dependencies" FORCE)
if(NOT FETCHCONTENT_SOURCE_DIR_OPENXR)
  # OpenXR-SDK release-1.1.43 (781f2eab), kept outside every build directory.
  set(FETCHCONTENT_SOURCE_DIR_OPENXR
      "D:/acz-deps/openxr-sdk-1.1.43"
      CACHE PATH "Existing read-only OpenXR SDK")
endif()
if(NOT EXISTS "${FETCHCONTENT_SOURCE_DIR_OPENXR}/CMakeLists.txt")
  message(FATAL_ERROR "An existing cached OpenXR SDK is required; no downloads permitted")
endif()
FetchContent_Declare(openxr SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_OPENXR}")
FetchContent_MakeAvailable(openxr)

add_library(acz-vr SHARED
  diagnostics_log.cpp
  runtime_config.cpp
  dllmain.cpp
  launch_mode_win.cpp    # LAUNCH-MODE: the launcher's VR Mode combo + the On/Off factory gate (docs/world-render/launch-mode.md)
  d3d12_hook.cpp
  gpu_copy_ledger.cpp
  gpu_copy_witness.cpp
  gpu_copy_owner.cpp
  native_binding_observer.cpp
  buffer_binding_registry.cpp
  draw_capture.cpp
  capture_input_provider.cpp
  native_copy_provider.cpp
  capture_enablement.cpp
  shader_metadata.cpp
  pso_stage_inventory.cpp
  shader_access_facts.cpp
  capture_requirements_proof.cpp
  native_evidence_epochs.cpp
  root_binding_metadata.cpp
  srt_payload.cpp
  stereo_payload.cpp
  drawcpt_filter.cpp
  openxr_session.cpp
  stereo_renderer.cpp
  # J-xr-stereo-output (default OFF; constructed only by xr_stereo_proxy.cpp
  # when xr_stereo_output + gpu_copy_early_device_hook + gpu_copy_step5_continuous)
  xr_owner.cpp
  xr_start.cpp
  xr_owner_openxr.cpp
  xr_stereo_output.cpp
  tactical.cpp
  terrain_stage.cpp
  world_props.cpp
  cockpit_model.cpp
  aircraft_auto.cpp      # AIRCRAFT-AUTO: xr_cockpit_model=auto (E6 eye offset -> aircraft type)
  xr_stereo_d3d12.cpp
  xr_props_d3d12.cpp
  xr_stereo_proxy.cpp
  # STAGES: xr_terrain_stage=auto (fingerprint table + read-only memory scan)
  stage_fingerprint.cpp
  stage_scan.cpp         # R-3: bounded, prioritised, resident-only scan loop (SDK-free)
  stage_detect_win.cpp
  # J-w1-ee-camera (default OFF; armed only by ee_camera_probe + producer keys)
  ee_camera_probe.cpp
  ee_camera_scan.cpp
  ee_camera_diff.cpp
  ee_look_write.cpp
  ee_look_input_trace.cpp
  ee_lens_scan.cpp
  ee_ps2_probe.cpp
  ee_e6_locator.cpp      # E6-ANCHOR-FREE: content-validated E6 block locator (xr_terrain_stage e6loc:)
  ee_camera_probe_win.cpp
  # VSCREEN (xr_terrain_stage vscreen:1, default OFF): capture at the game's Present + the XR screen copy (docs/world-render/vscreen.md)
  vscreen_capture_win.cpp
  xr_vscreen_d3d12.cpp
  # BRIEF3D (xr_terrain_stage brief3d:1, default OFF): the briefing map table blit into the eye swapchains (docs/world-render/brief3d.md)
  xr_brief3d_d3d12.cpp
  # VSEARLY (xr_terrain_stage vsearly:1, default OFF): XR in the menus + per-flight stage detection (docs/world-render/vsearly.md)
  xr_flight_stage.cpp
  # xr_look_drive_keys (default OFF): real SendInput injector + its factory registration (the logic is header-only)
  look_drive_keys.cpp
  dxgi_proxy.def
)
# Blit shaders: Windows SDK fxc at build time -> embedded bytecode headers
# (same approach as the standalone probe; no runtime shader compiler).
find_program(ACZ_VR_FXC fxc REQUIRED)
set(_acz_vr_blit_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/xr_stereo_blit.hlsl)
set(_acz_vr_props_hlsli ${CMAKE_CURRENT_SOURCE_DIR}/xr_props_composite.hlsli)   # WORLD-3: included by xr_stereo_blit.hlsl
set(_acz_vr_props_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/xr_props.hlsl)               # WORLD-3: props/trees pass
set(_acz_vr_shader_dir ${CMAKE_CURRENT_BINARY_DIR}/acz-vr-shaders)
file(MAKE_DIRECTORY ${_acz_vr_shader_dir})
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stereo_blit_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_main /Vn g_xr_stereo_vs /Fh ${_acz_vr_shader_dir}/xr_stereo_blit_vs.h ${_acz_vr_blit_hlsl}
  DEPENDS ${_acz_vr_blit_hlsl} ${_acz_vr_props_hlsli} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stereo_blit_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_main /Vn g_xr_stereo_ps /Fh ${_acz_vr_shader_dir}/xr_stereo_blit_ps.h ${_acz_vr_blit_hlsl}
  DEPENDS ${_acz_vr_blit_hlsl} ${_acz_vr_props_hlsli} VERBATIM)
# J-world-cache: the cache write pass (ps_cache_update) and the horizon-fit fill pass (ps_cache_fill) are further entry points of the same file.
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stereo_cache_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_cache_update /Vn g_xr_cache_ps /Fh ${_acz_vr_shader_dir}/xr_stereo_cache_ps.h ${_acz_vr_blit_hlsl}
  DEPENDS ${_acz_vr_blit_hlsl} ${_acz_vr_props_hlsli} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stereo_fill_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_cache_fill /Vn g_xr_fill_ps /Fh ${_acz_vr_shader_dir}/xr_stereo_fill_ps.h ${_acz_vr_blit_hlsl}
  DEPENDS ${_acz_vr_blit_hlsl} ${_acz_vr_props_hlsli} VERBATIM)
# WORLD-3: the props/trees pass (instanced, depth-tested) -> g_xr_props_vs / g_xr_props_ps
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_props_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_props /Vn g_xr_props_vs /Fh ${_acz_vr_shader_dir}/xr_props_vs.h ${_acz_vr_props_hlsl}
  DEPENDS ${_acz_vr_props_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_props_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_props /Vn g_xr_props_ps /Fh ${_acz_vr_shader_dir}/xr_props_ps.h ${_acz_vr_props_hlsl}
  DEPENDS ${_acz_vr_props_hlsl} VERBATIM)
# COCKPIT-2: the model cockpit pass (vs_model / ps_model) is a separate file (WORLD tracks edit xr_stereo_blit.hlsl in parallel).
set(_acz_vr_cockpit_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/cockpit_model.hlsl)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/cockpit_model_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_model /Vn g_cockpit_vs /Fh ${_acz_vr_shader_dir}/cockpit_model_vs.h ${_acz_vr_cockpit_hlsl}
  DEPENDS ${_acz_vr_cockpit_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/cockpit_model_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_model /Vn g_cockpit_ps /Fh ${_acz_vr_shader_dir}/cockpit_model_ps.h ${_acz_vr_cockpit_hlsl}
  DEPENDS ${_acz_vr_cockpit_hlsl} VERBATIM)
# COCKPIT-4: the canopy glass pass (dual-source transmission) -> g_cockpit_glass_ps
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/cockpit_model_glass_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_glass /Vn g_cockpit_glass_ps /Fh ${_acz_vr_shader_dir}/cockpit_model_glass_ps.h ${_acz_vr_cockpit_hlsl}
  DEPENDS ${_acz_vr_cockpit_hlsl} VERBATIM)
# TACTICAL (tac:1): the overlay pass (clip-space triangles from a root SRV) -> g_xr_tac_vs / g_xr_tac_ps
set(_acz_vr_tac_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/xr_tactical.hlsl)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_tactical_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_tac /Vn g_xr_tac_vs /Fh ${_acz_vr_shader_dir}/xr_tactical_vs.h ${_acz_vr_tac_hlsl}
  DEPENDS ${_acz_vr_tac_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_tactical_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_tac /Vn g_xr_tac_ps /Fh ${_acz_vr_shader_dir}/xr_tactical_ps.h ${_acz_vr_tac_hlsl}
  DEPENDS ${_acz_vr_tac_hlsl} VERBATIM)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/xr_tactical_vs.h ${_acz_vr_shader_dir}/xr_tactical_ps.h)
# NBOX (mk360:3): the stamp pass (the game's own box + label pixels from the stamp atlas, same vertex stream) -> g_xr_stamp_vs / g_xr_stamp_ps
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stamp_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_stamp /Vn g_xr_stamp_vs /Fh ${_acz_vr_shader_dir}/xr_stamp_vs.h ${_acz_vr_tac_hlsl}
  DEPENDS ${_acz_vr_tac_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_stamp_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_stamp /Vn g_xr_stamp_ps /Fh ${_acz_vr_shader_dir}/xr_stamp_ps.h ${_acz_vr_tac_hlsl}
  DEPENDS ${_acz_vr_tac_hlsl} VERBATIM)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/xr_stamp_vs.h ${_acz_vr_shader_dir}/xr_stamp_ps.h)
# VSCREEN (vscreen:1): the capture pass (back-buffer copy -> R8G8B8A8_UNORM screen image) -> g_xr_vscreen_vs / g_xr_vscreen_ps
set(_acz_vr_vscreen_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/xr_vscreen.hlsl)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_vscreen_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_screen /Vn g_xr_vscreen_vs /Fh ${_acz_vr_shader_dir}/xr_vscreen_vs.h ${_acz_vr_vscreen_hlsl}
  DEPENDS ${_acz_vr_vscreen_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_vscreen_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_screen /Vn g_xr_vscreen_ps /Fh ${_acz_vr_shader_dir}/xr_vscreen_ps.h ${_acz_vr_vscreen_hlsl}
  DEPENDS ${_acz_vr_vscreen_hlsl} VERBATIM)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/xr_vscreen_vs.h ${_acz_vr_shader_dir}/xr_vscreen_ps.h)
# BRIEF3D (brief3d:1): producer eye image (R16G16B16A16_FLOAT, premultiplied display values) -> eye swapchain (sRGB RTV) -> g_xr_brief3d_vs / _ps
set(_acz_vr_brief3d_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/xr_brief3d.hlsl)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_brief3d_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_table /Vn g_xr_brief3d_vs /Fh ${_acz_vr_shader_dir}/xr_brief3d_vs.h ${_acz_vr_brief3d_hlsl}
  DEPENDS ${_acz_vr_brief3d_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/xr_brief3d_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_table /Vn g_xr_brief3d_ps /Fh ${_acz_vr_shader_dir}/xr_brief3d_ps.h ${_acz_vr_brief3d_hlsl}
  DEPENDS ${_acz_vr_brief3d_hlsl} VERBATIM)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/xr_brief3d_vs.h ${_acz_vr_shader_dir}/xr_brief3d_ps.h)
# EFFECTS-OUTSIDE (M6, fxatlas:1): the puff-card capture pass (the game's root signature: t0 table + static s0) -> g_fx_atlas_vs / g_fx_atlas_ps.
# d3d12_hook.cpp includes them under ACZ_FX_ATLAS_SHADERS (acz-vr only; the offline test targets that compile d3d12_hook.cpp build without them: no copy PSO).
set(_acz_vr_atlas_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/fx_puff_atlas.hlsl)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/fx_puff_atlas_vs.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_copy /Vn g_fx_atlas_vs /Fh ${_acz_vr_shader_dir}/fx_puff_atlas_vs.h ${_acz_vr_atlas_hlsl}
  DEPENDS ${_acz_vr_atlas_hlsl} VERBATIM)
add_custom_command(OUTPUT ${_acz_vr_shader_dir}/fx_puff_atlas_ps.h
  COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_copy /Vn g_fx_atlas_ps /Fh ${_acz_vr_shader_dir}/fx_puff_atlas_ps.h ${_acz_vr_atlas_hlsl}
  DEPENDS ${_acz_vr_atlas_hlsl} VERBATIM)
add_custom_target(acz_vr_atlas_shader_headers DEPENDS ${_acz_vr_shader_dir}/fx_puff_atlas_vs.h ${_acz_vr_shader_dir}/fx_puff_atlas_ps.h)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/fx_puff_atlas_vs.h ${_acz_vr_shader_dir}/fx_puff_atlas_ps.h)
target_compile_definitions(acz-vr PRIVATE ACZ_FX_ATLAS_SHADERS=1)
target_sources(acz-vr PRIVATE ${_acz_vr_shader_dir}/xr_stereo_blit_vs.h ${_acz_vr_shader_dir}/xr_stereo_blit_ps.h
  ${_acz_vr_shader_dir}/xr_stereo_cache_ps.h ${_acz_vr_shader_dir}/xr_stereo_fill_ps.h ${_acz_vr_shader_dir}/xr_props_vs.h ${_acz_vr_shader_dir}/xr_props_ps.h
  ${_acz_vr_shader_dir}/cockpit_model_vs.h ${_acz_vr_shader_dir}/cockpit_model_ps.h ${_acz_vr_shader_dir}/cockpit_model_glass_ps.h)
target_include_directories(acz-vr PRIVATE ${_acz_vr_shader_dir})

# A per-application XInput bridge for automated, owner-authorized test input.
# It deliberately avoids a system-wide virtual-HID driver.  Deployment into a
# game directory is never part of the build or test targets.
add_library(acz-xinput-proxy SHARED
  xinput_proxy.cpp
  xinput_proxy.def
)
set_target_properties(acz-xinput-proxy PROPERTIES
  OUTPUT_NAME "XINPUT1_4"
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(acz-input-bridge
  acz_input_bridge.cpp
)
set_target_properties(acz-input-bridge PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(acz-vr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Step-1 experiment evidence pin: configure-time source commit (+"-dirty" when
# tracked files differ). Re-run the configure step after committing to refresh.
set(ACZ_VR_SOURCE_PIN "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _acz_sha RESULT_VARIABLE _acz_rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_acz_rc EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain --untracked-files=no
      OUTPUT_VARIABLE _acz_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(ACZ_VR_SOURCE_PIN "${_acz_sha}")
    if(_acz_dirty)
      string(APPEND ACZ_VR_SOURCE_PIN "-dirty")
    endif()
  endif()
endif()
message(STATUS "acz-vr source pin: ${ACZ_VR_SOURCE_PIN}")
target_compile_definitions(acz-vr PRIVATE "ACZ_VR_SOURCE_PIN=\"${ACZ_VR_SOURCE_PIN}\"")
target_link_libraries(acz-vr PRIVATE
  openxr_loader
  d3d12
  dxgi
  bcrypt
)

set_target_properties(acz-vr PROPERTIES
  OUTPUT_NAME "dxgi"
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  # The proxy DLL deliberately has the DXGI filename, but its import library
  # must not shadow the Windows SDK's dxgi.lib while this target links it.
  ARCHIVE_OUTPUT_NAME "acz-vr-dxgi-proxy"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# J-3b4: optional forwarding d3d12.dll proxy for the early device hook.
# Default OFF: when OFF nothing below is configured, and the dxgi.dll target is
# identical either way. Never installed by the build (tools/live/l1.ps1 only).
# The output lands in its own directory so no test or probe executable can
# accidentally load it from its application directory.
option(ACZ_VR_BUILD_D3D12_PROXY "Build the optional forwarding d3d12.dll proxy (J-3b4)" OFF)
if(ACZ_VR_BUILD_D3D12_PROXY)
  # d3d12_proxy_exports.h is generated by scripts/gen-d3d12-proxy-exports.ps1.
  add_library(acz-d3d12-proxy SHARED d3d12_proxy.cpp d3d12_proxy_exports.h)
  target_include_directories(acz-d3d12-proxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(acz-d3d12-proxy PROPERTIES
    OUTPUT_NAME "d3d12"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/d3d12-proxy"
    # Must not shadow the Windows SDK's d3d12.lib used by every other target.
    ARCHIVE_OUTPUT_NAME "acz-vr-d3d12-proxy"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  # Build-time check: generated export table == live system export table.
  add_executable(d3d12_proxy_export_check ../../tests/acz-vr/d3d12_proxy_export_check.cpp)
  file(TO_CMAKE_PATH "$ENV{SystemRoot}/System32/d3d12.dll" ACZ_VR_SYSTEM_D3D12)
  add_dependencies(acz-d3d12-proxy d3d12_proxy_export_check)
  add_custom_command(TARGET acz-d3d12-proxy POST_BUILD
    COMMAND $<TARGET_FILE:d3d12_proxy_export_check> "${ACZ_VR_SYSTEM_D3D12}" "$<TARGET_FILE:acz-d3d12-proxy>"
    COMMENT "Checking d3d12.dll proxy exports against ${ACZ_VR_SYSTEM_D3D12}"
    VERBATIM)
endif()

option(ACZ_VR_BUILD_TESTS "Build cockpit-math C++ tests" ON)
option(ACZ_VR_BUILD_XR_PROBE "Build standalone phase-1 XR harness; never run automatically" OFF)
set(ACZ_XR_PROBE_BUILD_TESTS ${ACZ_VR_BUILD_TESTS})
add_subdirectory(xr_probe)
if(ACZ_VR_BUILD_TESTS)
  enable_testing()
  add_executable(gpu_copy_observation_indices ../../tests/acz-vr/gpu_copy_observation_indices.cpp)
  target_include_directories(gpu_copy_observation_indices PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu_copy_observation_indices COMMAND gpu_copy_observation_indices)
  add_executable(gpu_copy_observation_offline_tests
    ../../tests/acz-vr/gpu_copy_observation_offline_tests.cpp gpu_copy_witness.cpp gpu_copy_ledger.cpp gpu_copy_owner.cpp
    diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp
    root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp
    buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
  target_include_directories(gpu_copy_observation_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(gpu_copy_observation_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(gpu_copy_observation_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME gpu_copy_observation_offline COMMAND gpu_copy_observation_offline_tests)
  set_tests_properties(gpu_copy_observation_offline PROPERTIES TIMEOUT 30)
  add_executable(gpu_copy_witness_offline_tests
    ../../tests/acz-vr/gpu_copy_witness_offline_tests.cpp gpu_copy_witness.cpp gpu_copy_ledger.cpp)
  target_include_directories(gpu_copy_witness_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu_copy_witness_offline COMMAND gpu_copy_witness_offline_tests)
  set_tests_properties(gpu_copy_witness_offline PROPERTIES TIMEOUT 30)
  # 3b part 2: the step-1 experiment through the REAL hook wrappers, owner and
  # ledger with fake D3D12 objects (writable fake vtables). Synthetic only.
  add_executable(gpu_copy_step1_offline_tests
    ../../tests/acz-vr/gpu_copy_step1_offline_tests.cpp gpu_copy_witness.cpp gpu_copy_ledger.cpp gpu_copy_owner.cpp
    diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp
    root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp
    buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
  target_include_directories(gpu_copy_step1_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(gpu_copy_step1_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(gpu_copy_step1_offline_tests PRIVATE d3d12 bcrypt)
  foreach(scenario IN ITEMS off success refusals no-qualified prepare-fail close-fail duplicate reexecute
      uint64max signal-fail mismatch closure-timeout no-execute reset-before-complete poll-timeout cross-queue
      off-list1 list1-witnessed list1-distinct prehook-discovery release-recreate
      early-off early-device early-repeat early-mismatch early-distinct
      later-generations later-generation-queries later-reexecute-same later-reset-failed
      later-reset-in-join later-unclosed-execute later-released-before-reset later-execute-open early-binding
      step2-success step2-default-heap step2-shape-changed step2-over-cap step2-cpu-mismatch step2-no-cpu
      step2-all-refused step2-index-range step2-signal-fail step2-duplicate step2-no-qualified step2-rules
      step3-success step3-msaa step3-om-unknown step3-om-gap step3-over-cap step3-srv-unknown step3-format
      step3-depth-equal step3-mismatch step3-no-snapshot step3-create-fail step3-signal-fail step3-off step3-off-step2 step3-rules
      step3-float step3-crop-clip step3-crop-offscreen step3-crop-cap step3-arm-timeout step3-gs-rejected
      step3-select step3-no-visible step3-ring-srt step3-live-unreadable step3-set step3-set-partial step3-set-unreadable step3-set-rules
      step5-continuous step5-discard step5-ambiguity step5-pose step5-threads step5-rules step5-varying step5-skip step5-world step5-world-changed step5-world-simultaneous step5-float-depth step5-upload-retire step5-pose-drop step5-look-drive step5-look-sweep step5-source step5-om-descriptor step5-world-barriers step5-fence-epoch step5-alt-admit step5-owner-aba step5-owner-batch step5-owner-pin step5-owner-held step5-acquire-quarantine-race step5-untracked-before-acquire step5-unexamined-phase step5-owner-table-full step5-fence-rollover-held step5-world-entry step5-world-array feature-log step5-feature-log step5-consumer-rev5 step3-ring-overflow step3-eval-rules
      step5-om-interleave step5-world-reselect-identity step5-world-reselect-uncovered step5-world-foreign-queue step5-world-reexecute step5-world-preinject step5-neutral-generation step5-neutral-export step5-world-texture-retirement step5-world-texture-uncovered step5-world-texture-release-reentry step5-hud-observe
      step5-hud-off step5-hud step5-hud-refused step5-hud-rs-skip step5-hud-cap step5-hud-region step5-hud-faults step5-hud-rules step5-hud-radar-rules step5-hud-radar-off step5-hud-radar-region step5-hud-radar step5-hud-radar-caps step5-png-scene-end-radar
      step5-cockpit-keys step5-hmd-fov-rules step5-hmd-fov step5-precockpit-observe step5-members-by-class
      step5-hud-markers-rules step5-hud-markers-off step5-hud-markers step5-hud-markers-refused step5-hud-markers-region step5-hud-markers-create-refused step5-tac
      step5-effects-rules step5-world-scene-identity step5-effects-smoke-rules step5-effects-gamecloud-rules step5-effects-gamecloud step5-effects-smoke step5-effects-smoke-capture step5-effects-smoke-gpu step5-effects-smoke-gpu-late step5-effects-sprite-gpu step5-eye-ring step5-eye-ring-eyeobj-off step5-eye-ring-no-extent step5-eye-ring-fx step5-effects-off step5-effects step5-effects-refused step5-effects-dual-rules step5-effects-dual step5-effects-dual-refused step5-effects-additive-off step5-effects-additive-on step5-effects-dual-clone-missing step5-effects-dual-mixed step5-effects-create-refused step5-effects-pred-refused
      step5-world-keys step5-world-rules step5-world-objects step5-world-vs-binding step5-world-hud step5-flags-by-class step5-flags-world step5-flags-hud step5-world-observe-rules step5-world-observe step5-world-observe-carrier step5-world-early step5-world-early-look step5-world-evidence-rules step5-world-evidence step5-world-evidence-early step5-phase-observe-rules step5-phase-observe step5-phase-observe-window step5-world-observe-root step5-world-stored
      scratch-dir-unique step5-png-rules step5-png-off step5-png step5-png-faults step5-png-refused step5-producer-dead
      step5-png-fx-rules step5-png-effects step5-png-effects-threshold step5-png-effects-invalid-threshold step5-png-effects-off step5-png-burst-file step5-png-burst-file-stale step5-png-scene-end-barrier step5-png-scene-end-declined step5-png-scene-end-absent step5-effects-alpha-only)
    add_test(NAME gpu_copy_step1_${scenario}_offline COMMAND gpu_copy_step1_offline_tests ${scenario})
    set_tests_properties(gpu_copy_step1_${scenario}_offline PROPERTIES TIMEOUT 60)
  endforeach()
  # J-171: SDK-free observer state/formatting and config parser fixtures.
  add_executable(gpu_copy_hud_observe_offline_tests
    ../../tests/acz-vr/gpu_copy_hud_observe_offline_tests.cpp runtime_config.cpp)
  target_include_directories(gpu_copy_hud_observe_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  foreach(scenario IN ITEMS config fields events bounds)
    add_test(NAME gpu_copy_hud_observe_${scenario}_offline COMMAND gpu_copy_hud_observe_offline_tests ${scenario})
    set_tests_properties(gpu_copy_hud_observe_${scenario}_offline PROPERTIES TIMEOUT 30)
  endforeach()
  # 3b part 1 owner unit fakes (gpu_copy_owner.cpp is in the DLL since part 2).
  add_executable(gpu_copy_owner_offline_tests
    ../../tests/acz-vr/gpu_copy_owner_offline_tests.cpp gpu_copy_owner.cpp gpu_copy_witness.cpp gpu_copy_ledger.cpp)
  target_include_directories(gpu_copy_owner_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu_copy_owner_offline COMMAND gpu_copy_owner_offline_tests)
  set_tests_properties(gpu_copy_owner_offline PROPERTIES TIMEOUT 30)
  add_executable(gpu_copy_ledger_offline_tests
    ../../tests/acz-vr/gpu_copy_ledger_offline_tests.cpp gpu_copy_ledger.cpp)
  target_include_directories(gpu_copy_ledger_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu_copy_ledger_offline COMMAND gpu_copy_ledger_offline_tests)
  set_tests_properties(gpu_copy_ledger_offline PROPERTIES TIMEOUT 30)
  add_executable(gpu_copy_drain_offline_tests
    ../../tests/acz-vr/gpu_copy_drain_offline_tests.cpp gpu_copy_ledger.cpp
    diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp
    root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp
    buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
  target_include_directories(gpu_copy_drain_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(gpu_copy_drain_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(gpu_copy_drain_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME gpu_copy_drain_offline COMMAND gpu_copy_drain_offline_tests)
  set_tests_properties(gpu_copy_drain_offline PROPERTIES TIMEOUT 30)
  add_executable(diagnostics_activation_offline_tests
    ../../tests/acz-vr/diagnostics_activation_offline_tests.cpp
    diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp
    root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp
    buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
  target_include_directories(diagnostics_activation_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(diagnostics_activation_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(diagnostics_activation_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME diagnostics_activation_offline COMMAND diagnostics_activation_offline_tests)
  set_tests_properties(diagnostics_activation_offline PROPERTIES TIMEOUT 30)
  # One additive CPU-only logger/config and fake-COM forwarding fixture.
  add_executable(diagnostics_offline_tests
    ../../tests/acz-vr/diagnostics_offline_tests.cpp)
  target_include_directories(diagnostics_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME diagnostics_offline COMMAND diagnostics_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  set_tests_properties(diagnostics_offline PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:diagnostics_offline_tests>")
  # CPU-only pinned access facts and SDK root diagnostic; no device or shader execution.
  add_executable(shader_access_facts_offline_tests
    ../../tests/acz-vr/shader_access_facts_offline_tests.cpp
    shader_access_facts.cpp pso_stage_inventory.cpp shader_metadata.cpp root_binding_metadata.cpp)
  target_include_directories(shader_access_facts_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(shader_access_facts_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME shader_access_facts_offline COMMAND shader_access_facts_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  # Real-byte rejection and explicitly synthetic CPU/fake-COM capture chain.
  add_executable(e2e_capture_chain_offline_tests
    ../../tests/acz-vr/e2e_capture_chain_offline_tests.cpp
    shader_access_facts.cpp capture_requirements_proof.cpp pso_stage_inventory.cpp shader_metadata.cpp
    native_copy_provider.cpp capture_enablement.cpp stereo_payload.cpp
    capture_input_provider.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp native_evidence_epochs.cpp)
  target_include_directories(e2e_capture_chain_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(e2e_capture_chain_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME e2e_capture_chain_offline COMMAND e2e_capture_chain_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  # CPU-only parts/provenance proof; SDK CPU root exports and CNG hashes only.
  add_executable(capture_requirements_proof_offline_tests
    ../../tests/acz-vr/capture_requirements_proof_offline_tests.cpp
    shader_access_facts.cpp capture_requirements_proof.cpp pso_stage_inventory.cpp shader_metadata.cpp
    capture_input_provider.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp native_evidence_epochs.cpp)
  target_include_directories(capture_requirements_proof_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(capture_requirements_proof_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME capture_requirements_proof_offline COMMAND capture_requirements_proof_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  # Selected-input and retained-epoch fixtures: CPU metadata only; no native copy/replay.
  add_executable(draw_input_provenance_contract_offline_tests
    ../../tests/acz-vr/draw_input_provenance_contract_offline_tests.cpp
    shader_access_facts.cpp capture_requirements_proof.cpp pso_stage_inventory.cpp shader_metadata.cpp
    capture_input_provider.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp native_evidence_epochs.cpp)
  target_include_directories(draw_input_provenance_contract_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(draw_input_provenance_contract_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME draw_input_provenance_contract_offline COMMAND draw_input_provenance_contract_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  # Fixed-storage CPU evidence epochs and injected fake ownership/fence backends only.
  add_executable(native_evidence_epochs_offline_tests
    ../../tests/acz-vr/native_evidence_epochs_offline_tests.cpp native_evidence_epochs.cpp)
  target_include_directories(native_evidence_epochs_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME native_evidence_epochs_offline COMMAND native_evidence_epochs_offline_tests)
  add_executable(native_ownership_retirement_offline_tests
    ../../tests/acz-vr/native_ownership_retirement_offline_tests.cpp native_ownership_retirement.cpp)
  target_include_directories(native_ownership_retirement_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME native_ownership_retirement_offline COMMAND native_ownership_retirement_offline_tests)
  # Literal container metadata only; SDK root decoding is a CPU export.
  add_executable(pso_stage_inventory_offline_tests
    ../../tests/acz-vr/pso_stage_inventory_offline_tests.cpp
    pso_stage_inventory.cpp shader_metadata.cpp root_binding_metadata.cpp)
  target_include_directories(pso_stage_inventory_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(pso_stage_inventory_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME pso_stage_inventory_offline COMMAND pso_stage_inventory_offline_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/evidence/e10-drawcpt-pso.json")
  add_executable(test_cockpit_math
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/test_cockpit_math.cpp
  )
  target_include_directories(test_cockpit_math PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cockpit_math COMMAND test_cockpit_math)
  # J-w1-ee-camera: fake EE block + fake game frame; real SEH paths on private pages.
  add_executable(ee_camera_probe_offline_tests
    ../../tests/acz-vr/ee_camera_probe_offline_tests.cpp ee_look_write.cpp ee_camera_probe.cpp ee_camera_scan.cpp ee_camera_diff.cpp runtime_config.cpp)
  target_include_directories(ee_camera_probe_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(ee_camera_probe_offline_tests PRIVATE ACZ_VR_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")   # review 2026-11-01 F7 source check
  add_test(NAME ee_camera_probe_offline COMMAND ee_camera_probe_offline_tests)
  set_tests_properties(ee_camera_probe_offline PROPERTIES TIMEOUT 120)
  # Option 3 stage 2a/2b look-write state machines (fake memory/game only; never touches a process).
  add_executable(ee_look_write_offline_tests
    ../../tests/acz-vr/ee_look_write_offline_tests.cpp ee_look_write.cpp ee_camera_probe.cpp ee_camera_scan.cpp ee_camera_diff.cpp runtime_config.cpp)
  target_include_directories(ee_look_write_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ee_look_write_offline COMMAND ee_look_write_offline_tests)
  set_tests_properties(ee_look_write_offline PROPERTIES TIMEOUT 120)
  # xr_look_drive_keys: controller, look measurement, double-rotation fix and config keys against fakes and a simulated look filter.
  # The real injector is linked (factory registration only) but never sends a key here.
  add_executable(look_drive_keys_offline_tests
    ../../tests/acz-vr/look_drive_keys_offline_tests.cpp look_drive_keys.cpp runtime_config.cpp)
  target_include_directories(look_drive_keys_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME look_drive_keys_offline COMMAND look_drive_keys_offline_tests)
  set_tests_properties(look_drive_keys_offline PROPERTIES TIMEOUT 120)
  # POV fixes (docs/review/pov-fixes-impl.md): F-REF clusters + majority reference + diagnostic line, F-LEAN setup_poses maths, F-HUD rate limiter, config keys.
  add_executable(pov_fixes_offline_tests
    ../../tests/acz-vr/pov_fixes_offline_tests.cpp runtime_config.cpp)
  target_include_directories(pov_fixes_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME pov_fixes_offline COMMAND pov_fixes_offline_tests)
  set_tests_properties(pov_fixes_offline PROPERTIES TIMEOUT 120)
  # xr_look_drive_xinput: the right-stick IPC (src/skip-intro/look_ipc.h) staleness/clamp/kill switch, the analog controller route, open-loop
  # default, idle calibration and the jump gate, against fakes and a private named mapping (no key, no game, no XInput device).
  add_executable(look_drive_xinput_offline_tests
    ../../tests/acz-vr/look_drive_xinput_offline_tests.cpp look_drive_keys.cpp runtime_config.cpp)
  target_include_directories(look_drive_xinput_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME look_drive_xinput_offline COMMAND look_drive_xinput_offline_tests)
  set_tests_properties(look_drive_xinput_offline PROPERTIES TIMEOUT 120)
  # LOOK-STICK GATE (docs/world-render/look-stick-gate.md, D-355): lookstick option, the xr-view heartbeat IPC (src/skip-intro/xr_view_ipc.h),
  # the proxy's right-stick gate policy and the writer's heartbeat decision (header-only, no OS object).
  add_executable(look_stick_gate_offline_tests
    ../../tests/acz-vr/look_stick_gate_offline_tests.cpp)
  target_include_directories(look_stick_gate_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME look_stick_gate_offline COMMAND look_stick_gate_offline_tests)
  set_tests_properties(look_stick_gate_offline PROPERTIES TIMEOUT 60)
  # SEATED RECENTER (docs/world-render/recenter.md): rc:/rckey:/rcauto: options, pose math (checked against the producer's Neutral), key debounce and
  # the controller decisions (header-only, no OS object, no key state).
  add_executable(recenter_offline_tests ../../tests/acz-vr/recenter_offline_tests.cpp)
  target_include_directories(recenter_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME recenter_offline COMMAND recenter_offline_tests)
  set_tests_properties(recenter_offline PROPERTIES TIMEOUT 60)
  # NULL-HEAD (TEST ONLY, docs/world-render/null-head-look.md): nh:/nhs: options, head.now parser, mailbox, slew/hold/settle controller, pose
  # composition (sign checked against recenter.h) and the xr_owner PoseFilter seam (snapshot and consumer see the same turned views).
  add_executable(null_head_offline_tests ../../tests/acz-vr/null_head_offline_tests.cpp xr_owner.cpp)
  target_include_directories(null_head_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME null_head_offline COMMAND null_head_offline_tests)
  set_tests_properties(null_head_offline PROPERTIES TIMEOUT 60)
  # D-269 follow-up: read-only look-input trace (fake memory only). The analysis tool's python self-test is NOT in ctest
  # (the repo runs no python tests there): python -I tools/analysis/look_input_trace.py --selftest
  add_executable(ee_look_input_trace_offline_tests
    ../../tests/acz-vr/ee_look_input_trace_offline_tests.cpp ee_look_input_trace.cpp ee_look_write.cpp ee_camera_probe.cpp ee_camera_scan.cpp ee_camera_diff.cpp runtime_config.cpp)
  target_include_directories(ee_look_input_trace_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ee_look_input_trace_offline COMMAND ee_look_input_trace_offline_tests)
  set_tests_properties(ee_look_input_trace_offline PROPERTIES TIMEOUT 120)
  # Lens/FOV widen plan phase 0: READ-ONLY lens scanner (synthetic memory images only; no game, no process memory).
  add_executable(ee_lens_scan_offline_tests
    ../../tests/acz-vr/ee_lens_scan_offline_tests.cpp ee_lens_scan.cpp ee_look_write.cpp ee_camera_probe.cpp ee_camera_scan.cpp ee_camera_diff.cpp runtime_config.cpp)
  target_include_directories(ee_lens_scan_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ee_lens_scan_offline COMMAND ee_lens_scan_offline_tests)
  set_tests_properties(ee_lens_scan_offline PROPERTIES TIMEOUT 120)
  # PS2 address probe: READ-ONLY (synthetic memory images only; no game, no process memory).
  add_executable(ee_ps2_probe_offline_tests
    ../../tests/acz-vr/ee_ps2_probe_offline_tests.cpp ee_ps2_probe.cpp ee_lens_scan.cpp ee_look_write.cpp ee_camera_probe.cpp ee_camera_scan.cpp ee_camera_diff.cpp runtime_config.cpp)
  target_include_directories(ee_ps2_probe_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ee_ps2_probe_offline COMMAND ee_ps2_probe_offline_tests)
  set_tests_properties(ee_ps2_probe_offline PROPERTIES TIMEOUT 120)


  add_executable(xinput_bridge_offline_tests
    ../../tests/acz-vr/xinput_bridge_offline_tests.cpp
  )
  target_include_directories(xinput_bridge_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME xinput_bridge_offline COMMAND xinput_bridge_offline_tests)

  add_executable(xinput_proxy_offline_tests
    ../../tests/acz-vr/xinput_proxy_offline_tests.cpp
  )
  target_include_directories(xinput_proxy_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_dependencies(xinput_proxy_offline_tests acz-xinput-proxy)
  add_test(NAME xinput_proxy_offline COMMAND xinput_proxy_offline_tests
    "$<TARGET_FILE:acz-xinput-proxy>")

  # Includes dllmain.cpp through its fake-only lifecycle seam; no GPU/XR linkage.
  add_executable(ownership_shutdown_offline_tests ../../tests/acz-vr/ownership_shutdown_offline_tests.cpp)
  target_include_directories(ownership_shutdown_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_sources(ownership_shutdown_offline_tests PRIVATE diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    capture_enablement.cpp capture_input_provider.cpp)
  target_compile_definitions(ownership_shutdown_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(ownership_shutdown_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME ownership_shutdown_offline COMMAND ownership_shutdown_offline_tests)
  set_tests_properties(ownership_shutdown_offline PROPERTIES TIMEOUT 30)
  # FPS growth root cause (docs/world-render/fps-investigation.md section 7): HookState::pipelines claims index == the former whole-map scans.
  add_executable(fps_growth_offline_tests ../../tests/acz-vr/fps_growth_offline_tests.cpp)
  target_include_directories(fps_growth_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_sources(fps_growth_offline_tests PRIVATE diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    capture_enablement.cpp capture_input_provider.cpp gpu_copy_ledger.cpp gpu_copy_witness.cpp gpu_copy_owner.cpp)
  target_compile_definitions(fps_growth_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
  target_link_libraries(fps_growth_offline_tests PRIVATE d3d12 bcrypt)
  add_test(NAME fps_growth_offline COMMAND fps_growth_offline_tests)
  set_tests_properties(fps_growth_offline PROPERTIES TIMEOUT 120)

  add_executable(dll_lifecycle_offline_tests
    ../../tests/acz-vr/dll_lifecycle_offline_tests.cpp)
  target_include_directories(dll_lifecycle_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME dll_lifecycle_offline COMMAND dll_lifecycle_offline_tests)

  add_executable(openxr_session_offline_tests
    ../../tests/acz-vr/openxr_session_offline_tests.cpp openxr_session.cpp)
  target_include_directories(openxr_session_offline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${FETCHCONTENT_SOURCE_DIR_OPENXR}/include)
  target_link_libraries(openxr_session_offline_tests PRIVATE openxr_loader d3d12 dxgi)
  add_test(NAME openxr_session_offline COMMAND openxr_session_offline_tests)

  # In-proxy XR frame owner (cards b/c): fake runtime/clock only; NOT linked into the proxy.
  add_executable(xr_owner_offline_tests ../../tests/acz-vr/xr_owner_offline_tests.cpp xr_owner.cpp xr_start.cpp)
  target_include_directories(xr_owner_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME xr_owner_offline COMMAND xr_owner_offline_tests)
  set_tests_properties(xr_owner_offline PROPERTIES TIMEOUT 60)
  # Compile-only check of the real OpenXR backend (also linked into dxgi.dll
  # since J-xr-stereo-output, where it runs only with xr_stereo_output on).
  add_library(xr_owner_openxr_compile OBJECT xr_owner_openxr.cpp)
  target_include_directories(xr_owner_openxr_compile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${FETCHCONTENT_SOURCE_DIR_OPENXR}/include)
  # J-xr-stereo-output consumer: the REAL owner + consumer core against fake
  # XR/swapchain/D3D12/FrameSource objects; no loader, device or headset.
  add_executable(xr_stereo_output_offline_tests ../../tests/acz-vr/xr_stereo_output_offline_tests.cpp
    xr_owner.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp world_props.cpp cockpit_model.cpp aircraft_auto.cpp runtime_config.cpp)
  target_include_directories(xr_stereo_output_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME xr_stereo_output_offline COMMAND xr_stereo_output_offline_tests)
  set_tests_properties(xr_stereo_output_offline PROPERTIES TIMEOUT 120)
  # VSCREEN (vscreen:1; docs/world-render/vscreen.md): options, mode decision, placement, capture ring (incl. two-thread stress), the screen-only
  # layer list, and the consumer's no-new-frame path with a fake hook (the real owner + consumer core; no device, no headset).
  add_executable(vscreen_offline_tests ../../tests/acz-vr/vscreen_offline_tests.cpp
    xr_owner.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp world_props.cpp cockpit_model.cpp aircraft_auto.cpp runtime_config.cpp)
  target_include_directories(vscreen_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME vscreen_offline COMMAND vscreen_offline_tests)
  set_tests_properties(vscreen_offline PROPERTIES TIMEOUT 60)
  # VSEARLY (vsearly:1; docs/world-render/vsearly.md): early start gate, flight start/end, the per-flight stage slot and its retire rule (incl. a
  # two-thread run), the early retry policy and the owner's frame-stall limit (header-only core + the real xr_start/xr_owner; no device, no headset).
  add_executable(flight_cycle_offline_tests ../../tests/acz-vr/flight_cycle_offline_tests.cpp xr_owner.cpp xr_start.cpp)
  target_include_directories(flight_cycle_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME flight_cycle_offline COMMAND flight_cycle_offline_tests)
  set_tests_properties(flight_cycle_offline PROPERTIES TIMEOUT 60)
  # LAUNCH-MODE + NATIVE-VR START (docs/world-render/launch-mode.md): launch-mode.txt parse/format, the On/Off latch decision, the launcher
  # combo layout, the vsstart:/ACZ_XR_START start gate and the vscreen option token (header-only cores; no window, no device, no headset).
  add_executable(launch_mode_offline_tests ../../tests/acz-vr/launch_mode_offline_tests.cpp)
  target_include_directories(launch_mode_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME launch_mode_offline COMMAND launch_mode_offline_tests)
  set_tests_properties(launch_mode_offline PROPERTIES TIMEOUT 30)
  # Manual only (shows a window; NOT in ctest, not built by default): a fake launcher named acz.exe that loads the built dxgi.dll and
  # checks the combo injection, the selection file and the factory gate (launch-mode.md 6). Run it with LOCALAPPDATA set to a scratch folder.
  add_executable(launch_mode_harness EXCLUDE_FROM_ALL ../../tests/acz-vr/launch_mode_harness.cpp)
  set_target_properties(launch_mode_harness PROPERTIES OUTPUT_NAME "acz" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launch-mode-harness")
  target_link_libraries(launch_mode_harness PRIVATE dxguid)
  # VSEARLY adapter (Astra 6458816088 P2): the real xr_flight_stage.cpp producer switches across an in-flight session loss and a failing detection.
  add_executable(flight_stage_adapter_offline_tests ../../tests/acz-vr/flight_stage_adapter_offline_tests.cpp xr_flight_stage.cpp tactical.cpp terrain_stage.cpp world_props.cpp)
  target_include_directories(flight_stage_adapter_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME flight_stage_adapter_offline COMMAND flight_stage_adapter_offline_tests)
  set_tests_properties(flight_stage_adapter_offline PROPERTIES TIMEOUT 60)
  # VIEW-MODE (pov:1; docs/world-render/pov.md): option, E6 view geometry on the logged W1 samples, mode machine + flight-stage keep-alive (header-only).
  add_executable(view_mode_offline_tests ../../tests/acz-vr/view_mode_offline_tests.cpp)
  target_include_directories(view_mode_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME view_mode_offline COMMAND view_mode_offline_tests)
  set_tests_properties(view_mode_offline PROPERTIES TIMEOUT 60)
  # EYE-RES (eyeres:<pct>; docs/world-render/eye-resolution.md): option, recommended-size extent, test-8 densities, byte estimate (header-only).
  add_executable(eye_res_offline_tests ../../tests/acz-vr/eye_res_offline_tests.cpp)
  target_include_directories(eye_res_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME eye_res_offline COMMAND eye_res_offline_tests)
  set_tests_properties(eye_res_offline PROPERTIES TIMEOUT 60)
  # POV-RENDER stage 1 (povc / povrig / povv / povtc; docs/world-render/other-povs.md): options, routes, rigs, vignette, census + ACM request (header-only).
  add_executable(pov_render_offline_tests ../../tests/acz-vr/pov_render_offline_tests.cpp)
  target_include_directories(pov_render_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME pov_render_offline COMMAND pov_render_offline_tests)
  set_tests_properties(pov_render_offline PROPERTIES TIMEOUT 60)
  # Astra review 6455891169 finding 1: the REAL Output + ScreenCore on one shared HUD swapchain (wait timeout / release failure -> flight).
  add_test(NAME xr_stereo_output_vscreen_offline COMMAND xr_stereo_output_offline_tests --vscreen)
  set_tests_properties(xr_stereo_output_vscreen_offline PROPERTIES TIMEOUT 120)
  # TACTICAL (tac:1): option parsing, producer helpers (marker clustering/matching, wedge key), tracker, radar pixels, marker projection, firing
  # debounce, tracers, overlay geometry and the consumer's key-off identity (docs/world-render/tactical.md).
  add_executable(tactical_offline_tests ../../tests/acz-vr/tactical_offline_tests.cpp tactical.cpp world_props.cpp terrain_stage.cpp
    xr_owner.cpp xr_stereo_output.cpp cockpit_model.cpp aircraft_auto.cpp runtime_config.cpp)
  target_include_directories(tactical_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME tactical_offline COMMAND tactical_offline_tests)
  set_tests_properties(tactical_offline PROPERTIES TIMEOUT 60)
  # MARKERS-360 (mk360:1|2 inside xr_terrain_stage): one world-anchored box per game-reported contact over the whole sphere (marker360.h,
  # docs/world-render/markers-360.md): options/flags, the producer's hide rule, A geometry, identity/removal sequences, one box per contact.
  add_executable(marker360_offline_tests ../../tests/acz-vr/marker360_offline_tests.cpp tactical.cpp)
  target_include_directories(marker360_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME marker360_offline COMMAND marker360_offline_tests)
  set_tests_properties(marker360_offline PROPERTIES TIMEOUT 60)
  # MSL-HUD (hud_msl.h; docs/world-render/aircraft-auto.md section 14): HUD MSSL count from the game's glyph quads (extraction, block, reading, the 4-tap
  # salvo 50 -> 46, non-ASCII atlas, option, dump round trip). `hud_msl_offline_tests --replay <step5-producer.log>` replays mslhud:2 dumps.
  add_executable(hud_msl_offline_tests ../../tests/acz-vr/hud_msl_offline_tests.cpp)
  target_include_directories(hud_msl_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME hud_msl_offline COMMAND hud_msl_offline_tests)
  set_tests_properties(hud_msl_offline PROPERTIES TIMEOUT 60)
  # HUD-LATE (xr_terrain_stage hudlate:1; hud_late.h; docs/world-render/markers-360.md section 18): the post-tone-map HUD text class (radio subtitles),
  # the late-skip decision, counters / status line, the stage switch and the option parse.
  add_executable(hud_late_offline_tests ../../tests/acz-vr/hud_late_offline_tests.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(hud_late_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME hud_late_offline COMMAND hud_late_offline_tests)
  set_tests_properties(hud_late_offline PROPERTIES TIMEOUT 60)
  # J-terrain-render: baked stage loader/errors, conventions, sampling, CPU ray march, camera mailbox, config keys; and the consumer-side cases alone.
  add_executable(terrain_offline_tests ../../tests/acz-vr/terrain_offline_tests.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(terrain_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME terrain_offline COMMAND terrain_offline_tests)
  set_tests_properties(terrain_offline PROPERTIES TIMEOUT 120)
  # PRESET (D-400, terrain_preset.h): xr_terrain_stage vrn:1 expansion in the config reader; every extractor parses the expanded value like the long form.
  add_executable(terrain_preset_offline_tests ../../tests/acz-vr/terrain_preset_offline_tests.cpp runtime_config.cpp)
  target_include_directories(terrain_preset_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME terrain_preset_offline COMMAND terrain_preset_offline_tests)
  set_tests_properties(terrain_preset_offline PROPERTIES TIMEOUT 60)
  # LENS-FLARE (docs/world-render/lens-flare.md): the CPU mirror of the eye compositor's head-look flare (lens_flare.h), options and the env texel.
  add_executable(lens_flare_offline_tests ../../tests/acz-vr/lens_flare_offline_tests.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(lens_flare_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME lens_flare_offline COMMAND lens_flare_offline_tests)
  set_tests_properties(lens_flare_offline PROPERTIES TIMEOUT 60)
  # WORLD-CLOUDS (docs/world-render/world-clouds.md): world_clouds.h CPU reference, DDA vs brute force, producer puff drop + census
  add_executable(world_clouds_offline_tests ../../tests/acz-vr/world_clouds_offline_tests.cpp terrain_stage.cpp runtime_config.cpp fx_memory.cpp)
  target_include_directories(world_clouds_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME world_clouds_offline COMMAND world_clouds_offline_tests)
  set_tests_properties(world_clouds_offline PROPERTIES TIMEOUT 60)
  # EFFECTS-OUTSIDE (M) fxtrail (docs/world-render/effects-outside-window.md section 19): fx_puff_trail.h world-space smoke-puff memory, plan_draw extra_drop,
  # the fx-puff-trail line, fxtrail:/fxtmax: options and the stage switches.
  add_executable(fx_puff_trail_offline_tests ../../tests/acz-vr/fx_puff_trail_offline_tests.cpp terrain_stage.cpp runtime_config.cpp fx_memory.cpp)
  target_include_directories(fx_puff_trail_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_puff_trail_offline COMMAND fx_puff_trail_offline_tests)
  set_tests_properties(fx_puff_trail_offline PROPERTIES TIMEOUT 60)
  # EFFECTS-OUTSIDE (M4) fxcloud / smoke stop-start (docs/world-render/effects-outside-window.md section 20): the cloud class of fx_puff_trail.h, one card per
  # stored card per frame, kMinSeen, the game-window hand-over, cross-key hosts, the fades, nohost, the fx-puff-mem line, fxcloud:/fxcfar:/fxtxkey:.
  add_executable(fx_puff_cloud_offline_tests ../../tests/acz-vr/fx_puff_cloud_offline_tests.cpp terrain_stage.cpp runtime_config.cpp fx_memory.cpp)
  target_include_directories(fx_puff_cloud_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_puff_cloud_offline COMMAND fx_puff_cloud_offline_tests)
  set_tests_properties(fx_puff_cloud_offline PROPERTIES TIMEOUT 60)
  # EFFECTS-OUTSIDE (M6) fxatlas (docs/world-render/effects-outside-window.md section 23): fx_puff_atlas.h tile table / lifetime / uv round trip / host check /
  # line, and the test-7 scenario (a cached card whose key is not bound this frame drawn from its tile; unb 0) next to the fxatlas:0 (M5) behaviour.
  add_executable(fx_puff_atlas_offline_tests ../../tests/acz-vr/fx_puff_atlas_offline_tests.cpp terrain_stage.cpp runtime_config.cpp fx_memory.cpp)
  target_include_directories(fx_puff_atlas_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_puff_atlas_offline COMMAND fx_puff_atlas_offline_tests)
  set_tests_properties(fx_puff_atlas_offline PROPERTIES TIMEOUT 60)
  # WORLD-SCALE (docs/world-render/world-scale.md): evidenced 5 u/m world head scale, wscale:, perceived size via the terrain eyes, producer publish/fallback.
  add_executable(world_scale_offline_tests ../../tests/acz-vr/world_scale_offline_tests.cpp terrain_stage.cpp)
  target_include_directories(world_scale_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME world_scale_offline COMMAND world_scale_offline_tests)
  set_tests_properties(world_scale_offline PROPERTIES TIMEOUT 60)
  # WORLD-3: .zvro loader/errors (incl. the Python-baked fixture), options, ground snap, grid selection (range/behind/cap), projection, occlusion reference.
  add_executable(props_offline_tests ../../tests/acz-vr/props_offline_tests.cpp world_props.cpp terrain_stage.cpp)
  target_include_directories(props_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(props_offline_tests PRIVATE ACZ_PROPS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/fixtures/props_synth.zvro")
  add_test(NAME props_offline COMMAND props_offline_tests)
  set_tests_properties(props_offline PROPERTIES TIMEOUT 120)
  # PERF: the xr_png_once write path's heap (counting allocator): png_stream == png_encode bytes, streamed peak, the sequential writer queue.
  add_executable(png_dump_offline_tests ../../tests/acz-vr/png_dump_offline_tests.cpp)
  target_include_directories(png_dump_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME png_dump_offline COMMAND png_dump_offline_tests)
  set_tests_properties(png_dump_offline PROPERTIES TIMEOUT 120)
  # STAGES: fingerprints.json parser (incl. the Python-built synthetic fixture) and the Matcher against fake byte streams (no game data).
  add_executable(stage_fingerprint_offline_tests ../../tests/acz-vr/stage_fingerprint_offline_tests.cpp stage_fingerprint.cpp stage_scan.cpp)
  target_include_directories(stage_fingerprint_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(stage_fingerprint_offline_tests PRIVATE ACZ_STAGEFP_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/fixtures/stage_fingerprints_synth.json")
  add_test(NAME stage_fingerprint_offline COMMAND stage_fingerprint_offline_tests)
  set_tests_properties(stage_fingerprint_offline PROPERTIES TIMEOUT 120)
  # E6-ANCHOR-FREE: content-validated E6 locator against fake memory images (planted block among decoys, static/moving/unreadable, caps, option).
  add_executable(e6_locator_offline_tests ../../tests/acz-vr/e6_locator_offline_tests.cpp ee_e6_locator.cpp)
  target_include_directories(e6_locator_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME e6_locator_offline COMMAND e6_locator_offline_tests)
  set_tests_properties(e6_locator_offline PROPERTIES TIMEOUT 120)
  # CAM-EPOCH (Astra P2): timed/epoch-stamped E6 samples, history lookup, admission, lag meter, randomized reference model, thread stress.
  add_executable(cam_epoch_offline_tests ../../tests/acz-vr/cam_epoch_offline_tests.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(cam_epoch_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cam_epoch_offline COMMAND cam_epoch_offline_tests)
  set_tests_properties(cam_epoch_offline PROPERTIES TIMEOUT 180)
  # CAM-EPOCH offline lag analysis (tools/live/camera-lag-analyze.py) on synthetic camlag lines; only when a Python 3 interpreter is installed.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME camera_lag_analyze_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/live/camera-lag-analyze.py --selftest)
    set_tests_properties(camera_lag_analyze_selftest PROPERTIES TIMEOUT 120)
  endif()
  add_test(NAME xr_stereo_terrain_offline COMMAND xr_stereo_output_offline_tests --terrain)
  # COCKPIT-2: .zvrm loader/refusals, options, visible mesh, 6DoF eye camera, CPU reference raster landmarks (no GPU, no game data).
  add_executable(cockpit_model_offline_tests ../../tests/acz-vr/cockpit_model_offline_tests.cpp cockpit_model.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(cockpit_model_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME cockpit_model_offline COMMAND cockpit_model_offline_tests)
  set_tests_properties(cockpit_model_offline PROPERTIES TIMEOUT 120)
  add_test(NAME xr_stereo_cockpit_model_offline COMMAND xr_stereo_output_offline_tests --cockpit)
  set_tests_properties(xr_stereo_cockpit_model_offline PROPERTIES TIMEOUT 60)
  # REVIEW (docs/review/overnight-loaders-review.md): adversarial loader/parser tests: truncation at every length, header words at boundary values,
  # payload mutation with a recomputed hash (fixed seeds), accepted mutants driven through the proxy's consumers; option-string fuzz (no path escapes).
  add_executable(loader_fuzz_offline_tests ../../tests/acz-vr/loader_fuzz_offline_tests.cpp terrain_stage.cpp world_props.cpp cockpit_model.cpp
    stage_fingerprint.cpp aircraft_auto.cpp ee_e6_locator.cpp)
  target_include_directories(loader_fuzz_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(loader_fuzz_offline_tests PRIVATE ACZ_PROPS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/fixtures/props_synth.zvro")
  add_test(NAME loader_fuzz_offline COMMAND loader_fuzz_offline_tests)
  set_tests_properties(loader_fuzz_offline PROPERTIES TIMEOUT 300)
  # AIRCRAFT-AUTO: type table vs aircraft_cockpits.json, E6 sample -> aircraft-frame eye, detector on fake E6 streams (36 types + noise, F-15 tie,
  # mid-session switch, no lock), auto config parse; the consumer side (fake eye source + loader) runs as xr_stereo_output_offline_tests --cockpit-auto.
  add_executable(aircraft_auto_offline_tests ../../tests/acz-vr/aircraft_auto_offline_tests.cpp aircraft_auto.cpp ee_e6_locator.cpp cockpit_model.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(aircraft_auto_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(aircraft_auto_offline_tests PRIVATE ACZ_AIRCRAFT_TABLE="${CMAKE_CURRENT_SOURCE_DIR}/../../research/world-render-data/aircraft_cockpits.json")
  add_test(NAME aircraft_auto_offline COMMAND aircraft_auto_offline_tests)
  set_tests_properties(aircraft_auto_offline PROPERTIES TIMEOUT 120)
  # EFFECTS-OUTSIDE (B): particle-memory core (unprojection of the game's pre-projected sprites, memory, billboards, CPU reference renderer).
  add_executable(fx_memory_offline_tests ../../tests/acz-vr/fx_memory_offline_tests.cpp fx_memory.cpp)
  target_include_directories(fx_memory_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_memory_offline COMMAND fx_memory_offline_tests)
  set_tests_properties(fx_memory_offline PROPERTIES TIMEOUT 60)
  # EFFECTS-OUTSIDE (C): missile smoke (fx_smoke.h): record layout from the dumped GS, eye substitution of +0/+64, CPU emulation of the GS, bounded capture.
  add_executable(fx_smoke_offline_tests ../../tests/acz-vr/fx_smoke_offline_tests.cpp)
  target_include_directories(fx_smoke_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_smoke_offline COMMAND fx_smoke_offline_tests)
  set_tests_properties(fx_smoke_offline PROPERTIES TIMEOUT 60)
  # BRIEFING-3D: briefing-observe.arm draw histogram (briefing_observe.h; docs/world-render/briefing-3d-map.md section 4).
  add_executable(briefing_observe_offline_tests ../../tests/acz-vr/briefing_observe_offline_tests.cpp)
  target_include_directories(briefing_observe_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME briefing_observe_offline COMMAND briefing_observe_offline_tests)
  set_tests_properties(briefing_observe_offline PROPERTIES TIMEOUT 60)
  # BRIEF3D (roadmap R4): tabletop briefing map core (brief3d.h; docs/world-render/brief3d.md): fit, eye matrices, records, replay plan.
  add_executable(brief3d_offline_tests ../../tests/acz-vr/brief3d_offline_tests.cpp)
  target_include_directories(brief3d_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME brief3d_offline COMMAND brief3d_offline_tests)
  set_tests_properties(brief3d_offline PROPERTIES TIMEOUT 60)
  # UI3D stage 1: the game's 2D menu draws re-projected per eye onto depth layers (ui3d.h; docs/world-render/ui-3d.md). Pure core only.
  add_executable(ui3d_offline_tests ../../tests/acz-vr/ui3d_offline_tests.cpp)
  target_include_directories(ui3d_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_offline COMMAND ui3d_offline_tests)
  set_tests_properties(ui3d_offline PROPERTIES TIMEOUT 60)
  # UI3D 2026-10-30b: the table blit's resample filter (ui3d_resample.h mirror of xr_brief3d.hlsl; ui-3d.md 11).
  add_executable(ui3d_resample_offline_tests ../../tests/acz-vr/ui3d_resample_offline_tests.cpp)
  target_include_directories(ui3d_resample_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_resample_offline COMMAND ui3d_resample_offline_tests)
  set_tests_properties(ui3d_resample_offline PROPERTIES TIMEOUT 60)
  # UI3D stage 3a: the in-flight pause menu on depth layers (ui3d_pause.h, the producer pause frames, PauseCore, census tail; ui-3d.md 7).
  add_executable(ui3d_pause_offline_tests ../../tests/acz-vr/ui3d_pause_offline_tests.cpp)
  target_include_directories(ui3d_pause_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_pause_offline COMMAND ui3d_pause_offline_tests)
  set_tests_properties(ui3d_pause_offline PROPERTIES TIMEOUT 60)
  # UI3D stage 3b: the hangar screens as a 3D room (ui3d_hangar.h on the real K10 records, the producer hangar frames; ui-3d.md 9).
  add_executable(ui3d_hangar_offline_tests ../../tests/acz-vr/ui3d_hangar_offline_tests.cpp)
  target_include_directories(ui3d_hangar_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_hangar_offline COMMAND ui3d_hangar_offline_tests)
  set_tests_properties(ui3d_hangar_offline PROPERTIES TIMEOUT 60)
  # UI3D 2026-10-30c: the selection box as a 3D slab behind the text, the dropped hangar tint, the re-projected hangar light glows (ui-3d.md 12).
  add_executable(ui3d_select_offline_tests ../../tests/acz-vr/ui3d_select_offline_tests.cpp)
  target_include_directories(ui3d_select_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_select_offline COMMAND ui3d_select_offline_tests)
  set_tests_properties(ui3d_select_offline PROPERTIES TIMEOUT 60)
  # UI3D 2026-10-30d: one surface per panel (u3pp), frame rims (u3lt), the hangar glow memory (u3agm) (ui-3d.md 13).
  add_executable(ui3d_panel_offline_tests ../../tests/acz-vr/ui3d_panel_offline_tests.cpp)
  target_include_directories(ui3d_panel_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_panel_offline COMMAND ui3d_panel_offline_tests)
  set_tests_properties(ui3d_panel_offline PROPERTIES TIMEOUT 60)
  # UI3D 2026-10-31 (ui-3d.md 14): stages 2-3 of the 3D reinterpretation (panel groups, bevels, raised text / selection, pane back faces)
  add_executable(ui3d_groups_offline_tests ../../tests/acz-vr/ui3d_groups_offline_tests.cpp)
  target_include_directories(ui3d_groups_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_groups_offline COMMAND ui3d_groups_offline_tests)
  set_tests_properties(ui3d_groups_offline PROPERTIES TIMEOUT 60)
  # DEBRIEF-3D: the post-mission debrief in 3D (dbr3d.h; debrief-3d-map.md 7-8): the pure core (stage 1 table + stage 2 route, index scan,
  # wraparound rewrite) and the producer's debrief frame kind (brief3d_live.h Producer::dbr3d_draw) with fakes.
  add_executable(dbr3d_offline_tests ../../tests/acz-vr/dbr3d_offline_tests.cpp)
  target_include_directories(dbr3d_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME dbr3d_offline COMMAND dbr3d_offline_tests)
  set_tests_properties(dbr3d_offline PROPERTIES TIMEOUT 60)
  add_executable(dbr3d_live_offline_tests ../../tests/acz-vr/dbr3d_live_offline_tests.cpp)
  target_include_directories(dbr3d_live_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME dbr3d_live_offline COMMAND dbr3d_live_offline_tests)
  set_tests_properties(dbr3d_live_offline PROPERTIES TIMEOUT 60)
  # BRIEF3D live wiring: producer core (brief3d_live.h: frame/slot/list lifetime) + consumer core (xr_brief3d_core.h) with fakes.
  add_executable(brief3d_live_offline_tests ../../tests/acz-vr/brief3d_live_offline_tests.cpp)
  target_include_directories(brief3d_live_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME brief3d_live_offline COMMAND brief3d_live_offline_tests)
  set_tests_properties(brief3d_live_offline PROPERTIES TIMEOUT 60)
  add_executable(brief3d_table_offline_tests ../../tests/acz-vr/brief3d_table_offline_tests.cpp
    xr_owner.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp world_props.cpp cockpit_model.cpp aircraft_auto.cpp runtime_config.cpp)
  target_include_directories(brief3d_table_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME brief3d_table_offline COMMAND brief3d_table_offline_tests)
  set_tests_properties(brief3d_table_offline PROPERTIES TIMEOUT 60)
  # BRIEF3D section 11 (b3l:1): the latched scene re-drawn every XR frame per eye (brief3d_latch.h, Producer::latch_draw, LatchCore) with fakes;
  # the world-lock proof (head pose changing between game updates) and the lean-in culling without a frame refusal.
  add_executable(brief3d_latch_offline_tests ../../tests/acz-vr/brief3d_latch_offline_tests.cpp
    xr_owner.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp world_props.cpp cockpit_model.cpp aircraft_auto.cpp runtime_config.cpp)
  target_include_directories(brief3d_latch_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME brief3d_latch_offline COMMAND brief3d_latch_offline_tests)
  set_tests_properties(brief3d_latch_offline PROPERTIES TIMEOUT 60)
  # BRIEF3D phase-2 capture (brief3d_capture.h; brief3d.md 6.6/7): safe reader with fakes, sampling, icon-vs-grid comparison, bounded line.
  add_executable(brief3d_capture_offline_tests ../../tests/acz-vr/brief3d_capture_offline_tests.cpp)
  target_include_directories(brief3d_capture_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME brief3d_capture_offline COMMAND brief3d_capture_offline_tests)
  set_tests_properties(brief3d_capture_offline PROPERTIES TIMEOUT 60)
  # UI3D menu draw census (ui3d_census.h; ui-3d.md 4.1): file-armed read-only sampling, bounded formatting, bbox maths, latch.
  add_executable(ui3d_census_offline_tests ../../tests/acz-vr/ui3d_census_offline_tests.cpp)
  target_include_directories(ui3d_census_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ui3d_census_offline COMMAND ui3d_census_offline_tests)
  set_tests_properties(ui3d_census_offline PROPERTIES TIMEOUT 60)
  # EFFECTS-OUTSIDE (D): fxmem:3 explosion/tracer draw census (fx_census.h) + CPU core of the ps2_v sprite head-pose replay (fx_sprite_replay.h).
  add_executable(fx_census_offline_tests ../../tests/acz-vr/fx_census_offline_tests.cpp fx_memory.cpp)
  target_include_directories(fx_census_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_census_offline COMMAND fx_census_offline_tests)
  set_tests_properties(fx_census_offline PROPERTIES TIMEOUT 60)
  # READ-ONLY flare / aircraft diagnostics (fx_diag_flare_air.h): predicates, bbox, limiter, bounded formatters, line queue.
  add_executable(fx_diag_flare_air_offline_tests ../../tests/acz-vr/fx_diag_flare_air_offline_tests.cpp)
  target_include_directories(fx_diag_flare_air_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fx_diag_flare_air_offline COMMAND fx_diag_flare_air_offline_tests)
  set_tests_properties(fx_diag_flare_air_offline PROPERTIES TIMEOUT 60)
  add_test(NAME xr_stereo_cockpit_auto_offline COMMAND xr_stereo_output_offline_tests --cockpit-auto)
  set_tests_properties(xr_stereo_cockpit_auto_offline PROPERTIES TIMEOUT 60)
  # AIRCRAFT-AUTO-JUNK: a junk-block lock (implausible eye) no longer vouches for the remembered aircraft (headset 2026-10-30 F-5E drawn as the Draken).
  add_test(NAME xr_stereo_cockpit_auto_junk_offline COMMAND xr_stereo_output_offline_tests --cockpit-auto-junk)
  set_tests_properties(xr_stereo_cockpit_auto_junk_offline PROPERTIES TIMEOUT 60)
  # Overnight runtime-safety review (docs/review/overnight-runtime-review.md): SEH/VirtualQuery reads vs racing decommit, the stage scanner on
  # real memory (guard/no-access pages never read, read-only region unchanged, concurrent alloc/free churn), E6 locator block loss/torn/fuzz,
  # AIRCRAFT-AUTO detector under allocation failure.
  add_executable(runtime_safety_review_tests ../../tests/acz-vr/runtime_safety_review_tests.cpp aircraft_auto.cpp ee_e6_locator.cpp cockpit_model.cpp
    terrain_stage.cpp runtime_config.cpp stage_fingerprint.cpp stage_scan.cpp stage_detect_win.cpp)
  target_include_directories(runtime_safety_review_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME runtime_safety_review COMMAND runtime_safety_review_tests)
  set_tests_properties(runtime_safety_review PROPERTIES TIMEOUT 300)
  set_tests_properties(xr_stereo_terrain_offline PROPERTIES TIMEOUT 60)
  # J-hud-consumer: the HUD quad layer cases alone (also part of the full run above).
  add_test(NAME xr_stereo_hud_layer_offline COMMAND xr_stereo_output_offline_tests --hud)
  set_tests_properties(xr_stereo_hud_layer_offline PROPERTIES TIMEOUT 60)
  # J-feather: edge feather + world-quad distance cases alone (also part of the full run above).
  add_test(NAME xr_stereo_feather_offline COMMAND xr_stereo_output_offline_tests --feather)
  set_tests_properties(xr_stereo_feather_offline PROPERTIES TIMEOUT 60)
  # J-cockpit-fov-consumer: per-eye cockpit FOV submission / refusal cases alone (also in the full run).
  add_test(NAME xr_stereo_cockpit_fov_offline COMMAND xr_stereo_output_offline_tests --cockpit-fov)
  set_tests_properties(xr_stereo_cockpit_fov_offline PROPERTIES TIMEOUT 60)
  # EYE-RES: eyeres:<pct> eye swapchain sizes (recommended view size, quads unchanged, off byte-identical) alone (not in the full run).
  add_test(NAME xr_stereo_eye_res_offline COMMAND xr_stereo_output_offline_tests --eye-res)
  set_tests_properties(xr_stereo_eye_res_offline PROPERTIES TIMEOUT 60)
  # J-hud-placement: HUD distance / scale / head-locked placement keys alone (also in the full run).
  add_test(NAME xr_stereo_hud_placement_offline COMMAND xr_stereo_output_offline_tests --hud-placement)
  add_test(NAME xr_stereo_quad_yaw_offline COMMAND xr_stereo_output_offline_tests --quad-yaw)
  # Timing / anchor fixtures (render pose P0 vs located P1, reused/held metadata, HUD anchor contract, select-corr record) alone (also in the full run).
  add_test(NAME xr_stereo_timing_anchor_offline COMMAND xr_stereo_output_offline_tests --timing-anchor)
  set_tests_properties(xr_stereo_timing_anchor_offline PROPERTIES TIMEOUT 120)
  # POV fixes: F-LEAN anchors (world/marker/HUD quads shifted to the virtual eye) and F-HUD modes alone (also in the full run).
  add_test(NAME xr_stereo_pov_fixes_offline COMMAND xr_stereo_output_offline_tests --pov-fixes)
  set_tests_properties(xr_stereo_pov_fixes_offline PROPERTIES TIMEOUT 120)
  # J-hud-markers: marker quad layer order / registration / refusals alone (also in the full run).
  add_test(NAME xr_stereo_hud_markers_offline COMMAND xr_stereo_output_offline_tests --hud-markers)
  set_tests_properties(xr_stereo_hud_markers_offline PROPERTIES TIMEOUT 60)
  # J-world-in-projection: world composited into the eye images (layer list, ray maths, runs) alone (also in the full run).
  add_test(NAME xr_stereo_world_projection_offline COMMAND xr_stereo_output_offline_tests --world-projection)
  set_tests_properties(xr_stereo_world_projection_offline PROPERTIES TIMEOUT 60)
  # J-effects-layer: effects image composited inside the world-in-projection pass (CPU mirror, config, runs) alone (also in the full run).
  add_test(NAME xr_stereo_effects_offline COMMAND xr_stereo_output_offline_tests --effects)
  set_tests_properties(xr_stereo_effects_offline PROPERTIES TIMEOUT 60)
  # J-world-hold / J-world-extend: hold-last-good fade, periphery fill maths, hold bookkeeping, config keys and the layer-fov line alone (also in the full run).
  add_test(NAME xr_stereo_world_hold_offline COMMAND xr_stereo_output_offline_tests --world-hold)
  set_tests_properties(xr_stereo_world_hold_offline PROPERTIES TIMEOUT 120)
  # DECOUPLE (decouple:1, docs/world-render/decouple.md): re-projection maths, 90 Hz XR over a 60 Hz producer, P1 == P0 no-op, stall/slot lifetime (also in the full run).
  add_test(NAME xr_stereo_decouple_offline COMMAND xr_stereo_output_offline_tests --decouple)
  set_tests_properties(xr_stereo_decouple_offline PROPERTIES TIMEOUT 120)
  set_tests_properties(xr_stereo_hud_placement_offline PROPERTIES TIMEOUT 60)

  # Offline GPU harness: the REAL xr_stereo_d3d12.cpp blit backend + the REAL embedded xr_stereo_blit.hlsl bytecode executed on the Windows
  # WARP software adapter with synthetic producer images (no game, SteamVR or OpenXR runtime). Prints SKIP and returns 0 when WARP cannot
  # create a D3D12 device. acz::nativeDispatch is defined by the harness itself (plain vtable lookup); production code is not changed.
  add_custom_target(acz_vr_blit_shader_headers DEPENDS ${_acz_vr_shader_dir}/xr_stereo_blit_vs.h ${_acz_vr_shader_dir}/xr_stereo_blit_ps.h
    ${_acz_vr_shader_dir}/xr_stereo_cache_ps.h ${_acz_vr_shader_dir}/xr_stereo_fill_ps.h ${_acz_vr_shader_dir}/xr_props_vs.h ${_acz_vr_shader_dir}/xr_props_ps.h
    ${_acz_vr_shader_dir}/cockpit_model_vs.h ${_acz_vr_shader_dir}/cockpit_model_ps.h ${_acz_vr_shader_dir}/cockpit_model_glass_ps.h
    ${_acz_vr_shader_dir}/xr_tactical_vs.h ${_acz_vr_shader_dir}/xr_tactical_ps.h ${_acz_vr_shader_dir}/xr_stamp_vs.h ${_acz_vr_shader_dir}/xr_stamp_ps.h)
  add_executable(xr_composite_warp_harness ../../tests/acz-vr/xr_composite_warp_harness.cpp
    xr_stereo_d3d12.cpp xr_props_d3d12.cpp world_props.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp cockpit_model.cpp aircraft_auto.cpp xr_owner.cpp runtime_config.cpp)
  add_dependencies(xr_composite_warp_harness acz_vr_blit_shader_headers)
  target_include_directories(xr_composite_warp_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_acz_vr_shader_dir})
  target_link_libraries(xr_composite_warp_harness PRIVATE d3d12 dxgi)
  add_test(NAME xr_composite_warp COMMAND xr_composite_warp_harness --out ${CMAKE_CURRENT_BINARY_DIR}/warp-composite-out)
  set_tests_properties(xr_composite_warp PROPERTIES TIMEOUT 300)
  # EFFECTS-OUTSIDE (M6) fxatlas on WARP: the production capture pipeline description + embedded fx_puff_atlas.hlsl bytecode against a stand-in of the
  # game's 4caf6e2d root signature / pixel shader (tests/acz-vr/fx_puff_atlas_warp_game.hlsl): capture, draw from the atlas under our heap, the restore.
  set(_acz_vr_atlas_game_hlsl ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/fx_puff_atlas_warp_game.hlsl)
  add_custom_command(OUTPUT ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_vs.h
    COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T vs_5_0 /E vs_game /Vn g_warp_game_vs /Fh ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_vs.h ${_acz_vr_atlas_game_hlsl}
    DEPENDS ${_acz_vr_atlas_game_hlsl} VERBATIM)
  add_custom_command(OUTPUT ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_ps.h
    COMMAND ${ACZ_VR_FXC} /nologo /WX /O3 /T ps_5_0 /E ps_game /Vn g_warp_game_ps /Fh ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_ps.h ${_acz_vr_atlas_game_hlsl}
    DEPENDS ${_acz_vr_atlas_game_hlsl} VERBATIM)
  add_executable(fx_puff_atlas_warp_harness ../../tests/acz-vr/fx_puff_atlas_warp_harness.cpp
    ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_vs.h ${_acz_vr_shader_dir}/fx_puff_atlas_warp_game_ps.h)
  add_dependencies(fx_puff_atlas_warp_harness acz_vr_atlas_shader_headers)
  target_include_directories(fx_puff_atlas_warp_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_acz_vr_shader_dir})
  target_link_libraries(fx_puff_atlas_warp_harness PRIVATE d3d12 dxgi)
  add_test(NAME fx_puff_atlas_warp COMMAND fx_puff_atlas_warp_harness)
  set_tests_properties(fx_puff_atlas_warp PROPERTIES TIMEOUT 300)
  # GPU-COST (docs/world-render/gpu-cost.md): OPT-IN benchmark of the same backend/shaders on the REAL hardware adapter with the extracted m02a stage,
  # props/trees and model cockpit (D:\acz-extract). Deliberately NOT an add_test: it needs a real GPU and game data; run it by hand.
  add_executable(xr_gpu_bench ../../tests/acz-vr/xr_gpu_bench.cpp
    xr_stereo_d3d12.cpp xr_props_d3d12.cpp world_props.cpp xr_stereo_output.cpp tactical.cpp terrain_stage.cpp cockpit_model.cpp aircraft_auto.cpp xr_owner.cpp runtime_config.cpp)
  add_dependencies(xr_gpu_bench acz_vr_blit_shader_headers)
  target_include_directories(xr_gpu_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_acz_vr_shader_dir})
  target_link_libraries(xr_gpu_bench PRIVATE d3d12 dxgi)

  # J-157: real bounded queues/drains against blocked, failing and partial sinks.
  add_executable(feature_log_offline_tests ../../tests/acz-vr/feature_log_offline_tests.cpp)
  target_include_directories(feature_log_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  foreach(sink_case IN ITEMS slow failing partial)
    add_test(NAME feature_log_${sink_case}_sink COMMAND feature_log_offline_tests ${sink_case})
    add_test(NAME xr_stereo_consumer_${sink_case}_sink
      COMMAND xr_stereo_output_offline_tests --evidence-sink ${sink_case})
    set_tests_properties(feature_log_${sink_case}_sink xr_stereo_consumer_${sink_case}_sink PROPERTIES TIMEOUT 30)
  endforeach()

  add_executable(stereo_math_offline_tests
    ../../tests/acz-vr/stereo_math_offline_tests.cpp stereo_renderer.cpp)
  target_include_directories(stereo_math_offline_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${FETCHCONTENT_SOURCE_DIR_OPENXR}/include)
  add_test(NAME stereo_math_offline COMMAND stereo_math_offline_tests)

  # Sibling hook/proxy fixtures include their production cpp directly.
  # They are absent on the standalone XR branch and need no GPU/XR linkage.
  foreach(fixture IN ITEMS d3d12_hook dxgi_proxy)
    set(fixture_source "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/acz-vr/${fixture}_offline_tests.cpp")
    if(EXISTS "${fixture_source}")
      add_executable(${fixture}_offline_tests "${fixture_source}")
      target_sources(${fixture}_offline_tests PRIVATE draw_capture.cpp shader_metadata.cpp root_binding_metadata.cpp srt_payload.cpp
        native_binding_observer.cpp buffer_binding_registry.cpp)
      target_include_directories(${fixture}_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_link_libraries(${fixture}_offline_tests PRIVATE d3d12)
      add_test(NAME ${fixture}_offline COMMAND ${fixture}_offline_tests)
    endif()
  endforeach()

  foreach(fixture IN ITEMS shader_metadata root_binding_metadata srt_payload)
    add_executable(${fixture}_offline_tests
      ../../tests/acz-vr/${fixture}_offline_tests.cpp ${fixture}.cpp)
    target_include_directories(${fixture}_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${fixture}_offline COMMAND ${fixture}_offline_tests)
  endforeach()
  add_executable(draw_capture_offline_tests ../../tests/acz-vr/draw_capture_offline_tests.cpp
    draw_capture.cpp shader_metadata.cpp root_binding_metadata.cpp srt_payload.cpp)
  target_include_directories(draw_capture_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(draw_capture_offline_tests PRIVATE d3d12)
  add_test(NAME draw_capture_offline COMMAND draw_capture_offline_tests)
  target_sources(draw_capture_offline_tests PRIVATE native_binding_observer.cpp buffer_binding_registry.cpp)
  # Pure CPU snapshot/payload preparation; no D3D12/OpenXR runtime linkage.
  add_executable(stereo_payload_offline_tests
    ../../tests/acz-vr/stereo_payload_offline_tests.cpp stereo_payload.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp)
  target_include_directories(stereo_payload_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME stereo_payload_offline COMMAND stereo_payload_offline_tests)
  # Owned CPU inputs and observed metadata only; no GPU/XR runtime linkage.
  add_executable(capture_input_provider_offline_tests
    ../../tests/acz-vr/capture_input_provider_offline_tests.cpp capture_input_provider.cpp
    native_binding_observer.cpp buffer_binding_registry.cpp draw_capture.cpp
    root_binding_metadata.cpp srt_payload.cpp stereo_payload.cpp)
  target_include_directories(capture_input_provider_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME capture_input_provider_offline COMMAND capture_input_provider_offline_tests)
  # Injected fake byte source only; no GPU/XR runtime linkage.
  add_executable(native_copy_provider_offline_tests
    ../../tests/acz-vr/native_copy_provider_offline_tests.cpp native_copy_provider.cpp
    capture_input_provider.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp)
  target_include_directories(native_copy_provider_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME native_copy_provider_offline COMMAND native_copy_provider_offline_tests)
  # Pure injected-fact replay contract; no D3D12/GPU/XR runtime linkage.
  add_executable(private_replay_contract_offline_tests
    ../../tests/acz-vr/private_replay_contract_offline_tests.cpp private_replay_contract.cpp)
  target_include_directories(private_replay_contract_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME private_replay_contract_offline COMMAND private_replay_contract_offline_tests)
  # Pure fake fault matrix; no D3D12/GPU/XR runtime linkage.
  add_executable(fake_l3_fault_matrix_offline_tests
    ../../tests/acz-vr/fake_l3_fault_matrix_offline_tests.cpp fake_l3_fault_matrix.cpp)
  target_include_directories(fake_l3_fault_matrix_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fake_l3_fault_matrix_offline COMMAND fake_l3_fault_matrix_offline_tests)
  # Offline metadata decision only; no GPU/XR runtime linkage.
  add_executable(capture_enablement_offline_tests
    ../../tests/acz-vr/capture_enablement_offline_tests.cpp capture_enablement.cpp
    native_copy_provider.cpp capture_input_provider.cpp native_binding_observer.cpp
    buffer_binding_registry.cpp draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp)
  target_include_directories(capture_enablement_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME capture_enablement_offline COMMAND capture_enablement_offline_tests)
  # Root decoding uses only SDK CPU serialization/deserialization exports.
  # Every fixture compiling root_binding_metadata.cpp needs the import library.
  foreach(root_consumer IN ITEMS root_binding_metadata stereo_payload capture_input_provider
      native_copy_provider capture_enablement)
    target_link_libraries(${root_consumer}_offline_tests PRIVATE d3d12)
  endforeach()
  add_executable(buffer_binding_registry_offline_tests
    ../../tests/acz-vr/buffer_binding_registry_offline_tests.cpp buffer_binding_registry.cpp)
  target_include_directories(buffer_binding_registry_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME buffer_binding_registry_offline COMMAND buffer_binding_registry_offline_tests)
  # FPS investigation (docs/world-render/fps-investigation.md): indexed registry lookups == the former scans; perf_lean; cost line.
  add_executable(fps_lean_offline_tests
    ../../tests/acz-vr/fps_lean_offline_tests.cpp buffer_binding_registry.cpp runtime_config.cpp)
  target_include_directories(fps_lean_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME fps_lean_offline COMMAND fps_lean_offline_tests)
  # UNCAP / EXTRAP (docs/world-render/uncap.md): option extraction, Present override + gate, Present statistics, sim-rate guard, motion extrapolation.
  add_executable(frame_pacing_offline_tests
    ../../tests/acz-vr/frame_pacing_offline_tests.cpp terrain_stage.cpp runtime_config.cpp)
  target_include_directories(frame_pacing_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME frame_pacing_offline COMMAND frame_pacing_offline_tests)
  set_tests_properties(frame_pacing_offline PROPERTIES TIMEOUT 60)
  # OBJECT-INTERP (interp:1; object_interp.h, docs/world-render/interp.md): header-only tracker / interpolation / camera delta
  add_executable(object_interp_offline_tests ../../tests/acz-vr/object_interp_offline_tests.cpp)
  target_include_directories(object_interp_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME object_interp_offline COMMAND object_interp_offline_tests)
  set_tests_properties(object_interp_offline PROPERTIES TIMEOUT 60)
  add_executable(native_binding_observer_offline_tests
    ../../tests/acz-vr/native_binding_observer_offline_tests.cpp native_binding_observer.cpp buffer_binding_registry.cpp
    draw_capture.cpp shader_metadata.cpp root_binding_metadata.cpp srt_payload.cpp)
  add_library(native_binding_sdk_abi OBJECT ../../tests/acz-vr/native_binding_observer_offline_tests.cpp)
  target_compile_definitions(native_binding_sdk_abi PRIVATE ACZ_BINDING_ABI_CHECK)
  target_sources(native_binding_observer_offline_tests PRIVATE $<TARGET_OBJECTS:native_binding_sdk_abi>)
  target_include_directories(native_binding_observer_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(native_binding_observer_offline_tests PRIVATE d3d12)
  add_test(NAME native_binding_observer_offline COMMAND native_binding_observer_offline_tests)
  foreach(scenario IN ITEMS reentry overlap placed reserved unknown-iid alias extension exception patch restore
    device-extension device-release heap-iid shared-flags proxy-scope proxy-scope-shared proxy-window-expired
    compat-proxy-scope compat-game-overlap compat-xr-window compat-xr-window-expired)
    add_test(NAME native_binding_${scenario}_offline COMMAND native_binding_observer_offline_tests ${scenario})
  endforeach()
  # These fixtures include production hook/dllmain source directly. Their
  # optional diagnostics sink and default decision use the same CPU code.
  foreach(consumer IN ITEMS diagnostics d3d12_hook dxgi_proxy draw_capture native_binding_observer dll_lifecycle)
    target_sources(${consumer}_offline_tests PRIVATE diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp)
    target_compile_definitions(${consumer}_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
    target_link_libraries(${consumer}_offline_tests PRIVATE bcrypt)
  endforeach()
  target_sources(diagnostics_offline_tests PRIVATE draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp
    native_binding_observer.cpp buffer_binding_registry.cpp)
  target_link_libraries(diagnostics_offline_tests PRIVATE d3d12)
    # Additive regression; the original diagnostics fixture remains unchanged.
    add_executable(retirement_snapshot_offline_tests ../../tests/acz-vr/retirement_snapshot_offline_tests.cpp
      diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp root_binding_metadata.cpp
      srt_payload.cpp native_binding_observer.cpp buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
    target_include_directories(retirement_snapshot_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(retirement_snapshot_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
    target_link_libraries(retirement_snapshot_offline_tests PRIVATE d3d12 bcrypt)
    add_test(NAME retirement_snapshot_offline COMMAND retirement_snapshot_offline_tests)
    set_tests_properties(retirement_snapshot_offline PROPERTIES TIMEOUT 30
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:retirement_snapshot_offline_tests>")
    add_executable(stack_storage_offline_tests ../../tests/acz-vr/stack_storage_offline_tests.cpp
      diagnostics_log.cpp runtime_config.cpp shader_metadata.cpp draw_capture.cpp root_binding_metadata.cpp
      srt_payload.cpp native_binding_observer.cpp buffer_binding_registry.cpp capture_enablement.cpp capture_input_provider.cpp)
    target_include_directories(stack_storage_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(stack_storage_offline_tests PRIVATE ACZ_DIAGNOSTICS_OFFLINE_TEST)
    target_link_libraries(stack_storage_offline_tests PRIVATE d3d12 bcrypt)
    add_test(NAME stack_storage_offline COMMAND stack_storage_offline_tests)
    set_tests_properties(stack_storage_offline PROPERTIES TIMEOUT 30
      WORKING_DIRECTORY "$<TARGET_FILE_DIR:stack_storage_offline_tests>")
  target_sources(dll_lifecycle_offline_tests PRIVATE draw_capture.cpp root_binding_metadata.cpp srt_payload.cpp
    native_binding_observer.cpp buffer_binding_registry.cpp)
  target_link_libraries(dll_lifecycle_offline_tests PRIVATE d3d12)
  foreach(consumer IN ITEMS diagnostics d3d12_hook dxgi_proxy draw_capture native_binding_observer dll_lifecycle)
    target_sources(${consumer}_offline_tests PRIVATE capture_enablement.cpp capture_input_provider.cpp)
  endforeach()
  # Hook consumers include d3d12_hook.cpp (some through diagnostics fixtures).
  foreach(consumer IN ITEMS diagnostics_activation diagnostics d3d12_hook dxgi_proxy
      draw_capture native_binding_observer ownership_shutdown retirement_snapshot stack_storage)
    target_sources(${consumer}_offline_tests PRIVATE gpu_copy_ledger.cpp gpu_copy_witness.cpp gpu_copy_owner.cpp)
  endforeach()
  target_sources(gpu_copy_drain_offline_tests PRIVATE gpu_copy_witness.cpp gpu_copy_owner.cpp)
  if(ACZ_VR_BUILD_D3D12_PROXY)
    # J-3b4 wrapper decisions with fakes, and the export-table equality check.
    add_executable(d3d12_proxy_offline_tests ../../tests/acz-vr/d3d12_proxy_offline_tests.cpp)
    target_include_directories(d3d12_proxy_offline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME d3d12_proxy_offline COMMAND d3d12_proxy_offline_tests)
    add_test(NAME d3d12_proxy_exports COMMAND d3d12_proxy_export_check "${ACZ_VR_SYSTEM_D3D12}" "$<TARGET_FILE:acz-d3d12-proxy>")
  endif()
endif()
