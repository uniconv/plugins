#!/usr/bin/env python3
"""
uniconv CLI Plugin: qr-code

Generate and decode QR codes.

Usage:
    qr-code.py --input <file> --target <qr|qr-decode> [--output <file>] [options]

Targets:
    qr          Generate QR code PNG from text
    qr-decode   Decode QR code from image to text

Plugin options (generation only):
    --scale <n>         Pixels per module (default: 10)
    --border <n>        Quiet zone in modules (default: 4)
    --error <L|M|Q|H>   Error correction level (default: M)
"""

import argparse
import ctypes
import ctypes.util
import json
import os
import sys

# Pre-load bundled libzbar (if present) before importing pyzbar.
# pyzbar uses ctypes.util.find_library('zbar') which won't find a bundled lib,
# so we load it explicitly and monkey-patch find_library.
_plugin_dir = os.path.dirname(os.path.abspath(__file__))
_lib_dir = os.path.join(_plugin_dir, 'lib')

if os.path.isdir(_lib_dir):
    if sys.platform == 'darwin':
        _zbar_name = 'libzbar.0.dylib'
    elif sys.platform == 'win32':
        _zbar_name = 'libzbar-0.dll'
    else:
        _zbar_name = 'libzbar.so.0'
    _zbar_path = os.path.join(_lib_dir, _zbar_name)
    if os.path.exists(_zbar_path):
        ctypes.CDLL(_zbar_path)
        _orig_find_library = ctypes.util.find_library
        def _patched_find_library(name):
            if name == 'zbar':
                return _zbar_path
            return _orig_find_library(name)
        ctypes.util.find_library = _patched_find_library

try:
    import segno
except ImportError:
    segno = None

try:
    from PIL import Image
    from pyzbar import pyzbar
except ImportError:
    Image = None
    pyzbar = None


# ---------------------------------------------------------------------------
# QR Code Generation
# ---------------------------------------------------------------------------

def generate_qr(text, output_path, scale=10, border=4, error="M"):
    """Generate a QR code PNG from text using segno."""
    if segno is None:
        raise RuntimeError("segno is not installed. Run: pip install segno")

    # Map error correction level to segno constants
    error_map = {
        "L": "l",
        "M": "m",
        "Q": "q",
        "H": "h",
    }
    error_level = error_map.get(error.upper())
    if error_level is None:
        raise ValueError(f"Invalid error correction level: {error}. Use L, M, Q, or H.")

    # Generate QR code
    qr = segno.make(text, error=error_level)

    # Save as PNG
    qr.save(output_path, kind="png", scale=scale, border=border)


# ---------------------------------------------------------------------------
# QR Code Decoding
# ---------------------------------------------------------------------------

def decode_qr(image_path):
    """Decode a QR code from an image file."""
    if Image is None or pyzbar is None:
        raise RuntimeError(
            "PIL and pyzbar are not installed. Run: pip install Pillow pyzbar\n"
            "Note: pyzbar requires system package 'zbar'. Install with:\n"
            "  macOS: brew install zbar\n"
            "  Linux: apt install libzbar0"
        )

    # Open image
    img = Image.open(image_path)

    # Decode QR codes
    decoded_objects = pyzbar.decode(img)

    if not decoded_objects:
        raise ValueError(f"No QR code found in image: {image_path}")

    # Return the first QR code's data
    return decoded_objects[0].data.decode("utf-8")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate and decode QR codes")

    # Universal arguments (passed by uniconv core)
    parser.add_argument("--input", required=True, help="Input file path")
    parser.add_argument("--target", required=True, help="Target format (qr or qr-decode)")
    parser.add_argument("--output", help="Output file path")
    parser.add_argument("--force", action="store_true", help="Overwrite existing")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode")

    # Plugin-specific options (for generation)
    parser.add_argument("--scale", type=int, default=10, help="Pixels per module (default: 10)")
    parser.add_argument("--border", type=int, default=4, help="Quiet zone in modules (default: 4)")
    parser.add_argument("--error", default="M", help="Error correction level: L/M/Q/H (default: M)")

    args, _ = parser.parse_known_args()

    # Validate input
    if not os.path.exists(args.input):
        result = {
            "success": False,
            "error": f"Input file not found: {args.input}",
        }
        print(json.dumps(result))
        return 1

    # Normalize target
    target = args.target.lower()

    if target not in ["qr", "qr-decode"]:
        result = {
            "success": False,
            "error": f"Invalid target: {target}. Use 'qr' or 'qr-decode'.",
        }
        print(json.dumps(result))
        return 1

    # Determine output path
    if target == "qr":
        if args.output:
            output_path = args.output
            if not output_path.lower().endswith(".png"):
                output_path = f"{output_path}.png"
        else:
            base = os.path.splitext(args.input)[0]
            output_path = f"{base}.png"
    else:  # qr-decode
        if args.output:
            output_path = args.output
            if not output_path.lower().endswith(".txt"):
                output_path = f"{output_path}.txt"
        else:
            base = os.path.splitext(args.input)[0]
            output_path = f"{base}_qr-decode.txt"

    # Dry run
    if args.dry_run:
        result = {
            "success": True,
            "output": output_path,
            "extra": {"dry_run": True},
        }
        print(json.dumps(result))
        return 0

    # Check if output exists
    if os.path.exists(output_path) and not args.force:
        result = {
            "success": False,
            "error": f"Output file exists (use --force to overwrite): {output_path}",
        }
        print(json.dumps(result))
        return 1

    # Execute conversion
    try:
        if target == "qr":
            # Read text input
            with open(args.input, "r", encoding="utf-8") as f:
                text = f.read().strip()

            if not text:
                raise ValueError("Input text is empty")

            # Generate QR code
            generate_qr(
                text,
                output_path,
                scale=args.scale,
                border=args.border,
                error=args.error,
            )

            extra = {
                "scale": args.scale,
                "border": args.border,
                "error": args.error.upper(),
            }
        else:  # qr-decode
            # Decode QR code
            decoded_text = decode_qr(args.input)

            # Write decoded text
            with open(output_path, "w", encoding="utf-8") as f:
                f.write(decoded_text)

            extra = {
                "decoded_length": len(decoded_text),
            }

    except Exception as e:
        result = {
            "success": False,
            "error": f"Failed to {target}: {e}",
        }
        print(json.dumps(result))
        return 1

    # Success
    output_size = os.path.getsize(output_path)
    input_size = os.path.getsize(args.input)

    result = {
        "success": True,
        "output": output_path,
        "output_size": output_size,
        "extra": {
            **extra,
            "input_size": input_size,
        },
    }
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
