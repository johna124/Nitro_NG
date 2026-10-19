#!/bin/bash
# compile_windows.sh — Nitro NG v1.09

echo "=== Compiling Nitro NG for Windows x64 (Adaptive Low-RAM / High-Speed) ==="
echo "-> Auto-detects hardware: Low-RAM Mode on low-threads, High-Speed Mode on NVMe/Multi-core"
echo ""

########################################
# Build local optimized zlib for MinGW64
########################################
ZLIB_VERSION="1.3.2"
ZLIB_DIR="$PWD/zlib-mingw64"
ZLIB_SRC="zlib-${ZLIB_VERSION}"

if [ ! -f "$ZLIB_DIR/lib/libz.a" ]; then
    echo "=== Building optimized zlib ${ZLIB_VERSION} for MinGW-w64 ==="   
    cd "$ZLIB_SRC"
    export CC=x86_64-w64-mingw32-gcc
    export AR=x86_64-w64-mingw32-ar
    export RANLIB=x86_64-w64-mingw32-ranlib
    export CFLAGS="-Os -march=core2 -mtune=core2 -fomit-frame-pointer -fno-ident -ffunction-sections -fdata-sections -DNDEBUG"
    ./configure --prefix="$ZLIB_DIR" --static --libdir="$ZLIB_DIR/lib"
    make -j$(nproc)
    make install
    cd ..
fi

########################################
# Toolchain Selector
########################################
echo "Checking 64-bit MinGW toolchain status..."

if dpkg -l | grep -qE "gcc-mingw-w64-x86-64(-posix)?" && dpkg -l | grep -qE "g\+\+-mingw-w64-x86-64(-posix)?"; then
    echo "MinGW 64-bit compiler is already installed. Skipping installation."
else
    echo "MinGW 64-bit not found. Installing toolchain..."
    
    if sudo apt-get install -y gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix 2>/dev/null; then
        echo "MinGW 64-bit POSIX toolchain installed successfully!"
    else
        echo "POSIX packages not found. Installing generic x86_64 MinGW compiler..."
        sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64
    fi
fi

########################################
# Compiler Variable Definition
########################################
if command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then
    CXX="x86_64-w64-mingw32-g++-posix"
else
    CXX="x86_64-w64-mingw32-g++"
fi
echo "Using compiler: $CXX"

########################################
# Find 64-bit winpthread via compiler queries
########################################
echo "Looking for 64-bit winpthread..."
PTHREAD_LIB=""
WINPTHREAD_A=""

for lib in libwinpthread.a libpthread.a; do
    candidate=$("$CXX" -print-file-name="$lib" 2>/dev/null || true)
    if [ -n "$candidate" ] && [ "$candidate" != "$lib" ] && [ -f "$candidate" ]; then
        WINPTHREAD_A="$candidate"
        break
    fi
done

if [ -n "$WINPTHREAD_A" ]; then
    WINPTHREAD_A=$(readlink -f "$WINPTHREAD_A" 2>/dev/null || echo "$WINPTHREAD_A")
    echo "Using winpthread: $WINPTHREAD_A"
    PTHREAD_LIB="$WINPTHREAD_A"
else
    echo "libwinpthread.a not found; falling back to -lpthread"
    PTHREAD_LIB="-lpthread"
fi

########################################
# Flags (Optimized for Low Cache & Extreme Size/RAM Efficiency)
########################################
export CXXFLAGS="-Os -march=core2 -mtune=core2 -fomit-frame-pointer -fno-ident -fno-rtti -ffunction-sections -fdata-sections -flto -DNDEBUG -std=c++20"

########################################
# Link final executable
########################################
echo "Linking final Adaptive Windows 64-bit executable..."
$CXX $CXXFLAGS \
    nitro_ng.cpp \
    -o nitro_ng.exe \
    -I"$ZLIB_DIR/include" \
    "$ZLIB_DIR/lib/libz.a" \
    -municode \
    -mconsole \
    -lpsapi \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--gc-sections \
    -s \
    -Wl,--file-alignment,512 \
    -Wl,--section-alignment,4096 \
    "$PTHREAD_LIB"

echo ""
echo "✅ DONE: nitro_ng.exe built successfully"
echo "📦 Size: $(du -h nitro_ng.exe | awk '{print $1}')"

