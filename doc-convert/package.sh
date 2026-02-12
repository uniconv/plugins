#!/bin/bash
# doc-convert packaging hook
# Bundles LibreOffice and Pandoc into the staging directory.
# Called by the top-level package.sh via _run_plugin_hook().
#
# $1 = staging directory (e.g. /tmp/xxx/doc-convert)

set -euo pipefail

STAGING="$1"
BIN_DIR="$STAGING/bin"
mkdir -p "$BIN_DIR"

echo "  Bundling binaries for doc-convert ($PLATFORM) ..."

# ---------------------------------------------------------------------------
# Pandoc
# ---------------------------------------------------------------------------

bundle_pandoc() {
    local pandoc_path
    pandoc_path=$(command -v pandoc 2>/dev/null || true)
    if [[ -z "$pandoc_path" ]]; then
        echo "  WARNING: pandoc not found, skipping pandoc bundling" >&2
        return
    fi
    cp -L "$pandoc_path" "$BIN_DIR/pandoc"
    chmod +x "$BIN_DIR/pandoc"
    echo "    Bundled: pandoc"

    # Collect transitive shared-lib deps and patch load paths
    # (same approach as image-convert's native library bundling)
    collect_and_patch_deps "$BIN_DIR/pandoc" "$BIN_DIR"
}

# ---------------------------------------------------------------------------
# LibreOffice – strip to essentials
# ---------------------------------------------------------------------------

# Patterns to remove (saves ~60% of installed size)
_LO_STRIP_DIRS=(
    gallery
    template
    autocorr
    autotext
    help
    wizards
    basic/Gimmicks
    basic/Template
    basic/Tutorials
    basic/FormWizard
    basic/ImportWizard
    basic/Depot
    basic/Euro
    basic/Access2Base
    # Non-English UI locales — keep en-US
    "registry/res/registry_*.xcd"
)

_LO_STRIP_GLOBS=(
    "*.bau"           # Base wizards
    "libscriptframe*" # macro scripting framework
    "libbasctl*"      # Basic IDE
    "libdbp*"         # Base
    "libdba*"         # Base
    "libmath*"        # Math component
)

strip_libreoffice() {
    local lo_dest="$1"

    echo "    Stripping LibreOffice ..."

    # Remove known unnecessary directories
    for pattern in "${_LO_STRIP_DIRS[@]}"; do
        # Use find to handle both dirs and glob patterns
        while IFS= read -r match; do
            [[ -e "$match" ]] && rm -rf "$match"
        done < <(find "$lo_dest" -path "*/$pattern" 2>/dev/null || true)
    done

    # Remove unnecessary files by glob
    for pattern in "${_LO_STRIP_GLOBS[@]}"; do
        find "$lo_dest" -name "$pattern" -delete 2>/dev/null || true
    done

    # Remove non-en-US resource files
    find "$lo_dest" -name "*.res" ! -name "*en_US*" ! -name "*en-US*" -delete 2>/dev/null || true

    # Strip debug symbols on Linux
    if [[ "$PLATFORM" == linux-* ]]; then
        find "$lo_dest" \( -name "*.so" -o -name "*.so.*" \) -exec strip --strip-debug {} + 2>/dev/null || true
    fi
}

bundle_libreoffice_darwin() {
    local lo_app="/Applications/LibreOffice.app/Contents"
    if [[ ! -d "$lo_app" ]]; then
        echo "  ERROR: LibreOffice.app not found at /Applications/LibreOffice.app" >&2
        return 1
    fi

    local lo_dest="$BIN_DIR/libreoffice"
    mkdir -p "$lo_dest"

    echo "    Copying LibreOffice (macOS) ..."

    # Copy essential directories
    for subdir in MacOS Frameworks Resources; do
        if [[ -d "$lo_app/$subdir" ]]; then
            cp -RL "$lo_app/$subdir" "$lo_dest/$subdir"
        fi
    done

    strip_libreoffice "$lo_dest"

    # Collect transitive deps for soffice and patch load paths
    if [[ -f "$lo_dest/MacOS/soffice" ]]; then
        collect_and_patch_deps "$lo_dest/MacOS/soffice" "$lo_dest/Frameworks"
    fi

    # Re-sign after stripping/patching (required on Apple Silicon)
    find "$lo_dest" \( -name "*.dylib" -o -name "soffice" \) -exec codesign -s - -f {} + 2>/dev/null || true

    echo "    Bundled: libreoffice (macOS)"
}

bundle_libreoffice_linux() {
    local lo_prefix="/usr/lib/libreoffice"
    if [[ ! -d "$lo_prefix" ]]; then
        echo "  ERROR: LibreOffice not found at $lo_prefix" >&2
        return 1
    fi

    local lo_dest="$BIN_DIR/libreoffice"
    mkdir -p "$lo_dest"

    echo "    Copying LibreOffice (Linux) ..."

    # Copy essential directories
    for subdir in program share presets; do
        if [[ -d "$lo_prefix/$subdir" ]]; then
            cp -RL "$lo_prefix/$subdir" "$lo_dest/$subdir"
        fi
    done

    strip_libreoffice "$lo_dest"

    # Collect transitive deps for the soffice binary and patch load paths
    if [[ -f "$lo_dest/program/soffice.bin" ]]; then
        collect_and_patch_deps "$lo_dest/program/soffice.bin" "$lo_dest/program"
    elif [[ -f "$lo_dest/program/soffice" ]]; then
        collect_and_patch_deps "$lo_dest/program/soffice" "$lo_dest/program"
    fi

    # Ensure all LO shared libs have RPATH set to $ORIGIN
    if command -v patchelf >/dev/null 2>&1; then
        echo "    Patching RPATHs ..."
        find "$lo_dest/program" \( -name "*.so" -o -name "*.so.*" \) 2>/dev/null | while IFS= read -r lib; do
            patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
        done
    fi

    echo "    Bundled: libreoffice (Linux)"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

bundle_pandoc

case "$PLATFORM" in
    darwin-*)
        bundle_libreoffice_darwin
        ;;
    linux-*)
        bundle_libreoffice_linux
        ;;
    windows-*)
        echo "  Skipping LibreOffice bundling on Windows (use system install)"
        ;;
    *)
        echo "  WARNING: Unknown platform $PLATFORM, skipping LibreOffice bundling" >&2
        ;;
esac

echo "  Bundled binaries size: $(du -sh "$BIN_DIR" | awk '{print $1}')"
