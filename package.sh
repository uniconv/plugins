#!/bin/bash
set -euo pipefail

# Package all uniconv plugins for publishing.
#
# Usage:
#   ./package.sh              # Package all plugins
#   ./package.sh ascii        # Package a specific plugin
#
# For native plugins, pass the platform explicitly:
#   PLATFORM=darwin-aarch64 ./package.sh video-convert
#
# Output goes to dist/

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIST_DIR="$SCRIPT_DIR/dist"

# Portable sha256 computation (macOS lacks sha256sum)
compute_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

# Detect platform for native plugins
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

PLATFORM="${PLATFORM:-$(detect_platform)}"

package_cli_plugin() {
    local name="$1"
    local dir="$SCRIPT_DIR/$name"
    local version

    version=$(python3 -c "import json; print(json.load(open('$dir/plugin.json'))['version'])")
    local tarball="$DIST_DIR/${name}-${version}.tar.gz"

    echo "Packaging $name v$version (cli, any) ..."

    tar czf "$tarball" \
        --exclude='__pycache__' \
        --exclude='*.pyc' \
        -C "$SCRIPT_DIR" "$name/"

    local sha256
    sha256=$(compute_sha256 "$tarball")

    echo "  -> $(basename "$tarball")"
    echo "  -> sha256: $sha256"
    echo "$sha256" > "$tarball.sha256"
}

package_native_plugin() {
    local name="$1"
    local dir="$SCRIPT_DIR/$name"
    local version lib_name

    version=$(python3 -c "import json; print(json.load(open('$dir/plugin.json'))['version'])")
    lib_name=$(python3 -c "import json; print(json.load(open('$dir/plugin.json'))['library'])")

    local tarball="$DIST_DIR/${name}-${version}-${PLATFORM}.tar.gz"

    echo "Packaging $name v$version (native, $PLATFORM) ..."

    # Determine shared library extension and find the built library
    local lib_ext lib_file
    case "$PLATFORM" in
        linux-*)   lib_ext="so" ;;
        darwin-*)  lib_ext="dylib" ;;
        windows-*) lib_ext="dll" ;;
    esac
    lib_file="$dir/build/${lib_name}.${lib_ext}"

    if [[ ! -f "$lib_file" ]]; then
        echo "  ERROR: Built library not found at $lib_file" >&2
        echo "  Build the plugin first: cd $name/build && cmake .. && make" >&2
        return 1
    fi

    # Create a staging area with only the distributable files
    local staging
    staging=$(mktemp -d)
    mkdir -p "$staging/$name"
    cp "$dir/plugin.json" "$staging/$name/"
    cp "$dir/manifest.json" "$staging/$name/"
    cp "$lib_file" "$staging/$name/"

    tar czf "$tarball" -C "$staging" "$name/"
    rm -rf "$staging"

    local sha256
    sha256=$(compute_sha256 "$tarball")

    echo "  -> $(basename "$tarball")"
    echo "  -> sha256: $sha256"
    echo "$sha256" > "$tarball.sha256"
}

package_plugin() {
    local name="$1"
    local dir="$SCRIPT_DIR/$name"

    if [[ ! -f "$dir/plugin.json" ]]; then
        echo "ERROR: $dir/plugin.json not found" >&2
        return 1
    fi

    local interface
    interface=$(python3 -c "import json; print(json.load(open('$dir/plugin.json'))['iface'])")

    case "$interface" in
        cli)    package_cli_plugin "$name" ;;
        native) package_native_plugin "$name" ;;
        *)      echo "ERROR: Unknown interface '$interface' for $name" >&2; return 1 ;;
    esac
}

# --- Main ---

mkdir -p "$DIST_DIR"

if [[ $# -gt 0 ]]; then
    for plugin in "$@"; do
        package_plugin "$plugin"
    done
else
    # Package all plugins found in the repo
    for dir in "$SCRIPT_DIR"/*/; do
        name=$(basename "$dir")
        if [[ -f "$dir/plugin.json" ]]; then
            package_plugin "$name"
        fi
    done
fi

echo ""
echo "Done. Artifacts in $DIST_DIR:"
ls -lh "$DIST_DIR"/*.tar.gz 2>/dev/null
