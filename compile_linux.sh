#!/bin/bash
set -e

echo "🔧 Compiling Nitro NG for Linux - Hardened for Intel Core 2 Quad Q6600"

########################################
# Toolchain Config
########################################
CXX=g++
CC=gcc
AR=ar

########################################
# Flags (Optimized for Q6600 Architectural Layout)
########################################
# -march=core2: Unlocks native MMX, SSE, SSE2, SSE3, and SSSE3 extensions.
# -fomit-frame-pointer: Frees up the frame pointer register for hot loops.
export CFLAGS="-O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG"
export CXXFLAGS="-O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG -std=c++20"

########################################
# 🔨 Final ELF Link (Zero Windows Overheads)
########################################
echo "🔨 Linking final Linux native binary..."

$CXX $CXXFLAGS \
    nitro_ng_linux.cpp \
    -o nitro_ng-amd64 \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections \
    -s \
    -lz

echo "✅ DONE: nitro_ng-amd64 built successfully for Linux Nativo"
echo "📦 Size: $(du -h nitro_ng-amd64 | awk '{print $1}')"

