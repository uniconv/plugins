#!/bin/bash
# Plugin-specific packaging hook for image-convert.
# Sourced by the top-level package.sh with $1 = staging directory.
# Shared helpers (patch_module_deps, etc.) are available.

local staging_dir="$1"

# Bundle libvips dynamic modules (heif, jxl, poppler, etc.)
local vips_libdir
vips_libdir=$(pkg-config --variable=libdir vips 2>/dev/null || true)
if [[ -z "$vips_libdir" ]]; then
    echo "  WARNING: pkg-config vips not found; skipping module bundling"
    return
fi

local mod_src
mod_src=$(find "$vips_libdir" -maxdepth 1 -type d -name 'vips-modules-*' 2>/dev/null | head -1)
if [[ -z "$mod_src" ]]; then
    echo "  No vips-modules-* directory found; skipping"
    return
fi

local mod_dest="$staging_dir/vips-modules"
mkdir -p "$mod_dest"
echo "  Bundling vips modules from $mod_src ..."

for mod in "$mod_src"/*.dylib "$mod_src"/*.so; do
    [[ -f "$mod" ]] || continue
    cp -L "$mod" "$mod_dest/$(basename "$mod")"
    echo "    Bundled: $(basename "$mod")"
done

patch_module_deps "$mod_dest" "$staging_dir" "vips-modules"
