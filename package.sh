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

# Convert path to format Python can understand (Windows needs mixed-style paths)
python_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"  # -m outputs forward slashes: D:/a/plugins/...
    else
        echo "$1"
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

# ---------------------------------------------------------------------------
# Shared library bundling: collect transitive deps and patch load paths
# ---------------------------------------------------------------------------

# System libraries to never bundle (Linux)
_LINUX_SYSTEM_LIBS="linux-vdso|ld-linux|libc\\.so|libm\\.so|libdl\\.so|librt\\.so|libpthread\\.so|libstdc\\+\\+|libgcc_s|libmvec\\.so"

_collect_deps_linux() {
    local plugin_lib="$1"
    local dest_dir="$2"

    echo "  Collecting shared library dependencies (Linux)..."

    # Get transitive deps via ldd, filter out system libs
    local deps
    deps=$(ldd "$plugin_lib" 2>/dev/null \
        | grep -oP '\S+\.so\S*\s+=>\s+\K/\S+' \
        | grep -vE "$_LINUX_SYSTEM_LIBS" \
        || true)

    if [[ -z "$deps" ]]; then
        echo "  No non-system dependencies to bundle."
        return
    fi

    # Copy deps (dereference symlinks)
    while IFS= read -r dep; do
        local basename
        basename=$(basename "$dep")
        if [[ ! -f "$dest_dir/$basename" ]]; then
            cp -L "$dep" "$dest_dir/$basename"
            echo "    Bundled: $basename"
        fi
    done <<< "$deps"

    # Patch RPATH on plugin and all bundled libs
    for lib in "$dest_dir"/*.so*; do
        patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
    done
}

_collect_deps_darwin() {
    local plugin_lib="$1"
    local dest_dir="$2"

    echo "  Collecting shared library dependencies (macOS)..."

    # BFS to collect transitive dylib dependencies
    # Avoid bash 4+ features (associative arrays) for macOS /bin/bash compatibility
    local queue="$plugin_lib"
    local visited=""
    local bundle_paths=""  # newline-separated list of absolute paths to bundle

    while [[ -n "$queue" ]]; do
        local current
        current=$(echo "$queue" | head -n 1)
        queue=$(echo "$queue" | tail -n +2)

        local current_real
        current_real=$(realpath "$current" 2>/dev/null || echo "$current")
        # Check if already visited
        case "$visited" in
            *"$current_real"*) continue ;;
        esac
        visited="$visited
$current_real"

        # Get dependencies
        local deps
        deps=$(otool -L "$current" 2>/dev/null \
            | tail -n +2 \
            | awk '{print $1}' \
            || true)

        # Directory of the current library — used to resolve @rpath refs
        local current_dir
        current_dir=$(dirname "$current_real")

        while IFS= read -r dep; do
            [[ -z "$dep" ]] && continue
            # Skip system libs and self-references
            case "$dep" in
                /usr/lib/*|/System/*|@loader_path/*|@executable_path/*) continue ;;
            esac

            # Resolve @rpath/ references by looking in the library's own directory
            # and in common homebrew/system locations
            if [[ "$dep" == @rpath/* ]]; then
                local rpath_name="${dep#@rpath/}"
                local resolved=""
                for search_dir in "$current_dir" /opt/homebrew/lib /usr/local/lib; do
                    if [[ -f "$search_dir/$rpath_name" ]]; then
                        resolved="$search_dir/$rpath_name"
                        break
                    fi
                done
                # Also search homebrew opt dirs for the library
                if [[ -z "$resolved" ]]; then
                    resolved=$(find /opt/homebrew/opt -name "$rpath_name" -type f 2>/dev/null | head -1)
                fi
                if [[ -z "$resolved" ]]; then
                    echo "    WARNING: Could not resolve $dep" >&2
                    continue
                fi
                dep="$resolved"
            fi

            if [[ -f "$dep" ]]; then
                # Add to bundle list if not already there
                case "$bundle_paths" in
                    *"$dep"*) ;;
                    *) bundle_paths="$bundle_paths
$dep" ;;
                esac
                local dep_real
                dep_real=$(realpath "$dep" 2>/dev/null || echo "$dep")
                case "$visited" in
                    *"$dep_real"*) ;;
                    *) queue="$queue
$dep" ;;
                esac
            fi
        done <<< "$deps"
    done

    # Trim leading blank lines
    bundle_paths=$(echo "$bundle_paths" | sed '/^$/d')

    if [[ -z "$bundle_paths" ]]; then
        echo "  No non-system dependencies to bundle."
        return
    fi

    # Copy deps to staging dir
    while IFS= read -r dep_path; do
        local dep_base
        dep_base=$(basename "$dep_path")
        if [[ ! -f "$dest_dir/$dep_base" ]]; then
            cp -L "$dep_path" "$dest_dir/$dep_base"
            echo "    Bundled: $dep_base"
        fi
    done <<< "$bundle_paths"

    # Patch install names: set each bundled dylib's id to @rpath/name
    while IFS= read -r dep_path; do
        local dep_base
        dep_base=$(basename "$dep_path")
        install_name_tool -id "@rpath/$dep_base" "$dest_dir/$dep_base" 2>/dev/null || true
    done <<< "$bundle_paths"

    # Patch references in plugin and all bundled dylibs
    for lib in "$dest_dir"/*.dylib "$plugin_lib"; do
        [[ -f "$lib" ]] || continue
        while IFS= read -r dep_path; do
            local dep_base
            dep_base=$(basename "$dep_path")
            install_name_tool -change "$dep_path" "@rpath/$dep_base" "$lib" 2>/dev/null || true
        done <<< "$bundle_paths"
    done

    # Add @loader_path rpath to each bundled dylib and the plugin
    for lib in "$dest_dir"/*.dylib "$plugin_lib"; do
        [[ -f "$lib" ]] || continue
        # Only add if not already present
        if ! otool -l "$lib" 2>/dev/null | grep -qF '@loader_path'; then
            install_name_tool -add_rpath @loader_path "$lib" 2>/dev/null || true
        fi
    done

    # Strip all RPATHs except @loader_path (removes build-time Homebrew paths
    # that won't exist on other machines)
    for lib in "$dest_dir"/*.dylib "$plugin_lib"; do
        [[ -f "$lib" ]] || continue
        local rpaths
        rpaths=$(otool -l "$lib" 2>/dev/null \
            | awk '/cmd LC_RPATH/{getline;getline;print $2}' || true)
        while IFS= read -r rp; do
            [[ -z "$rp" ]] && continue
            [[ "$rp" == "@loader_path" ]] && continue
            install_name_tool -delete_rpath "$rp" "$lib" 2>/dev/null || true
        done <<< "$rpaths"
    done

    # Neutralize compiled-in Homebrew paths in bundled dylibs.
    # Libraries like libvips and libgio embed absolute paths (e.g.
    # /opt/homebrew/Cellar/vips/.../lib) for runtime module discovery.
    # On the host these resolve to Homebrew-installed modules which link
    # against the *system* glib, causing duplicate-library ObjC class
    # conflicts.  Binary-patch the prefix to a same-length non-existent
    # path so those look-ups safely fail at runtime.
    local brew_prefix
    brew_prefix=$(brew --prefix 2>/dev/null || echo "/opt/homebrew")
    for lib in "$dest_dir"/*.dylib "$plugin_lib"; do
        [[ -f "$lib" ]] || continue
        if LC_ALL=C grep -qc "$brew_prefix" "$lib" 2>/dev/null; then
            chmod u+w "$lib"
            python3 -c "
import sys, pathlib
p = pathlib.Path(sys.argv[1])
prefix = sys.argv[2].encode()
dead = b'/not' + b'_' * (len(prefix) - 4)   # same length, non-existent
data = p.read_bytes()
data = data.replace(prefix, dead)
p.write_bytes(data)
" "$lib" "$brew_prefix"
            echo "    Patched compiled-in paths: $(basename "$lib")"
        fi
    done

    # Re-sign after patching (required on Apple Silicon)
    for lib in "$dest_dir"/*.dylib "$plugin_lib"; do
        [[ -f "$lib" ]] || continue
        codesign -s - -f "$lib" 2>/dev/null || true
    done
}

_collect_deps_windows() {
    local plugin_lib="$1"
    local dest_dir="$2"

    echo "  Collecting shared library dependencies (Windows/MSYS2)..."

    # Get transitive deps via ldd, only bundle from /mingw64/bin
    local deps
    deps=$(ldd "$plugin_lib" 2>/dev/null \
        | grep -oP '=> \K/mingw64/bin/\S+' \
        || true)

    if [[ -z "$deps" ]]; then
        echo "  No MINGW64 dependencies to bundle."
        return
    fi

    while IFS= read -r dep; do
        local basename
        basename=$(basename "$dep")
        if [[ ! -f "$dest_dir/$basename" ]]; then
            cp -L "$dep" "$dest_dir/$basename"
            echo "    Bundled: $basename"
        fi
    done <<< "$deps"
    # No patching needed — Windows searches DLL's directory automatically
}

collect_and_patch_deps() {
    local plugin_lib="$1"
    local dest_dir="$2"

    case "$PLATFORM" in
        linux-*)   _collect_deps_linux "$plugin_lib" "$dest_dir" ;;
        darwin-*)  _collect_deps_darwin "$plugin_lib" "$dest_dir" ;;
        windows-*) _collect_deps_windows "$plugin_lib" "$dest_dir" ;;
    esac
}

# ---------------------------------------------------------------------------

_find_pkg_lib() {
    local pkg_config_name="$1"
    local lib_name="$2"
    local lib_dir lib_ext

    lib_dir=$(pkg-config --variable=libdir "$pkg_config_name" 2>/dev/null || true)
    [[ -z "$lib_dir" ]] && return 1

    case "$PLATFORM" in
        darwin-*)  lib_ext="dylib" ;;
        linux-*)   lib_ext="so" ;;
        windows-*) lib_ext="dll" ;;
    esac

    # Find the versioned shared library (not symlink)
    local found
    found=$(find "$lib_dir" -maxdepth 1 -name "lib${lib_name}*.${lib_ext}*" -not -type l 2>/dev/null | head -1)
    [[ -n "$found" ]] && echo "$found"
}

package_cli_plugin() {
    local name="$1"
    local dir="$SCRIPT_DIR/$name"
    local version pypath

    pypath=$(python_path "$dir/plugin.json")
    version=$(python3 -c "import json; print(json.load(open('$pypath'))['version'])")

    # Check if this CLI plugin has bundled native libs
    local bundled_libs
    bundled_libs=$(python3 -c "
import json, sys
d = json.load(open('$pypath'))
libs = d.get('bundled_libs', [])
for lib in libs:
    print(lib['pkg_config'] + ':' + lib['lib_name'])
" 2>/dev/null || true)

    if [[ -n "$bundled_libs" ]]; then
        # Platform-specific packaging with bundled native libs
        local tarball="$DIST_DIR/${name}-${version}-${PLATFORM}.tar.gz"

        echo "Packaging $name v$version (cli+native, $PLATFORM) ..."

        local staging
        staging=$(mktemp -d)
        mkdir -p "$staging/$name/lib"

        # Copy plugin files
        for f in "$dir"/*.py "$dir"/plugin.json "$dir"/manifest.json; do
            [[ -f "$f" ]] && cp "$f" "$staging/$name/"
        done

        # Bundle each native lib
        while IFS= read -r entry; do
            [[ -z "$entry" ]] && continue
            local pkg_config_name="${entry%%:*}"
            local lib_name="${entry##*:}"

            local lib_path
            lib_path=$(_find_pkg_lib "$pkg_config_name" "$lib_name")
            if [[ -z "$lib_path" ]]; then
                echo "  ERROR: Could not find lib for pkg-config=$pkg_config_name lib=$lib_name" >&2
                rm -rf "$staging"
                return 1
            fi

            local lib_base
            lib_base=$(basename "$lib_path")
            cp -L "$lib_path" "$staging/$name/lib/$lib_base"
            echo "  Bundled: $lib_base"

            # Collect transitive deps and patch load paths
            collect_and_patch_deps "$staging/$name/lib/$lib_base" "$staging/$name/lib"
        done <<< "$bundled_libs"

        tar czf "$tarball" \
            --exclude='__pycache__' \
            --exclude='*.pyc' \
            -C "$staging" "$name/"
        rm -rf "$staging"

        local sha256
        sha256=$(compute_sha256 "$tarball")

        echo "  -> $(basename "$tarball")"
        echo "  -> sha256: $sha256"
        echo "$sha256" > "$tarball.sha256"
    else
        # Platform-independent packaging (no native libs)
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
    fi
}

package_native_plugin() {
    local name="$1"
    local dir="$SCRIPT_DIR/$name"
    local version lib_name pypath

    pypath=$(python_path "$dir/plugin.json")
    version=$(python3 -c "import json; print(json.load(open('$pypath'))['version'])")
    lib_name=$(python3 -c "import json; print(json.load(open('$pypath'))['library'])")

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

    # Bundle shared library dependencies alongside the plugin
    collect_and_patch_deps "$staging/$name/${lib_name}.${lib_ext}" "$staging/$name"

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

    local interface pypath
    pypath=$(python_path "$dir/plugin.json")
    interface=$(python3 -c "import json; print(json.load(open('$pypath'))['interface'])")

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
