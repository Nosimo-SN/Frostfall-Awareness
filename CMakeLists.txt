# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(Frostfall_Awareness VERSION 1.0.1 LANGUAGES CXX)

# SE + AE (flatrim) only; no hooks, so the trampoline patch-safety fetch is unnecessary.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)

# CommonLibSSE-NG (alandtse ng), the lib/commonlibsse-ng submodule (git submodule update --init --recursive).
# Point COMMONLIB_DIR elsewhere to share an existing checkout.
set(COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/commonlibsse-ng" CACHE PATH "CommonLibSSE-NG source")
add_subdirectory(${COMMONLIB_DIR} commonlib)

# A plain source add_subdirectory does not define the helper.
if(NOT COMMAND add_commonlibsse_plugin)
    include(${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake)
endif()

find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME} AUTHOR "nosimo" SOURCES src/plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor /permissive- /EHsc /W4)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
# SkyrimNet C++ API headers from the SkyrimNet devkit (CppAPI/, MIT; see include/SkyrimNet/LICENSE).
target_include_directories(${PROJECT_NAME} PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
