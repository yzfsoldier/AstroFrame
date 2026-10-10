# Cross-compiles the emulator for the Steam Frame (aarch64 Linux, glibc) on an x86-64 Linux
# machine with clang and a GCC cross sysroot:
#   Debian/Ubuntu: apt install clang lld g++-aarch64-linux-gnu libc6-dev-arm64-cross
# A sysroot of another kind (for example one exported from the Steam Linux Runtime's arm64 SDK)
# is named with -DSTEAM_FRAME_SYSROOT=<dir>.
#
# What is built this way links against the C library of that sysroot: for a build that runs in
# the Steam Linux Runtime 3.0 (sniper) on the headset, the sysroot has to be sniper's (glibc
# 2.31) or older. Ubuntu's cross packages are newer; they serve to check that it builds, and
# for running outside the runtime on the headset's own SteamOS. See README.md.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(STEAM_FRAME_TRIPLE aarch64-linux-gnu)
set(STEAM_FRAME_SYSROOT "" CACHE PATH "aarch64 sysroot (empty: the GCC cross packages')")
if (STEAM_FRAME_SYSROOT)
    set(CMAKE_SYSROOT "${STEAM_FRAME_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${STEAM_FRAME_SYSROOT}")
else()
    set(CMAKE_FIND_ROOT_PATH "/usr/${STEAM_FRAME_TRIPLE}")
endif()

# clang-NN when the plain names are not there (or STEAM_FRAME_CLANG=clang-19 and the like).
if (DEFINED ENV{STEAM_FRAME_CLANG})
    set(_clang "$ENV{STEAM_FRAME_CLANG}")
else()
    set(_clang clang)
endif()
string(REPLACE "clang" "clang++" _clangxx "${_clang}")
set(CMAKE_C_COMPILER "${_clang}")
set(CMAKE_CXX_COMPILER "${_clangxx}")
set(CMAKE_ASM_COMPILER "${_clang}")
set(CMAKE_C_COMPILER_TARGET ${STEAM_FRAME_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${STEAM_FRAME_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${STEAM_FRAME_TRIPLE})
set(CMAKE_LINKER_TYPE LLD)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config looks in the sysroot only.
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_FIND_ROOT_PATH}/usr/lib/${STEAM_FRAME_TRIPLE}/pkgconfig:${CMAKE_FIND_ROOT_PATH}/usr/share/pkgconfig:${CMAKE_FIND_ROOT_PATH}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_FIND_ROOT_PATH}")

set(CMAKE_CROSSCOMPILING_EMULATOR "")
