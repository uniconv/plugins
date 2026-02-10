#!/bin/bash
# Build static libraries for FFmpeg and its dependencies for standalone
# plugin distribution.
#
# Builds: dav1d (AV1 decoder), SVT-AV1 (AV1 encoder), and optionally
# FFmpeg itself from source (for Linux where system FFmpeg has too many
# transitive deps).
#
# Usage:
#   ./build-ffmpeg-deps.sh                      # Build dav1d + SVT-AV1 only
#   ./build-ffmpeg-deps.sh --build-ffmpeg       # Also build FFmpeg from source
#   ./build-ffmpeg-deps.sh --prefix /p          # Custom install prefix
#   ./build-ffmpeg-deps.sh --clean              # Force rebuild
#
# Default prefix: ./third-party/<platform>/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Pinned versions
DAV1D_VERSION="1.5.1"
SVT_AV1_VERSION="2.3.0"
FFMPEG_VERSION="7.1"

detect_platform() {
    local os arch
    case "$(uname -s)" in
        Linux)  os="linux" ;;
        Darwin) os="darwin" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *) echo "Unsupported OS: $(uname -s)" >&2; exit 1 ;;
    esac
    case "$(uname -m)" in
        x86_64|amd64)  arch="x86_64" ;;
        aarch64|arm64) arch="aarch64" ;;
        *) echo "Unsupported arch: $(uname -m)" >&2; exit 1 ;;
    esac
    echo "${os}-${arch}"
}

PLATFORM=$(detect_platform)
PREFIX="${SCRIPT_DIR}/third-party/${PLATFORM}"
CLEAN=false
BUILD_FFMPEG=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

while [[ $# -gt 0 ]]; do
    case $1 in
        --prefix) PREFIX="$2"; shift 2 ;;
        --clean) CLEAN=true; shift ;;
        --build-ffmpeg) BUILD_FFMPEG=true; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Determine expected output files
EXPECTED_FILES=("$PREFIX/lib/libdav1d.a" "$PREFIX/lib/libSvtAv1Enc.a")
if [[ "$BUILD_FFMPEG" == "true" ]]; then
    EXPECTED_FILES+=("$PREFIX/lib/libavcodec.a")
fi

# Check if already built
if [[ "$CLEAN" == "false" ]]; then
    ALL_EXIST=true
    for f in "${EXPECTED_FILES[@]}"; do
        [[ -f "$f" ]] || { ALL_EXIST=false; break; }
    done
    if [[ "$ALL_EXIST" == "true" ]]; then
        echo "Dependencies already built at $PREFIX"
        ls -lh "$PREFIX/lib/"*.a 2>/dev/null | awk '{print "  " $NF " (" $5 ")"}'
        echo "Use --clean to force rebuild."
        exit 0
    fi
fi

# Check build tools
REQUIRED_TOOLS=(meson ninja cmake)
if [[ "$BUILD_FFMPEG" == "true" ]]; then
    REQUIRED_TOOLS+=(nasm make)
fi
for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool is required but not found." >&2
        exit 1
    fi
done

mkdir -p "$PREFIX"
WORK_DIR=$(mktemp -d)
trap "rm -rf '$WORK_DIR'" EXIT

# Make our custom prefix discoverable for subsequent builds
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---- Build dav1d ----
if [[ "$CLEAN" == "true" ]] || [[ ! -f "$PREFIX/lib/libdav1d.a" ]]; then
    echo "==> Building dav1d v${DAV1D_VERSION} (static)..."
    cd "$WORK_DIR"
    curl -sL "https://code.videolan.org/videolan/dav1d/-/archive/${DAV1D_VERSION}/dav1d-${DAV1D_VERSION}.tar.bz2" -o dav1d.tar.bz2
    tar xjf dav1d.tar.bz2
    cd "dav1d-${DAV1D_VERSION}"
    meson setup build \
        --prefix="$PREFIX" \
        --default-library=static \
        --buildtype=release \
        -Denable_tools=false \
        -Denable_tests=false
    ninja -C build -j"$JOBS"
    ninja -C build install
    echo "  Installed: $(ls -lh "$PREFIX/lib/libdav1d.a" | awk '{print $5}')"
else
    echo "==> dav1d already built, skipping"
fi

# ---- Build SVT-AV1 ----
if [[ "$CLEAN" == "true" ]] || [[ ! -f "$PREFIX/lib/libSvtAv1Enc.a" ]]; then
    echo "==> Building SVT-AV1 v${SVT_AV1_VERSION} (static)..."
    cd "$WORK_DIR"
    curl -sL "https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v${SVT_AV1_VERSION}/SVT-AV1-v${SVT_AV1_VERSION}.tar.bz2" -o svt-av1.tar.bz2
    tar xjf svt-av1.tar.bz2
    cd "SVT-AV1-v${SVT_AV1_VERSION}"
    cmake -B build \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_APPS=OFF \
        -DBUILD_DEC=OFF
    cmake --build build -j"$JOBS"
    cmake --install build
    echo "  Installed: $(ls -lh "$PREFIX/lib/libSvtAv1Enc.a" | awk '{print $5}')"
else
    echo "==> SVT-AV1 already built, skipping"
fi

# ---- Build FFmpeg (optional, for Linux where system FFmpeg has too many deps) ----
if [[ "$BUILD_FFMPEG" == "true" ]]; then
    if [[ "$CLEAN" == "true" ]] || [[ ! -f "$PREFIX/lib/libavcodec.a" ]]; then
        echo "==> Building FFmpeg v${FFMPEG_VERSION} (static)..."
        cd "$WORK_DIR"
        curl -sL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" -o ffmpeg.tar.xz
        tar xJf ffmpeg.tar.xz
        cd "ffmpeg-${FFMPEG_VERSION}"

        CONFIGURE_FLAGS=(
            --prefix="$PREFIX"
            --enable-static
            --disable-shared
            --enable-pic
            --disable-programs
            --disable-doc
            --enable-gpl
            --enable-version3
            # External codec libraries (only what our plugins need)
            --enable-libx264
            --enable-libx265
            --enable-libvpx
            --enable-libopus
            --enable-libmp3lame
            --enable-libdav1d
            --enable-libsvtav1
            --enable-libvorbis
            # Point to our custom-built deps (dav1d, SVT-AV1)
            --extra-cflags="-I$PREFIX/include"
            --extra-ldflags="-L$PREFIX/lib"
        )

        ./configure "${CONFIGURE_FLAGS[@]}"
        make -j"$JOBS"
        make install
        echo "  Installed FFmpeg static libraries:"
        ls -lh "$PREFIX/lib/libav*.a" "$PREFIX/lib/libsw*.a" 2>/dev/null | awk '{print "    " $NF " (" $5 ")"}'
    else
        echo "==> FFmpeg already built, skipping"
    fi
fi

echo ""
echo "==> Done! Static deps installed to: $PREFIX"
ls -lh "$PREFIX/lib/"*.a 2>/dev/null | awk '{print "  " $NF " (" $5 ")"}'
