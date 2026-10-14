# Cross build for the Sipeed Maix-II Dock (V831, musl).
#
# Prerequisite: the sunxi musl toolchain from the pinned release
#   toolchain-sunxi-musl-pack-2021-01-09.tar.xz
# unpacked somewhere, and HV_TOOLCHAIN_DIR pointing at it.
#
#   cmake -S device -B build/v831 \
#     -DCMAKE_TOOLCHAIN_FILE=device/cmake/toolchain-v831.cmake \
#     -DHV_TOOLCHAIN_DIR=/opt/toolchain-sunxi-musl
#
# The toolchain version and download link are pinned in
# docs/homevideo-design.md §4.1.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(HV_TOOLCHAIN_DIR "/opt/toolchain-sunxi-musl" CACHE PATH "sunxi musl toolchain root")
set(HV_TOOLCHAIN_PREFIX "arm-openwrt-linux-muslgnueabi-" CACHE STRING "toolchain program prefix")

set(HV_TOOLCHAIN_BIN "${HV_TOOLCHAIN_DIR}/bin")
set(CMAKE_C_COMPILER "${HV_TOOLCHAIN_BIN}/${HV_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${HV_TOOLCHAIN_BIN}/${HV_TOOLCHAIN_PREFIX}g++")

set(CMAKE_FIND_ROOT_PATH "${HV_TOOLCHAIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# The board image has no libstdc++ we can rely on, so link it statically.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")

# Selecting this toolchain implies the board backends.
set(HV_PLATFORM "v831" CACHE STRING "Target platform: desktop or v831" FORCE)

if(NOT EXISTS "${CMAKE_CXX_COMPILER}")
  message(FATAL_ERROR
    "V831 toolchain not found at ${CMAKE_CXX_COMPILER}.\n"
    "Download toolchain-sunxi-musl-pack-2021-01-09.tar.xz (see docs/homevideo-design.md §4.1), "
    "unpack it, then pass -DHV_TOOLCHAIN_DIR=<path>.")
endif()
