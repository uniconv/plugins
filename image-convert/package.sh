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

# Preserve the versioned directory name (e.g. vips-modules-8.18) and
# place it under lib/ so that VIPSHOME-based discovery works:
#   VIPSHOME=<plugin_dir> → libvips searches <plugin_dir>/lib/vips-modules-X.Y/
local mod_basename
mod_basename=$(basename "$mod_src")
local mod_dest="$staging_dir/lib/$mod_basename"
mkdir -p "$mod_dest"
echo "  Bundling vips modules from $mod_src ..."

for mod in "$mod_src"/*.dylib "$mod_src"/*.so; do
    [[ -f "$mod" ]] || continue
    cp -L "$mod" "$mod_dest/$(basename "$mod")"
    echo "    Bundled: $(basename "$mod")"
done

# Bundle transitive deps of vips-modules (e.g. libheif for vips-heif)
# into the parent staging dir so patch_module_deps can resolve them.
for mod in "$mod_dest"/*.dylib "$mod_dest"/*.so; do
    [[ -f "$mod" ]] || continue
    collect_and_patch_deps "$mod" "$staging_dir"
done

patch_module_deps "$mod_dest" "$staging_dir" "lib/$mod_basename"
