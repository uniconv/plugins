#!/usr/bin/env python3
"""
uniconv CLI Plugin: image-filter

Applies image filters (grayscale, invert, rembg) using Python/Pillow.

Usage:
    image_filter.py --input <file> --target <filter> [--output <file>] [options]

Supported targets:
    grayscale, gray, bw  - Convert to grayscale
    invert, negative     - Invert image colors
    rembg                - Remove image background

Plugin options:
    --method <method>  Grayscale conversion method: luminosity, average, lightness
                       (only applies to grayscale targets; default: luminosity)
    --model <model>    Background removal model (only applies to rembg; default: u2net)
    --quality <int>    Output quality 1-100 (default: 85)
    --width <int>      Output width in pixels
    --height <int>     Output height in pixels
"""

import argparse
import json
import sys
import os


GRAYSCALE_TARGETS = {'grayscale', 'gray', 'bw'}
INVERT_TARGETS = {'invert', 'negative'}
REMBG_TARGETS = {'rembg'}


def main():
    parser = argparse.ArgumentParser(description='Apply image filters')

    # Universal arguments (passed by uniconv core to ALL plugins)
    parser.add_argument('--input', required=True, help='Input file path')
    parser.add_argument('--target', required=True, help='Target filter')
    parser.add_argument('--output', help='Output file path')
    parser.add_argument('--force', action='store_true', help='Overwrite existing')
    parser.add_argument('--dry-run', action='store_true', help='Dry run mode')

    # Plugin-specific options
    parser.add_argument('--quality', type=int, default=85, help='Output quality (1-100)')
    parser.add_argument('--width', type=int, help='Output width')
    parser.add_argument('--height', type=int, help='Output height')
    parser.add_argument('--method', default='luminosity',
                       choices=['luminosity', 'average', 'lightness'],
                       help='Grayscale conversion method')
    parser.add_argument('--model', default='u2net',
                       help='Background removal model (rembg target)')

    args, _ = parser.parse_known_args()

    # Check if Pillow is available
    try:
        from PIL import Image, ImageOps
    except ImportError:
        result = {
            "success": False,
            "error": "Pillow not installed. Run: pip install Pillow"
        }
        print(json.dumps(result))
        return 1

    # Validate input
    if not os.path.exists(args.input):
        result = {
            "success": False,
            "error": f"Input file not found: {args.input}"
        }
        print(json.dumps(result))
        return 1

    # Validate target
    target_lower = args.target.lower()
    all_targets = GRAYSCALE_TARGETS | INVERT_TARGETS | REMBG_TARGETS
    if target_lower not in all_targets:
        result = {
            "success": False,
            "error": f"Unknown target: {args.target}. Supported: grayscale, gray, bw, invert, negative, rembg"
        }
        print(json.dumps(result))
        return 1

    # Get input file extension (preserve format)
    _, input_ext = os.path.splitext(args.input)
    if not input_ext:
        input_ext = '.jpg'  # Default fallback

    # rembg always outputs PNG (RGBA with transparency)
    out_ext = '.png' if target_lower in REMBG_TARGETS else input_ext

    # Use target name as suffix to avoid conflicts
    target_suffix = f"_{args.target}"

    # Determine output path
    if args.output:
        base, _ = os.path.splitext(args.output)
        output_path = f"{base}{target_suffix}{out_ext}"
    else:
        base, _ = os.path.splitext(args.input)
        output_path = f"{base}{target_suffix}{out_ext}"

    # Check if output exists
    if os.path.exists(output_path) and not args.force:
        result = {
            "success": False,
            "error": f"Output file exists (use --force to overwrite): {output_path}"
        }
        print(json.dumps(result))
        return 1

    # Dry run
    if args.dry_run:
        result = {
            "success": True,
            "output": output_path,
            "extra": {"dry_run": True, "filter": target_lower}
        }
        print(json.dumps(result))
        return 0

    try:
        # Open image
        img = Image.open(args.input)

        if target_lower in GRAYSCALE_TARGETS:
            processed = apply_grayscale(img, args.method)
            filter_name = "grayscale"
        elif target_lower in INVERT_TARGETS:
            processed = apply_invert(img)
            filter_name = "invert"
        elif target_lower in REMBG_TARGETS:
            processed = apply_rembg(img, args.model)
            filter_name = "rembg"

        # Resize if specified
        if args.width or args.height:
            w = args.width or int(img.width * (args.height / img.height))
            h = args.height or int(img.height * (args.width / img.width))
            processed = processed.resize((w, h), Image.Resampling.LANCZOS)

        # Convert back to RGB if saving as jpg (jpg doesn't support grayscale/palette well)
        output_ext = os.path.splitext(output_path)[1].lower()
        if output_ext in ['.jpg', '.jpeg'] and processed.mode != 'RGB':
            processed = processed.convert('RGB')

        # Save
        save_kwargs = {}
        if output_ext in ['.jpg', '.jpeg']:
            save_kwargs['quality'] = args.quality
        elif output_ext == '.webp':
            save_kwargs['quality'] = args.quality
        elif output_ext == '.png':
            save_kwargs['compress_level'] = 9 - (args.quality * 9 // 100)

        processed.save(output_path, **save_kwargs)

        # Get output size
        output_size = os.path.getsize(output_path)

        extra = {
            "filter": filter_name,
            "original_size": [img.width, img.height],
            "output_size_px": [processed.width, processed.height]
        }
        if filter_name == "grayscale":
            extra["method"] = args.method
        elif filter_name == "rembg":
            extra["model"] = args.model

        result = {
            "success": True,
            "output": output_path,
            "output_size": output_size,
            "extra": extra
        }
        print(json.dumps(result))
        return 0

    except Exception as e:
        result = {
            "success": False,
            "error": str(e)
        }
        print(json.dumps(result))
        return 1


def apply_grayscale(img, method):
    """Apply grayscale conversion using the specified method."""
    if method == 'luminosity':
        # Standard grayscale (ITU-R 601-2 luma transform)
        return img.convert('L')
    elif method == 'average':
        # Simple average of RGB channels
        import numpy as np
        if img.mode != 'RGB':
            img = img.convert('RGB')
        arr = np.array(img)
        avg = arr.mean(axis=2).astype(np.uint8)
        from PIL import Image
        return Image.fromarray(avg, mode='L')
    else:  # lightness
        img_rgb = img.convert('RGB')
        return img_rgb.convert('L')


def apply_rembg(img, model):
    """Remove image background using rembg."""
    try:
        from rembg import remove, new_session
    except ImportError:
        raise RuntimeError('rembg not installed. Run: pip install "rembg[cpu]"')

    session = new_session(model)
    return remove(img, session=session)


def apply_invert(img):
    """Invert image colors using Pillow's ImageOps."""
    from PIL import ImageOps

    # Ensure image is in a mode that supports inversion
    if img.mode == 'RGBA':
        # Split alpha, invert RGB, recombine
        r, g, b, a = img.split()
        from PIL import Image
        rgb = Image.merge('RGB', (r, g, b))
        inverted_rgb = ImageOps.invert(rgb)
        ir, ig, ib = inverted_rgb.split()
        return Image.merge('RGBA', (ir, ig, ib, a))
    elif img.mode in ('RGB', 'L'):
        return ImageOps.invert(img)
    else:
        # Convert to RGB, invert, return
        return ImageOps.invert(img.convert('RGB'))


if __name__ == '__main__':
    sys.exit(main())
