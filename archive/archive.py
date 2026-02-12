#!/usr/bin/env python3
"""
uniconv CLI Plugin: archive

Archive format conversion (transcoding).

Usage:
    archive.py --input <file> --target <format> [--output <file>] [options]

Targets:
    zip                     Convert to zip
    tar                     Convert to tar
    tar-gz / tgz            Convert to tar.gz
    tar-bz2 / tbz2         Convert to tar.bz2
    tar-xz / txz           Convert to tar.xz
    gz                      Convert to gzip (single file archives only)

Supported input formats:
    zip, tar, tar.gz, tgz, tar.bz2, tbz2, tar.xz, txz, gz, bz2, xz, 7z

Plugin options:
    --compression-level <n>  Compression level 1-9 (default: 6)
"""

import argparse
import bz2
import gzip
import json
import lzma
import os
import shutil
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

try:
    import py7zr
except ImportError:
    py7zr = None


# ---------------------------------------------------------------------------
# Archive format detection
# ---------------------------------------------------------------------------

FORMAT_ALIASES = {
    "tgz": "tar-gz",
    "tbz2": "tar-bz2",
    "txz": "tar-xz",
}

# Map extensions to archive type
EXTENSION_MAP = {
    ".zip": "zip",
    ".tar": "tar",
    ".gz": "gz",
    ".bz2": "bz2",
    ".xz": "xz",
    ".7z": "7z",
    ".tgz": "tar-gz",
    ".tbz2": "tar-bz2",
    ".txz": "tar-xz",
}

# Map archive type to tar mode suffix
TAR_COMPRESSION_MODES = {
    "tar": "",
    "tar-gz": "gz",
    "tar-bz2": "bz2",
    "tar-xz": "xz",
}

# Map archive type to file extension
TARGET_EXTENSIONS = {
    "zip": ".zip",
    "tar": ".tar",
    "tar-gz": ".tar.gz",
    "tgz": ".tar.gz",
    "tar-bz2": ".tar.bz2",
    "tbz2": ".tar.bz2",
    "tar-xz": ".tar.xz",
    "txz": ".tar.xz",
    "gz": ".gz",
}


def detect_archive_format(filepath):
    """Detect archive format from file extension, handling compound extensions."""
    p = Path(filepath)
    name_lower = p.name.lower()

    # Check compound extensions first (e.g., .tar.gz, .tar.bz2, .tar.xz)
    if name_lower.endswith(".tar.gz"):
        return "tar-gz"
    if name_lower.endswith(".tar.bz2"):
        return "tar-bz2"
    if name_lower.endswith(".tar.xz"):
        return "tar-xz"

    # Single extension
    ext = p.suffix.lower()
    return EXTENSION_MAP.get(ext)


# ---------------------------------------------------------------------------
# Extraction helpers (used internally for transcoding)
# ---------------------------------------------------------------------------

def extract_zip(filepath, dest_dir):
    """Extract zip archive, returning list of extracted file paths."""
    with zipfile.ZipFile(filepath, "r") as zf:
        zf.extractall(dest_dir)
    return _collect_files(dest_dir)


def extract_tar(filepath, dest_dir):
    """Extract tar (optionally compressed) archive."""
    with tarfile.open(filepath, "r:*") as tf:
        tf.extractall(dest_dir, filter="data")
    return _collect_files(dest_dir)


def extract_gz(filepath, dest_dir):
    """Decompress a gzip file (single file, not tar.gz)."""
    out_name = Path(filepath).stem  # strip .gz
    out_path = os.path.join(dest_dir, out_name)
    with gzip.open(filepath, "rb") as f_in, open(out_path, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)
    return [out_path]


def extract_bz2(filepath, dest_dir):
    """Decompress a bz2 file."""
    out_name = Path(filepath).stem
    out_path = os.path.join(dest_dir, out_name)
    with bz2.open(filepath, "rb") as f_in, open(out_path, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)
    return [out_path]


def extract_xz(filepath, dest_dir):
    """Decompress an xz file."""
    out_name = Path(filepath).stem
    out_path = os.path.join(dest_dir, out_name)
    with lzma.open(filepath, "rb") as f_in, open(out_path, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)
    return [out_path]


def extract_7z(filepath, dest_dir):
    """Extract 7z archive."""
    if py7zr is None:
        raise RuntimeError("py7zr is not installed. Run: pip install py7zr")
    with py7zr.SevenZipFile(filepath, "r") as sz:
        sz.extractall(dest_dir)
    return _collect_files(dest_dir)


def _collect_files(directory):
    """Collect all files from a directory tree, flattening nested directories.

    Returns a list of absolute file paths (no directories).
    If there are name collisions from flattening, files are renamed with a suffix.
    """
    files = []
    for root, _dirs, filenames in os.walk(directory):
        for fname in filenames:
            full_path = os.path.join(root, fname)
            files.append(full_path)

    # Flatten: move all files to the top-level directory
    result = []
    seen_names = {}
    for fpath in sorted(files):
        fname = os.path.basename(fpath)
        dest = os.path.join(directory, fname)

        if fname in seen_names:
            # Handle name collision: add numeric suffix
            seen_names[fname] += 1
            stem, ext = os.path.splitext(fname)
            fname = f"{stem}_{seen_names[fname]}{ext}"
            dest = os.path.join(directory, fname)
        else:
            seen_names[fname] = 0

        # Move if not already at top level
        if fpath != dest:
            shutil.move(fpath, dest)

        result.append(dest)

    # Clean up empty subdirectories
    for root, dirs, _files in os.walk(directory, topdown=False):
        for d in dirs:
            dirpath = os.path.join(root, d)
            try:
                os.rmdir(dirpath)
            except OSError:
                pass

    return result


EXTRACTORS = {
    "zip": extract_zip,
    "tar": extract_tar,
    "tar-gz": extract_tar,
    "tar-bz2": extract_tar,
    "tar-xz": extract_tar,
    "gz": extract_gz,
    "bz2": extract_bz2,
    "xz": extract_xz,
    "7z": extract_7z,
}


def do_decompress(input_path, output_dir):
    """Decompress/extract an archive. Returns list of output file paths."""
    fmt = detect_archive_format(input_path)
    if fmt is None:
        raise ValueError(f"Cannot detect archive format from: {input_path}")

    extractor = EXTRACTORS.get(fmt)
    if extractor is None:
        raise ValueError(f"Unsupported archive format for extraction: {fmt}")

    # Extract to a temp directory inside output_dir
    extract_dir = tempfile.mkdtemp(dir=output_dir)
    files = extractor(input_path, extract_dir)

    # Move extracted files to output_dir directly
    result = []
    for fpath in files:
        fname = os.path.basename(fpath)
        dest = os.path.join(output_dir, fname)
        if fpath != dest:
            # Avoid overwriting
            if os.path.exists(dest):
                stem, ext = os.path.splitext(fname)
                counter = 1
                while os.path.exists(dest):
                    dest = os.path.join(output_dir, f"{stem}_{counter}{ext}")
                    counter += 1
            shutil.move(fpath, dest)
        result.append(dest)

    # Clean up temp extract dir
    shutil.rmtree(extract_dir, ignore_errors=True)

    return result


# ---------------------------------------------------------------------------
# Compression helpers (used internally for transcoding)
# ---------------------------------------------------------------------------

def _gather_input_files(input_path):
    """Gather files from input path (directory or single file).

    Returns a list of (absolute_path, arcname) tuples.
    """
    if os.path.isdir(input_path):
        files = []
        for root, _dirs, filenames in os.walk(input_path):
            for fname in sorted(filenames):
                fpath = os.path.join(root, fname)
                arcname = os.path.relpath(fpath, input_path)
                files.append((fpath, arcname))
        return files
    else:
        return [(input_path, os.path.basename(input_path))]


def compress_zip(input_path, output_path, compression_level=6):
    """Create a zip archive."""
    files = _gather_input_files(input_path)
    compression = zipfile.ZIP_DEFLATED
    with zipfile.ZipFile(output_path, "w", compression=compression,
                         compresslevel=compression_level) as zf:
        for fpath, arcname in files:
            zf.write(fpath, arcname)


def compress_tar(input_path, output_path, mode_suffix="", compression_level=6):
    """Create a tar archive (optionally compressed)."""
    mode = f"w:{mode_suffix}" if mode_suffix else "w"
    with tarfile.open(output_path, mode) as tf:
        files = _gather_input_files(input_path)
        for fpath, arcname in files:
            tf.add(fpath, arcname=arcname)


def compress_gz(input_path, output_path, compression_level=6):
    """Gzip compress a single file."""
    if os.path.isdir(input_path):
        raise ValueError(
            "gz target only supports single file input. "
            "Use tar-gz for compressing directories."
        )
    with open(input_path, "rb") as f_in, \
         gzip.open(output_path, "wb", compresslevel=compression_level) as f_out:
        shutil.copyfileobj(f_in, f_out)


def do_compress(input_path, target, output_path, compression_level=6):
    """Compress input into the specified archive format."""
    resolved_target = FORMAT_ALIASES.get(target, target)

    if resolved_target == "zip":
        compress_zip(input_path, output_path, compression_level)
    elif resolved_target in TAR_COMPRESSION_MODES:
        mode_suffix = TAR_COMPRESSION_MODES[resolved_target]
        compress_tar(input_path, output_path, mode_suffix, compression_level)
    elif resolved_target == "gz":
        compress_gz(input_path, output_path, compression_level)
    else:
        raise ValueError(f"Unsupported compression target: {target}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Archive format conversion")

    # Universal arguments (passed by uniconv core)
    parser.add_argument("--input", required=True, help="Input file path")
    parser.add_argument("--target", required=True, help="Target format")
    parser.add_argument("--output", help="Output file path")
    parser.add_argument("--input-format", help="Input format hint")
    parser.add_argument("--force", action="store_true", help="Overwrite existing")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode")

    # Plugin-specific options
    parser.add_argument("--compression-level", type=int, default=6,
                        help="Compression level 1-9 (default: 6)")

    args, _ = parser.parse_known_args()

    # Validate input
    if not os.path.exists(args.input):
        print(json.dumps({
            "success": False,
            "error": f"Input file not found: {args.input}",
        }))
        return 1

    target = args.target.lower()
    resolved_target = FORMAT_ALIASES.get(target, target)

    return handle_transcode(args, target, resolved_target)


def handle_transcode(args, target, resolved_target):
    """Handle archive-to-archive conversion."""
    # Determine output path
    ext = TARGET_EXTENSIONS.get(resolved_target)
    if ext is None:
        print(json.dumps({
            "success": False,
            "error": f"Unsupported compression target: {target}",
        }))
        return 1

    if args.output:
        output_path = args.output
    else:
        if os.path.isdir(args.input):
            base = args.input.rstrip("/")
        else:
            base = os.path.splitext(args.input)[0]
        output_path = base + ext

    # Dry run
    if args.dry_run:
        print(json.dumps({
            "success": True,
            "output": output_path,
            "extra": {"dry_run": True},
        }))
        return 0

    # Check if output exists
    if os.path.exists(output_path) and not args.force:
        print(json.dumps({
            "success": False,
            "error": f"Output file exists (use --force to overwrite): {output_path}",
        }))
        return 1

    # Require archive input for transcoding
    input_format = detect_archive_format(args.input)
    if input_format is None:
        print(json.dumps({
            "success": False,
            "error": f"Input is not a recognized archive format: {args.input}",
        }))
        return 1

    # Extract input archive, then recompress to target format
    temp_extract_dir = tempfile.mkdtemp(prefix="uniconv_transcode_")
    try:
        do_decompress(args.input, temp_extract_dir)
        compress_input = temp_extract_dir
    except Exception as e:
        shutil.rmtree(temp_extract_dir, ignore_errors=True)
        print(json.dumps({
            "success": False,
            "error": f"Failed to extract input archive for transcoding: {e}",
        }))
        return 1

    # Compress
    try:
        do_compress(
            compress_input,
            target,
            output_path,
            compression_level=args.compression_level,
        )
    except Exception as e:
        # Clean up partial output
        if os.path.exists(output_path):
            os.remove(output_path)
        shutil.rmtree(temp_extract_dir, ignore_errors=True)
        print(json.dumps({
            "success": False,
            "error": f"Failed to create archive: {e}",
        }))
        return 1

    shutil.rmtree(temp_extract_dir, ignore_errors=True)

    output_size = os.path.getsize(output_path)
    input_size = os.path.getsize(args.input)

    print(json.dumps({
        "success": True,
        "output": output_path,
        "output_size": output_size,
        "extra": {
            "archive_format": resolved_target,
            "input_size": input_size,
            "compression_ratio": round(output_size / input_size, 2) if input_size > 0 else None,
        },
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
