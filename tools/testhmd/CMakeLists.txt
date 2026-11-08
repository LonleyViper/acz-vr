# acz_testhmd: scriptable-pose SteamVR test HMD (see docs/world-render/test-hmd-driver.md).
# Build (MSVC x64 + Ninja):
#   cmake -S tools/testhmd -B E:/acz-testhmd/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build E:/acz-testhmd/build -j2
#   ctest --test-dir E:/acz-testhmd/build --output-on-failure
# The ready (NOT registered) driver folder is laid out at TESTHMD_STAGE_DIR after every build.
cmake_minimum_required(VERSION 3.20)
project(acz_testhmd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no runtime DLL inside vrserver

set(OPENVR_SDK_DIR "E:/acz-deps/openvr-2.5.1" CACHE PATH "OpenVR SDK root (headers/openvr_driver.h)")
set(TESTHMD_STAGE_DIR "E:/acz-testhmd/driver/acz_testhmd" CACHE PATH "Driver folder to lay out (not registered)")

if(NOT EXISTS "${OPENVR_SDK_DIR}/headers/openvr_driver.h")
  message(FATAL_ERROR "openvr_driver.h not found under ${OPENVR_SDK_DIR}/headers")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

# The driver only uses the header's inline driver-context accessors: no openvr_api.lib/.dll.
add_library(driver_acz_testhmd SHARED src/driver.cpp src/pose_core.h)
target_include_directories(driver_acz_testhmd PRIVATE src "${OPENVR_SDK_DIR}/headers")
target_compile_definitions(driver_acz_testhmd PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(testhmd_test test/pose_core_test.cpp src/pose_core.h)
target_include_directories(testhmd_test PRIVATE src)

enable_testing()
add_test(NAME testhmd_pose_core COMMAND testhmd_test)

# Lay out driver.vrdrivermanifest + resources + bin/win64/driver_acz_testhmd.dll.
add_custom_command(TARGET driver_acz_testhmd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/driver/acz_testhmd" "${TESTHMD_STAGE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${TESTHMD_STAGE_DIR}/bin/win64"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:driver_acz_testhmd>" "${TESTHMD_STAGE_DIR}/bin/win64/"
  COMMENT "Laying out ${TESTHMD_STAGE_DIR} (not registered with SteamVR)")
