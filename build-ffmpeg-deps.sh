#!/bin/bash
# Build static libraries for FFmpeg transitive dependencies that aren't
# typically available as static (.a) from package managers.
#
# Currently builds: dav1d (AV1 decoder), SVT-AV1 (AV1 encoder)
#
# Usage:
#   ./build-ffmpeg-deps.sh              # Build to default prefix
#   ./build-ffmpeg-deps.sh --prefix /p  # Build to custom prefix
#   ./build-ffmpeg-deps.sh --clean      # Force rebuild
#
# Default prefix: ./third-party/<platform>/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Pinned versions (match what Homebrew FFmpeg 8.x links against)
DAV1D_VERSION="1.5.1"
SVT_AV1_VERSION="4.0.1"

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
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

while [[ $# -gt 0 ]]; do
    case $1 in
        --prefix) PREFIX="$2"; shift 2 ;;
        --clean) CLEAN=true; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# Check if already built
if [[ "$CLEAN" == "false" ]] && \
   [[ -f "$PREFIX/lib/libdav1d.a" ]] && \
   [[ -f "$PREFIX/lib/libSvtAv1Enc.a" ]]; then
    echo "Dependencies already built at $PREFIX"
    echo "  $(ls -lh "$PREFIX/lib/libdav1d.a" "$PREFIX/lib/libSvtAv1Enc.a" | awk '{print $NF, $5}')"
    echo "Use --clean to force rebuild."
    exit 0
fi

# Check build tools
for tool in meson ninja cmake; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool is required but not found." >&2
        echo "  macOS: brew install $tool" >&2
        echo "  Linux: apt-get install $tool" >&2
        exit 1
    fi
done

mkdir -p "$PREFIX"
WORK_DIR=$(mktemp -d)
trap "rm -rf '$WORK_DIR'" EXIT

# ---- Build dav1d ----
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

# ---- Build SVT-AV1 ----
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

echo ""
echo "==> Done! Static deps installed to: $PREFIX"
ls -lh "$PREFIX/lib/"*.a
