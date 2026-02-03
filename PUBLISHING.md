# Publishing Plugins

This guide covers how to package plugins, upload them as GitHub releases, and submit them to the uniconv registry.

## Prerequisites

- [GitHub CLI](https://cli.github.com/) (`gh`) installed and authenticated
- Python 3.8+ (used by `package.sh`)
- For native plugins: the plugin must be built before packaging

## Overview

Each plugin directory contains two manifest files:

| File | Purpose |
|------|---------|
| `plugin.json` | Runtime manifest — tells uniconv how to load and run the plugin |
| `manifest.json` | Registry manifest — tells uniconv how to download and install the plugin |

The publishing workflow is:

1. Package the plugin into a tarball
2. Create a GitHub Release and upload the tarball
3. Update the SHA256 hash in `manifest.json`
4. Submit a PR to [uniconv/registry](https://github.com/uniconv/registry) with the `manifest.json`

## Step 1: Package

### Package all plugins

```bash
./package.sh
```

### Package a specific plugin

```bash
./package.sh image-grayscale
```

### Native plugins on a different platform

```bash
PLATFORM=darwin-aarch64 ./package.sh image-invert
```

Output goes to `dist/`. Each tarball gets a `.sha256` sidecar file.

**CLI plugins** produce platform-independent tarballs:

```
dist/image-grayscale-1.0.0.tar.gz
```

**Native plugins** produce platform-specific tarballs:

```
dist/image-invert-1.0.0-linux-x86_64.tar.gz
```

### Native plugin build (required before packaging)

Native plugins must be compiled before running `package.sh`:

```bash
cd image-invert
mkdir -p build && cd build
cmake ..
cmake --build .
```

## Step 2: Create a GitHub Release

Create a release with a tag matching `<plugin-name>-v<version>` and attach the tarball:

```bash
gh release create image-grayscale-v1.0.0 \
  --repo uniconv/plugins \
  --title "image-grayscale v1.0.0" \
  --notes "CLI plugin that converts images to grayscale using Python/Pillow." \
  dist/image-grayscale-1.0.0.tar.gz
```

For native plugins with multiple platform builds, attach all tarballs to the same release:

```bash
gh release create image-invert-v1.0.0 \
  --repo uniconv/plugins \
  --title "image-invert v1.0.0" \
  --notes "Native C++ plugin that inverts image colors using libvips." \
  dist/image-invert-1.0.0-linux-x86_64.tar.gz \
  dist/image-invert-1.0.0-linux-aarch64.tar.gz \
  dist/image-invert-1.0.0-darwin-aarch64.tar.gz
```

## Step 3: Verify the SHA256 hash

Download the release artifact and compute its hash:

```bash
gh release download image-grayscale-v1.0.0 \
  --repo uniconv/plugins \
  --pattern '*.tar.gz' \
  --dir /tmp

sha256sum /tmp/image-grayscale-1.0.0.tar.gz
```

Update the `sha256` field in the plugin's `manifest.json` to match this value. The hash must match the actual uploaded artifact, not the local tarball (timestamps in tar can cause differences across builds).

## Step 4: Submit to the registry

The [uniconv/registry](https://github.com/uniconv/registry) repo contains:

- `index.json` — plugin index with summary entries
- `plugins/<name>/manifest.json` — per-plugin registry manifest

### Create a branch and PR

```bash
# Clone or use existing checkout of uniconv/registry
cd /path/to/uniconv-registry

git checkout -b plugin/image-grayscale main

# Copy the manifest
mkdir -p plugins/image-grayscale
cp /path/to/uniconv-plugins/image-grayscale/manifest.json plugins/image-grayscale/manifest.json

# Commit and push
git add plugins/image-grayscale/manifest.json
git commit -m "Add image-grayscale plugin v1.0.0"
git push -u origin plugin/image-grayscale

# Create PR
gh pr create --repo uniconv/registry \
  --head plugin/image-grayscale \
  --base main \
  --title "Add image-grayscale plugin v1.0.0" \
  --body "Adds image-grayscale CLI plugin v1.0.0 to the registry."
```

### Update index.json

Submit a separate PR to add the plugin entry to `index.json`:

```json
{
  "name": "image-grayscale",
  "description": "Convert images to grayscale using Python/Pillow",
  "keywords": ["image", "grayscale", "filter", "python"],
  "latest": "1.0.0",
  "author": "uniconv",
  "interface": "cli"
}
```

## Registry manifest format

### CLI plugin (platform-independent)

```json
{
  "name": "image-grayscale",
  "description": "Convert images to grayscale using Python/Pillow",
  "author": "uniconv",
  "license": "MIT",
  "repository": "https://github.com/uniconv/plugins",
  "keywords": ["image", "grayscale", "filter", "conversion"],
  "releases": [
    {
      "version": "1.0.0",
      "uniconv_compat": ">=0.1.0",
      "interface": "cli",
      "dependencies": [
        { "name": "python3", "type": "system", "version": ">=3.8" },
        { "name": "Pillow", "type": "python", "version": ">=9.0" }
      ],
      "artifact": {
        "any": {
          "url": "https://github.com/uniconv/plugins/releases/download/image-grayscale-v1.0.0/image-grayscale-1.0.0.tar.gz",
          "sha256": "<sha256>"
        }
      }
    }
  ]
}
```

### Native plugin (per-platform)

```json
{
  "name": "image-invert",
  "description": "Invert image colors (native C++ plugin)",
  "author": "uniconv",
  "license": "MIT",
  "repository": "https://github.com/uniconv/plugins",
  "keywords": ["image", "invert", "negative", "filter"],
  "releases": [
    {
      "version": "1.0.0",
      "uniconv_compat": ">=0.1.0",
      "interface": "native",
      "dependencies": [
        { "name": "libvips", "type": "system", "check": "ldconfig -p | grep -q libvips" }
      ],
      "artifact": {
        "linux-x86_64": {
          "url": "https://github.com/uniconv/plugins/releases/download/image-invert-v1.0.0/image-invert-1.0.0-linux-x86_64.tar.gz",
          "sha256": "<sha256>"
        },
        "linux-aarch64": {
          "url": "...",
          "sha256": "<sha256>"
        },
        "darwin-aarch64": {
          "url": "...",
          "sha256": "<sha256>"
        }
      }
    }
  ]
}
```

### Artifact platform keys

| Key | Use for |
|-----|---------|
| `any` | CLI plugins (platform-independent scripts) |
| `linux-x86_64` | Native Linux x86_64 |
| `linux-aarch64` | Native Linux ARM64 |
| `darwin-aarch64` | Native macOS Apple Silicon |
| `darwin-x86_64` | Native macOS Intel |
| `windows-x86_64` | Native Windows x64 |

## Publishing updates

To publish a new version:

1. Update `version` in the plugin's `plugin.json`
2. Build (native plugins), package, and create a new release
3. Add a new entry to the `releases` array in `manifest.json` (newest first)
4. Submit a PR to the registry

```json
"releases": [
  {
    "version": "1.1.0",
    "...": "..."
  },
  {
    "version": "1.0.0",
    "...": "..."
  }
]
```
