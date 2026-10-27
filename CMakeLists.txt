cmake_minimum_required(VERSION 3.22)
project(armbat LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake's Debug flags are "-g" alone, and this project is an emulator: with -O0
# the JIT, the ELF loader and the inflate of the guest libraries run ten to
# twenty times slower, so a debug build reads as a hang instead of a build with
# symbols. Keep the symbols, keep the optimisation.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")
endif()

if(NOT ANDROID)
    message(FATAL_ERROR "armbat targets Android arm64 only; configure with the NDK toolchain file.")
endif()

# --- Shared warning set ------------------------------------------------------
# Only the project's own targets: the vendored Dynarmic builds with its own flags.
add_library(armbat_warnings INTERFACE)
target_compile_options(armbat_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)

# --- Dynarmic (pinned fork, A32 frontend, arm64 backend) ---------------------
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

# The selftest binary is cross-compiled for the device, so CTest cannot run it;
# the suites are driven by the CLI over adb (see README). Keep the targets, drop
# the testing machinery that would only ever report "exec format error".
add_subdirectory(runtime)
add_subdirectory(tests)
