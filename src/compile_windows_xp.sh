#!/bin/bash
# compile_win_xp Nitro NG v1.09 Windows XP (32-bit)
set -e

echo "=== Compiling Nitro NG for Windows XP (32-bit) (Adaptive Low-RAM / High-Speed) ==="
echo "-> Auto-detects hardware: Low-RAM Mode on low-threads, High-Speed Mode on NVMe/Multi-core"
echo ""

########################################
# Install toolchain
########################################
echo "Checking 32-bit MinGW toolchain status..."

if dpkg -l | grep -qE "gcc-mingw-w64-i686(-posix)?" && dpkg -l | grep -qE "g\+\+-mingw-w64-i686(-posix)?"; then
    echo "MinGW 32-bit compiler is already installed. Skipping installation."
else
    echo "MinGW not found. Installing 32-bit MinGW toolchain..."
    
     if sudo apt-get install -y gcc-mingw-w64-i686-posix g++-mingw-w64-i686-posix 2>/dev/null; then
        echo "MinGW POSIX toolchain installed successfully!"
    else
        echo "POSIX packages not found. Installing generic MinGW compiler..."
        sudo apt-get install -y gcc-mingw-w64-i686 g++-mingw-w64-i686
    fi
fi

########################################
# Build local optimized zlib for MinGW32
########################################
ZLIB_VERSION="1.3.2"
ZLIB_DIR="$PWD/zlib-mingw32"
ZLIB_SRC="zlib-${ZLIB_VERSION}"

if [ ! -f "$ZLIB_DIR/lib/libz.a" ]; then
    echo "=== Building zlib ${ZLIB_VERSION} for MinGW-w64 (32-bit, Core 2 Quad) ===" 
    cd "$ZLIB_SRC"
    
    export CC=i686-w64-mingw32-gcc
    export AR=i686-w64-mingw32-ar
    export RANLIB=i686-w64-mingw32-ranlib
    export CFLAGS="-Os -march=core2 -mtune=core2 -fomit-frame-pointer -fno-ident -ffunction-sections -fdata-sections -DNDEBUG"
    
    echo "Configuring with CC=$CC, CFLAGS=$CFLAGS"
    ./configure --prefix="$ZLIB_DIR" --static --libdir="$ZLIB_DIR/lib"
    
    make -j$(nproc)
    make install
    
    cd ..
    
    echo "OK: zlib ${ZLIB_VERSION} built for MinGW-w64 32-bit"
    echo ""
else
    echo "OK: Using existing local zlib: $ZLIB_DIR/lib/libz.a"
    echo ""
fi

########################################
# Toolchain
########################################
CXX=i686-w64-mingw32-g++-posix
if ! command -v "$CXX" >/dev/null 2>&1; then
    CXX=i686-w64-mingw32-g++
    echo "WARNING: i686-w64-mingw32-g++-posix not found."
    echo "Using $CXX. If std::thread fails, install the POSIX variant."
fi
echo "Compiler: $CXX"
"$CXX" -dumpmachine

########################################
# Find 32-bit winpthread
########################################
echo "Looking for 32-bit winpthread"
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
fi

if [ -z "$WINPTHREAD_A" ] && command -v locate >/dev/null 2>&1; then
    WINPTHREAD_A=$(locate /i686-w64-mingw32/lib/libwinpthread.a 2>/dev/null | head -n1 || true)
    if [ -n "$WINPTHREAD_A" ] && [ ! -f "$WINPTHREAD_A" ]; then WINPTHREAD_A=""; fi
fi

if [ -z "$WINPTHREAD_A" ]; then
    WINPTHREAD_A=$(find /usr/i686-w64-mingw32 /usr/lib/gcc/i686-w64-mingw32 -name libwinpthread.a -print -quit 2>/dev/null || true)
fi

if [ -n "$WINPTHREAD_A" ]; then
    echo "Using winpthread: $WINPTHREAD_A"
    PTHREAD_LIB="$WINPTHREAD_A"
else
    echo "libwinpthread.a not found; falling back to -lpthread"
    PTHREAD_LIB="-lpthread"
fi

########################################
# Optional libatomic test
########################################
ATOMIC_LIB=""
if printf 'int main(){return 0;}' > /tmp/nitro_atomic_test.cpp && \
"$CXX" /tmp/nitro_atomic_test.cpp -o /tmp/nitro_atomic_test.exe -static -latomic 2>/dev/null; then
    ATOMIC_LIB="-latomic"
    echo "Using -latomic"
else
    echo "Not using -latomic"
fi

########################################
# Flags 
########################################
ARCH_FLAGS="-march=core2 -mtune=core2"
XP_DEFS="-DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -D_WIN32_IE=0x0600"
UNICODE_DEFS="-DUNICODE -D_UNICODE"
MISC_DEFS="-DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNDEBUG"

CXXFLAGS="-std=c++20 -Os $ARCH_FLAGS -fomit-frame-pointer -fno-ident -fno-rtti"
CXXFLAGS="$CXXFLAGS -ffunction-sections -fdata-sections -flto"
CXXFLAGS="$CXXFLAGS $XP_DEFS $UNICODE_DEFS $MISC_DEFS"
CXXFLAGS="$CXXFLAGS -pthread"

LDFLAGS="-municode -mconsole"
LDFLAGS="$LDFLAGS -static -static-libgcc -static-libstdc++"
LDFLAGS="$LDFLAGS -pthread -lpsapi"
LDFLAGS="$LDFLAGS -Wl,--gc-sections"
LDFLAGS="$LDFLAGS -Wl,--large-address-aware"
LDFLAGS="$LDFLAGS -Wl,--stack,2097152"
LDFLAGS="$LDFLAGS -Wl,--major-os-version,5"
LDFLAGS="$LDFLAGS -Wl,--minor-os-version,1"
LDFLAGS="$LDFLAGS -Wl,--major-subsystem-version,5"
LDFLAGS="$LDFLAGS -Wl,--minor-subsystem-version,1"

LDFLAGS="$LDFLAGS -Wl,--file-alignment,512 -Wl,--section-alignment,4096"
LDFLAGS="$LDFLAGS -s"

########################################
# Build
########################################
echo ""
echo "Building nitro_ng_xp.exe..."
echo ""
"$CXX" $CXXFLAGS nitro_ng.cpp \
-o nitro_ng_xp.exe \
-I"$ZLIB_DIR/include" \
"$ZLIB_DIR/lib/libz.a" \
$LDFLAGS \
"$PTHREAD_LIB" \
$ATOMIC_LIB

echo ""
echo "Build finished."

########################################
# Basic checks
########################################
echo ""
echo "File type:"
file nitro_ng_xp.exe || true

echo ""
echo "PE header info:"
i686-w64-mingw32-objdump -p nitro_ng_xp.exe | \
grep -Ei 'file format|machine|Subsystem|MajorOSVersion|MinorOSVersion|MajorSubsystemVersion|MinorSubsystemVersion' || true

echo ""
echo "DLL imports:"
i686-w64-mingw32-objdump -p nitro_ng_xp.exe | grep 'DLL Name' || true

echo ""
echo "Checking for obvious post-XP API imports:"
if i686-w64-mingw32-objdump -p nitro_ng_xp.exe | \
grep -Ei 'SleepConditionVariable|InitializeSRWLock|InitOnceExecuteOnce|FlsAlloc|CreateThreadpool|WaitForThreadpool|api-ms-win'; then
    echo ""
    echo "WARNING: found possible Vista/7+ API imports."
    echo "This EXE may fail on Windows XP."
else
    echo "No obvious Vista/7+ imports detected."
fi

echo ""
echo "✅ DONE: nitro_ng_xp.exe built successfully"
echo "📦 Size: $(du -h nitro_ng_xp.exe | awk '{print $1}')"

