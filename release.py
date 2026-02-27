#!/usr/bin/env python3
"""
Release uniconv plugins.

Bumps the version in plugin.json, adds a release entry to manifest.json,
commits, tags, and optionally pushes to trigger the CI release workflow.

Usage:
    python release.py ascii patch              # 1.0.0 -> 1.0.1
    python release.py ascii minor              # 1.0.0 -> 1.1.0
    python release.py ascii major              # 1.0.0 -> 2.0.0
    python release.py ascii 2.0.0              # explicit version
    python release.py all patch                # bump all plugins
    python release.py ascii patch --push       # also push to origin
    python release.py ascii patch --dry-run    # show what would happen
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO = "uniconv/plugins"


def die(msg: str):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(SCRIPT_DIR), *args],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        die(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def bump_version(current: str, bump: str) -> str:
    major, minor, patch = (int(x) for x in current.split("."))
    if bump == "major":
        return f"{major + 1}.0.0"
    elif bump == "minor":
        return f"{major}.{minor + 1}.0"
    elif bump == "patch":
        return f"{major}.{minor}.{patch + 1}"
    else:
        die(f"Invalid bump type: {bump}")


def is_version(s: str) -> bool:
    return bool(re.match(r"^\d+\.\d+\.\d+$", s))


def validate_version(version: str):
    if not re.match(r"^\d+\.\d+\.\d+$", version):
        die(f"Invalid version format: {version} (expected X.Y.Z)")


def read_json(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def write_json(path: Path, data: dict):
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")


def release_plugin(name: str, bump_or_version: str, *, dry_run: bool) -> str | None:
    """Release a single plugin. Returns the tag name, or None on dry-run."""
    plugin_dir = SCRIPT_DIR / name
    plugin_json_path = plugin_dir / "plugin.json"
    manifest_json_path = plugin_dir / "manifest.json"

    if not plugin_dir.is_dir():
        die(f"Plugin directory not found: {plugin_dir}")
    if not plugin_json_path.is_file():
        die(f"plugin.json not found: {plugin_json_path}")
    if not manifest_json_path.is_file():
        die(f"manifest.json not found: {manifest_json_path}")

    plugin = read_json(plugin_json_path)
    current_version = plugin["version"]
    interface = plugin["interface"]

    if is_version(bump_or_version):
        new_version = bump_or_version
    else:
        new_version = bump_version(current_version, bump_or_version)

    validate_version(new_version)
    if new_version == current_version:
        die(f"New version is the same as current ({current_version})")

    tag = f"{name}-v{new_version}"

    print(f"=== Plugin release: {name} ===")
    print(f"  Current version: {current_version}")
    print(f"  New version:     {new_version}")
    print(f"  Interface:       {interface}")
    print(f"  Tag:             {tag}")
    print()

    # --- Step 1: Update plugin.json ---
    print("--- Step 1: Update plugin.json ---")
    if dry_run:
        print(f"  [dry-run] Would update version {current_version} -> {new_version}")
    else:
        plugin["version"] = new_version
        write_json(plugin_json_path, plugin)
        print(f"  Updated version to {new_version}")
    print()

    # --- Step 2: Update manifest.json ---
    print("--- Step 2: Update manifest.json ---")
    if dry_run:
        print(f"  [dry-run] Would add release entry for {new_version}")
    else:
        manifest = read_json(manifest_json_path)

        # Sync header fields from plugin.json
        for field in ["name", "description", "author", "license", "repository", "keywords"]:
            if field in plugin:
                manifest[field] = plugin[field]

        base_url = f"https://github.com/{REPO}/releases/download/{tag}"

        has_bundled_libs = bool(plugin.get("bundled_libs"))
        has_bundled_bins = bool(plugin.get("bundled_bins"))
        platform_specific = (interface == "native") or has_bundled_libs or has_bundled_bins

        if platform_specific:
            artifact = {}
            for p in ["linux-x86_64", "linux-aarch64", "darwin-aarch64", "windows-x86_64"]:
                artifact[p] = {
                    "url": f"{base_url}/{name}-{new_version}-{p}.tar.gz",
                    "sha256": ""
                }
        else:
            artifact = {
                "any": {
                    "url": f"{base_url}/{name}-{new_version}.tar.gz",
                    "sha256": ""
                }
            }

        new_release = {
            "version": new_version,
            "uniconv_compat": ">=0.1.0",
            "interface": interface,
            "dependencies": plugin.get("dependencies", []),
            "artifact": artifact
        }

        manifest.setdefault("releases", [])
        manifest["releases"].insert(0, new_release)

        write_json(manifest_json_path, manifest)
        print(f"  Added release entry for {new_version}")
    print()

    # --- Step 3: Commit ---
    print("--- Step 3: Commit ---")
    if dry_run:
        print(f"  [dry-run] Would commit: chore({name}): bump version to v{new_version}")
    else:
        git("add", f"{name}/plugin.json", f"{name}/manifest.json")
        git("commit", "-m", f"chore({name}): bump version to v{new_version}")
        print("  Committed.")
    print()

    # --- Step 4: Tag ---
    print("--- Step 4: Tag ---")
    if dry_run:
        print(f"  [dry-run] Would create annotated tag: {tag}")
    else:
        git("tag", "-a", tag, "-m", f"Release {name} v{new_version}")
        print(f"  Tagged: {tag}")
    print()

    print(f"=== Done: {name} {tag} released ===")
    print()

    return tag if not dry_run else None


def main():
    parser = argparse.ArgumentParser(description="Release uniconv plugins")
    parser.add_argument("name", help="Plugin name or 'all'")
    parser.add_argument("bump", help="patch, minor, major, or explicit X.Y.Z")
    parser.add_argument("--dry-run", action="store_true", help="Show what would happen")
    parser.add_argument("--push", action="store_true", help="Push commits and tags to origin")
    args = parser.parse_args()

    if args.name == "all":
        if not is_version(args.bump) and args.bump not in ("patch", "minor", "major"):
            die("'all' only supports patch|minor|major, not explicit versions")

        plugin_dirs = sorted(
            p.parent.name for p in SCRIPT_DIR.glob("*/plugin.json")
        )

        if not args.dry_run:
            print(f"Will release {len(plugin_dirs)} plugins with '{args.bump}' bump:")
            for name in plugin_dirs:
                print(f"  - {name}")
            answer = input("Proceed? [y/N] ").strip().lower()
            if answer != "y":
                print("Aborted.")
                return

        tags = []
        for name in plugin_dirs:
            tag = release_plugin(name, args.bump, dry_run=args.dry_run)
            if tag:
                tags.append(tag)
    else:
        if not args.dry_run:
            answer = input("Proceed with release? [y/N] ").strip().lower()
            if answer != "y":
                print("Aborted.")
                return

        tag = release_plugin(args.name, args.bump, dry_run=args.dry_run)
        tags = [tag] if tag else []

    # --- Push ---
    if not tags:
        if args.dry_run:
            print("=== Push ===")
            print(f"  [dry-run] Would push branch and tag(s) to origin")
        return

    if args.push:
        print("=== Pushing to origin ===")
        git("pull", "--rebase", "origin", "HEAD")
        git("push", "origin", "HEAD")
        print("  Pushed branch.")
        for tag in tags:
            git("push", "origin", tag)
            print(f"  Pushed tag: {tag}")
        print()
        print(f"  CI will build and create GitHub Releases.")
        print(f"  Monitor: https://github.com/{REPO}/actions")
    else:
        print("=== Push ===")
        print("  Skipped (use --push to push commits and tags to origin)")


if __name__ == "__main__":
    main()
