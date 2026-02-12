#!/bin/bash
set -euo pipefail

# Release a uniconv plugin.
#
# Bumps the version in plugin.json, adds a release entry to manifest.json,
# commits, and tags. Use --push to also push to origin.
#
# Usage:
#   ./release.sh ascii patch              # 1.0.0 → 1.0.1 (commit + tag only)
#   ./release.sh ascii minor              # 1.0.0 → 1.1.0
#   ./release.sh ascii major              # 1.0.0 → 2.0.0
#   ./release.sh ascii 2.0.0             # explicit version
#   ./release.sh all patch                # bump all plugins
#   ./release.sh ascii patch --push       # also push commit and tag to origin
#   ./release.sh ascii patch --dry-run    # show what would happen

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="uniconv/plugins"
DRY_RUN=false
PUSH=false
RELEASE_TAGS=()

# --- Helpers ---

die() { echo "ERROR: $*" >&2; exit 1; }

usage() {
    echo "Usage: $0 <plugin-name|all> <patch|minor|major|X.Y.Z> [--dry-run] [--push]"
    exit 1
}

json_field() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$1" "$2"
}

bump_version() {
    local current="$1" bump="$2"
    local major minor patch
    IFS='.' read -r major minor patch <<< "$current"
    case "$bump" in
        major) echo "$((major + 1)).0.0" ;;
        minor) echo "${major}.$((minor + 1)).0" ;;
        patch) echo "${major}.${minor}.$((patch + 1))" ;;
        *) die "Invalid bump type: $bump" ;;
    esac
}

validate_version() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Invalid version format: $1 (expected X.Y.Z)"
}

# --- Release a single plugin ---

release_plugin() {
    local NAME="$1"
    local BUMP_OR_VERSION="$2"

    local PLUGIN_DIR="$SCRIPT_DIR/$NAME"
    local PLUGIN_JSON="$PLUGIN_DIR/plugin.json"
    local MANIFEST_JSON="$PLUGIN_DIR/manifest.json"

    [[ -d "$PLUGIN_DIR" ]] || die "Plugin directory not found: $PLUGIN_DIR"
    [[ -f "$PLUGIN_JSON" ]] || die "plugin.json not found: $PLUGIN_JSON"
    [[ -f "$MANIFEST_JSON" ]] || die "manifest.json not found: $MANIFEST_JSON"

    local INTERFACE
    INTERFACE=$(json_field "$PLUGIN_JSON" "interface")
    local CURRENT_VERSION
    CURRENT_VERSION=$(json_field "$PLUGIN_JSON" "version")

    local NEW_VERSION
    if [[ "$BUMP_OR_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        NEW_VERSION="$BUMP_OR_VERSION"
    else
        NEW_VERSION=$(bump_version "$CURRENT_VERSION" "$BUMP_OR_VERSION")
    fi

    validate_version "$NEW_VERSION"
    [[ "$NEW_VERSION" != "$CURRENT_VERSION" ]] || die "New version is the same as current ($CURRENT_VERSION)"

    local TAG="${NAME}-v${NEW_VERSION}"

    echo "=== Plugin release: $NAME ==="
    echo "  Current version: $CURRENT_VERSION"
    echo "  New version:     $NEW_VERSION"
    echo "  Interface:       $INTERFACE"
    echo "  Tag:             $TAG"
    echo ""

    # --- Step 1: Update plugin.json ---

    echo "--- Step 1: Update plugin.json ---"
    if $DRY_RUN; then
        echo "  [dry-run] Would update version $CURRENT_VERSION → $NEW_VERSION"
    else
        python3 -c "
import json, sys
path = sys.argv[1]
version = sys.argv[2]
with open(path) as f:
    data = json.load(f)
data['version'] = version
with open(path, 'w') as f:
    json.dump(data, f, indent=2)
    f.write('\n')
" "$PLUGIN_JSON" "$NEW_VERSION"
        echo "  Updated version to $NEW_VERSION"
    fi
    echo ""

    # --- Step 2: Add release entry to manifest.json ---

    echo "--- Step 2: Update manifest.json ---"
    if $DRY_RUN; then
        echo "  [dry-run] Would add release entry for $NEW_VERSION"
    else
        python3 -c "
import json, sys

manifest_path = sys.argv[1]
plugin_json_path = sys.argv[2]
name = sys.argv[3]
version = sys.argv[4]
interface = sys.argv[5]
repo = sys.argv[6]

with open(manifest_path) as f:
    manifest = json.load(f)
with open(plugin_json_path) as f:
    plugin = json.load(f)

# Sync header fields from plugin.json to manifest.json
for field in ['name', 'description', 'author', 'license', 'repository', 'keywords']:
    if field in plugin:
        manifest[field] = plugin[field]

tag = f'{name}-v{version}'
base_url = f'https://github.com/{repo}/releases/download/{tag}'

has_bundled_libs = bool(plugin.get('bundled_libs'))
has_bundled_bins = bool(plugin.get('bundled_bins'))
platform_specific = (interface == 'native') or has_bundled_libs or has_bundled_bins

if platform_specific:
    artifact = {}
    for p in ['linux-x86_64', 'linux-aarch64', 'darwin-aarch64', 'windows-x86_64']:
        artifact[p] = {
            'url': f'{base_url}/{name}-{version}-{p}.tar.gz',
            'sha256': ''
        }
else:
    artifact = {
        'any': {
            'url': f'{base_url}/{name}-{version}.tar.gz',
            'sha256': ''
        }
    }

new_release = {
    'version': version,
    'uniconv_compat': '>=0.1.0',
    'interface': interface,
    'dependencies': plugin.get('dependencies', []),
    'artifact': artifact
}

manifest.setdefault('releases', [])
manifest['releases'].insert(0, new_release)

with open(manifest_path, 'w') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
" "$MANIFEST_JSON" "$PLUGIN_JSON" "$NAME" "$NEW_VERSION" "$INTERFACE" "$REPO"
        echo "  Added release entry for $NEW_VERSION"
    fi
    echo ""

    # --- Step 3: Commit ---

    echo "--- Step 3: Commit ---"
    if $DRY_RUN; then
        echo "  [dry-run] Would commit: chore($NAME): bump version to v$NEW_VERSION"
    else
        git -C "$SCRIPT_DIR" add "$NAME/plugin.json" "$NAME/manifest.json"
        git -C "$SCRIPT_DIR" commit -m "chore($NAME): bump version to v$NEW_VERSION"
        echo "  Committed."
    fi
    echo ""

    # --- Step 4: Tag ---

    echo "--- Step 4: Tag ---"
    if $DRY_RUN; then
        echo "  [dry-run] Would create annotated tag: $TAG"
    else
        git -C "$SCRIPT_DIR" tag -a "$TAG" -m "Release $NAME v$NEW_VERSION"
        echo "  Tagged: $TAG"
    fi
    echo ""

    RELEASE_TAGS+=("$TAG")

    echo "=== Done: $NAME $TAG released ==="
    echo ""
}

# --- Parse arguments ---

[[ $# -ge 2 ]] || usage

NAME="$1"
BUMP_OR_VERSION="$2"
shift 2
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=true ;;
        --push) PUSH=true ;;
        *) die "Unknown option: $1" ;;
    esac
    shift
done

# --- Run ---

if [[ "$NAME" == "all" ]]; then
    # 'all' only supports bump types, not explicit versions
    [[ "$BUMP_OR_VERSION" =~ ^(patch|minor|major)$ ]] || die "'all' only supports patch|minor|major, not explicit versions"

    # Confirmation for all plugins
    if ! $DRY_RUN; then
        echo "Will release ALL plugins with '$BUMP_OR_VERSION' bump."
        read -p "Proceed? [y/N] " -n 1 -r
        echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 0
    fi

    for plugin_json in "$SCRIPT_DIR"/*/plugin.json; do
        plugin_dir="$(dirname "$plugin_json")"
        plugin_name="$(basename "$plugin_dir")"
        release_plugin "$plugin_name" "$BUMP_OR_VERSION"
    done
else
    # Confirmation for single plugin
    if ! $DRY_RUN; then
        read -p "Proceed with release? [y/N] " -n 1 -r
        echo
        [[ $REPLY =~ ^[Yy]$ ]] || exit 0
    fi

    release_plugin "$NAME" "$BUMP_OR_VERSION"
fi

# --- Push ---

if [[ ${#RELEASE_TAGS[@]} -eq 0 ]]; then
    exit 0
fi

if $DRY_RUN; then
    echo "=== Push ==="
    echo "  [dry-run] Would push branch and ${#RELEASE_TAGS[@]} tag(s) to origin"
elif $PUSH; then
    echo "=== Pushing to origin ==="
    git -C "$SCRIPT_DIR" push origin HEAD
    echo "  Pushed branch."
    for tag in "${RELEASE_TAGS[@]}"; do
        git -C "$SCRIPT_DIR" push origin "$tag"
        echo "  Pushed tag: $tag"
    done
    echo ""
    echo "  CI will build and create GitHub Releases."
    echo "  Monitor: https://github.com/$REPO/actions"
else
    echo "=== Push ==="
    echo "  Skipped (use --push to push commits and tags to origin)"
fi
