#!/bin/bash
# compile_linux_universal.sh — Nitro NG v1.09
set -e

echo "=== Compiling Nitro NG for Linux Native (Adaptive Low-RAM / High-Speed) ==="
echo "-> Auto-detects hardware: Low-RAM Mode on low-threads, High-Speed Mode on NVMe/Multi-core"
echo ""

ZLIB_VERSION="1.3.2"
ZLIB_DIR="$PWD/zlib-local"
ZLIB_SRC="zlib-${ZLIB_VERSION}"

if [ ! -f "$ZLIB_DIR/lib/libz.a" ]; then
    echo "=== Building optimized zlib ${ZLIB_VERSION} for Linux ===" 
    cd "$ZLIB_SRC"
    export CC=gcc
    export CFLAGS="-Os -march=core2 -mtune=core2 -fomit-frame-pointer -fno-ident -ffunction-sections -fdata-sections -DNDEBUG"
    ./configure --prefix="$ZLIB_DIR" --static --libdir="$ZLIB_DIR/lib"
    make -j$(nproc)
    make install
    cd ..
fi

export CXXFLAGS="-Os -march=core2 -mtune=core2 -fomit-frame-pointer -fno-ident -fno-rtti -ffunction-sections -fdata-sections -flto -DNDEBUG -std=c++20"

echo "🔨 Linking final Adaptive Linux native binary..."
g++ $CXXFLAGS nitro_ng.cpp -o nitro_ng-amd64 -I"$ZLIB_DIR/include" "$ZLIB_DIR/lib/libz.a" -static -static-libgcc -static-libstdc++ -pthread -Wl,--gc-sections -Wl,--hash-style=gnu -Wl,-z,norelro -s

echo ""
echo "✅ DONE: nitro_ng-amd64 built successfully"
echo "📦 Size: $(du -h nitro_ng-amd64 | awk '{print $1}')"

