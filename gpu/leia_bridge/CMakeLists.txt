# SPDX-FileCopyrightText: Copyright 2026 Marcus André (Leia3DBridge), gow3 contributors
# SPDX-License-Identifier: MIT
#
# gow3_leia.dll links the Leia SR SDK (MSVC only) and is built separately from the port:
#   cmake -S gpu/leia_bridge -B out/leia-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DLEIASR_SDK_ROOT=<SR SDK with include/sr and lib/>
#   cmake --build out/leia-bridge --config Release

cmake_minimum_required(VERSION 3.20)
project(gow3_leia LANGUAGES CXX)

set(LEIASR_SDK_ROOT "" CACHE PATH "Leia SR SDK root (contains include/sr and lib/)")
# The SR headers include OpenCV (types only); Leia3DBridge keeps it beside the SDK.
set(OPENCV_INCLUDE "${LEIASR_SDK_ROOT}/../opencv/build/include" CACHE PATH "OpenCV include directory")

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Leia bridge is Windows x64 only")
endif()
if (NOT EXISTS "${LEIASR_SDK_ROOT}/include/sr/weaver/glweaver.h")
    message(FATAL_ERROR "Set LEIASR_SDK_ROOT to the Leia SR SDK (include/sr, lib)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gow3_leia SHARED gow3_leia_bridge.cpp gow3_leia_bridge.h)
target_include_directories(gow3_leia PRIVATE "${LEIASR_SDK_ROOT}/include" "${OPENCV_INCLUDE}")
target_link_libraries(gow3_leia PRIVATE
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityCore.lib"
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityDisplays.lib"
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityOpenGL.lib"
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityFaceTrackers.lib"
    opengl32 user32 gdi32)
target_compile_definitions(gow3_leia PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
