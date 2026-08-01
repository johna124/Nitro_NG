#!/bin/bash
set -e

echo "🔧 Cross-compiling Nitro NG for Windows (MinGW) - Hardened for Core 2 Quad"

########################################
# Toolchain
########################################
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
AR=x86_64-w64-mingw32-ar

########################################
# Flags (Optimized for Q6600 & Lossless Safety)
########################################
# -march=core2: Habilita MMX, SSE, SSE2, SSE3, SSSE3.
# -fomit-frame-pointer: Registro extra para CPUs con pocos registros generales.
export CFLAGS="-O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG"
export CXXFLAGS="-O3 -march=core2 -mtune=core2 -fomit-frame-pointer -DNDEBUG -std=c++20"

########################################
# 🔨 Final link
########################################
echo "🔨 Linking final Windows executable..."

$CXX $CXXFLAGS \
    nitro_ng_windows.cpp \
    -o nitro_ng.exe \
    -municode \
    -mconsole \
    -l:libz.a \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections \
    -s \
    -Wl,--stack,4194304

echo "✅ DONE: nitro_ng.exe built successfully"
echo "📦 Size: $(du -h nitro_ng.exe | awk '{print $1}')"

