#!/bin/sh
set -eu

TOOLCHAIN=${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to the OpenOrbis toolchain}
COMPILER=${CXX:-clang++}
REPO_ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BASE=${1:-build/deps/torrent}
CACHE=${ORBIT_CACHE_DIR:-${TMPDIR:-/tmp}/orbit-cache}

mkdir -p "$CACHE" "$BASE"
BASE=$(CDPATH= cd -- "$BASE" && pwd)
SOURCE="$BASE/libtorrent-source"
BOOST="$BASE/boost"
BUILD="$BASE/libtorrent-build"

mkdir -p "$SOURCE" "$BUILD"

verify_hash() {
    python3 - "$1" "$2" <<'PY'
import hashlib
import sys

path, expected = sys.argv[1:]
digest = hashlib.sha256()
with open(path, "rb") as source:
    for chunk in iter(lambda: source.read(1024 * 1024), b""):
        digest.update(chunk)
if digest.hexdigest() != expected:
    raise SystemExit("SHA-256 mismatch: " + path)
PY
}

fetch() {
    name=$1
    url=$2
    digest=$3
    archive="$CACHE/$name"
    if [ ! -f "$archive" ] || ! verify_hash "$archive" "$digest" 2>/dev/null; then
        curl --fail --location --retry 3 --silent --show-error "$url" -o "$archive.part"
        verify_hash "$archive.part" "$digest"
        mv "$archive.part" "$archive"
    fi
    printf '%s\n' "$archive"
}

LIBTORRENT_ARCHIVE=$(fetch libtorrent-2.0.11.tar.gz \
    https://github.com/arvidn/libtorrent/archive/9d7443f467147d1784fb7516d2a882db1abb5a8b.tar.gz \
    27669daa833531738ae51b29c55f438b07c69c6998b5bac8cb54a69568a7f890)
BOOST_ARCHIVE=$(fetch boost-1.89.0.tar.gz \
    https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.gz \
    9de758db755e8330a01d995b0a24d09798048400ac25c03fc5ea9be364b13c93)
TRY_SIGNAL_ARCHIVE=$(fetch try_signal-105cce5.tar.gz \
    https://github.com/arvidn/try_signal/archive/105cce59972f925a33aa6b1c3109e4cd3caf583d.tar.gz \
    6f111b0d77429a8051be4faed06cf23fb03cf6ee233967c84fdd9d8d3b42ba8e)

if [ ! -f "$SOURCE/CMakeLists.txt" ]; then
    tar -xzf "$LIBTORRENT_ARCHIVE" -C "$SOURCE" --strip-components=1
fi
if [ ! -f "$SOURCE/deps/try_signal/try_signal.cpp" ]; then
    mkdir -p "$SOURCE/deps/try_signal"
    tar -xzf "$TRY_SIGNAL_ARCHIVE" -C "$SOURCE/deps/try_signal" --strip-components=1
fi
if [ ! -f "$BOOST/boost/version.hpp" ]; then
    mkdir -p "$BOOST"
    tar -xzf "$BOOST_ARCHIVE" -C "$BOOST" --strip-components=1 boost_1_89_0/boost
fi
if ! grep -q 'Leave routes empty' "$SOURCE/src/enum_net.cpp"; then
    patch -d "$SOURCE" --forward -p1 < "$REPO_ROOT/patches/libtorrent-openorbis.patch"
fi

mkdir -p "$BASE/compat/sys"
: > "$BASE/compat/sys/sysctl.h"
CXXFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -D_GNU_SOURCE -DORBIS -DFIONBIO=0x8004667e -DFIONREAD=0x4004667f -DTORRENT_USE_IFADDRS=1 -DTORRENT_USE_SYSCTL=0 -DTORRENT_USE_IFCONF=0 -DTORRENT_HAS_PTHREAD_SET_NAME=0 -isysroot $TOOLCHAIN -isystem $TOOLCHAIN/include -isystem $TOOLCHAIN/include/c++/v1 -isystem $BOOST -I$BASE/compat"

cmake -S "$SOURCE" -B "$BUILD" \
    -DCMAKE_SYSTEM_NAME=FreeBSD \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_CXX_COMPILER="$COMPILER" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DCMAKE_CXX_STANDARD=14 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DBoost_INCLUDE_DIR="$BOOST" \
    -DBoost_NO_BOOST_CMAKE=ON \
    -DBoost_NO_SYSTEM_PATHS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -Dbuild_tests=OFF \
    -Dbuild_examples=OFF \
    -Dbuild_tools=OFF \
    -Ddht=ON \
    -Dencryption=OFF \
    -Dgnutls=OFF \
    -Dexceptions=ON \
    -Ddeprecated-functions=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_GnuTLS=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_LibGcrypt=TRUE
cmake --build "$BUILD" --target torrent-rasterbar --parallel "${ORBIT_BUILD_JOBS:-4}"
cp "$BUILD/libtorrent-rasterbar.a" "$BASE/libtorrent-rasterbar.a"
