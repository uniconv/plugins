#!/usr/bin/env python3
"""
uniconv CLI Plugin: generator

Generate random test data from scratch (generator mode — no input required).

Usage:
    generator.py --target <target> [--output <file>] [options]

Targets:
    random-noise      Random RGB noise image (PNG)
    random-names      Random full names (Faker)
    random-urls       Random URLs (Faker)
    random-lorem      Lorem ipsum paragraphs (Faker)
    random-uuid       Random UUIDs (stdlib)
    random-password   Random passwords
    random-csv        CSV with configurable fake data columns (Faker)

Plugin options:
    --width <n>         Image width for random-noise (default: 256)
    --height <n>        Image height for random-noise (default: 256)
    --count <n>         Number of items (default: 10)
    --length <n>        Password length for random-password (default: 16)
    --paragraphs <n>    Number of paragraphs for random-lorem (default: 3)
    --locale <locale>   Faker locale (default: en_US)
    --seed <n>          Random seed for reproducibility
    --columns <cols>    Column types for random-csv (default: name,email,phone,address)
    --rows <n>          Number of rows for random-csv (default: 100)
    --format <fmt>      Output format for text targets: json or txt (default: json)
"""

import argparse
import csv
import json
import os
import random
import string
import sys
import uuid

try:
    from PIL import Image
except ImportError:
    Image = None

try:
    from faker import Faker
except ImportError:
    Faker = None


# ---------------------------------------------------------------------------
# Generators
# ---------------------------------------------------------------------------

def generate_noise(width, height, output_path):
    """Generate a random RGB noise image."""
    if Image is None:
        raise RuntimeError("Pillow is not installed. Run: pip install Pillow")

    img = Image.new("RGB", (width, height))
    pixels = [
        (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
        for _ in range(width * height)
    ]
    img.putdata(pixels)
    img.save(output_path, format="PNG")


def generate_names(count, locale, output_path, fmt):
    """Generate random full names."""
    if Faker is None:
        raise RuntimeError("faker is not installed. Run: pip install faker")

    fake = Faker(locale)
    names = [fake.name() for _ in range(count)]
    _write_list(names, output_path, fmt)
    return names


def generate_urls(count, locale, output_path, fmt):
    """Generate random URLs."""
    if Faker is None:
        raise RuntimeError("faker is not installed. Run: pip install faker")

    fake = Faker(locale)
    urls = [fake.url() for _ in range(count)]
    _write_list(urls, output_path, fmt)
    return urls


def generate_lorem(paragraphs, locale, output_path, fmt):
    """Generate lorem ipsum paragraphs."""
    if Faker is None:
        raise RuntimeError("faker is not installed. Run: pip install faker")

    fake = Faker(locale)
    texts = [fake.paragraph(nb_sentences=5) for _ in range(paragraphs)]

    if fmt == "txt":
        with open(output_path, "w", encoding="utf-8") as f:
            f.write("\n\n".join(texts))
    else:
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(texts, f, indent=2, ensure_ascii=False)

    return texts


def generate_uuids(count, output_path, fmt):
    """Generate random UUIDs."""
    uuids = [str(uuid.uuid4()) for _ in range(count)]
    _write_list(uuids, output_path, fmt)
    return uuids


def generate_passwords(count, length, output_path, fmt):
    """Generate random passwords."""
    charset = string.ascii_letters + string.digits + string.punctuation
    passwords = [
        "".join(random.choices(charset, k=length))
        for _ in range(count)
    ]
    _write_list(passwords, output_path, fmt)
    return passwords


COLUMN_GENERATORS = {
    "name": lambda fake: fake.name(),
    "email": lambda fake: fake.email(),
    "phone": lambda fake: fake.phone_number(),
    "address": lambda fake: fake.address().replace("\n", ", "),
    "company": lambda fake: fake.company(),
    "date": lambda fake: fake.date(),
    "city": lambda fake: fake.city(),
    "country": lambda fake: fake.country(),
    "job": lambda fake: fake.job(),
    "text": lambda fake: fake.sentence(),
    "url": lambda fake: fake.url(),
    "uuid": lambda _fake: str(uuid.uuid4()),
    "number": lambda fake: fake.random_int(min=0, max=10000),
    "boolean": lambda fake: fake.boolean(),
}


def generate_csv_data(rows, columns, locale, output_path):
    """Generate CSV with configurable columns."""
    if Faker is None:
        raise RuntimeError("faker is not installed. Run: pip install faker")

    fake = Faker(locale)
    col_list = [c.strip() for c in columns.split(",")]

    # Validate columns
    for col in col_list:
        if col not in COLUMN_GENERATORS:
            raise ValueError(
                f"Unknown column type: {col}. "
                f"Available: {', '.join(sorted(COLUMN_GENERATORS.keys()))}"
            )

    with open(output_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=col_list)
        writer.writeheader()
        for _ in range(rows):
            row = {col: COLUMN_GENERATORS[col](fake) for col in col_list}
            writer.writerow(row)

    return col_list


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _write_list(items, output_path, fmt):
    """Write a list of items as JSON array or newline-separated text."""
    with open(output_path, "w", encoding="utf-8") as f:
        if fmt == "txt":
            f.write("\n".join(str(item) for item in items))
        else:
            json.dump(items, f, indent=2, ensure_ascii=False)


# ---------------------------------------------------------------------------
# Target configuration
# ---------------------------------------------------------------------------

TARGETS = {
    "random-noise": {"default_ext": "png"},
    "random-names": {"default_ext_json": "json", "default_ext_txt": "txt"},
    "random-urls": {"default_ext_json": "json", "default_ext_txt": "txt"},
    "random-lorem": {"default_ext_json": "json", "default_ext_txt": "txt"},
    "random-uuid": {"default_ext_json": "json", "default_ext_txt": "txt"},
    "random-password": {"default_ext_json": "json", "default_ext_txt": "txt"},
    "random-csv": {"default_ext": "csv"},
}


def get_default_ext(target, fmt):
    """Get the default file extension for a target."""
    config = TARGETS[target]
    if "default_ext" in config:
        return config["default_ext"]
    return config.get(f"default_ext_{fmt}", "json")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Generate random test data")

    # Universal arguments (passed by uniconv core)
    parser.add_argument("--input", help="Input file path (not used in generator mode)")
    parser.add_argument("--target", required=True, help="Target type")
    parser.add_argument("--output", help="Output file path")
    parser.add_argument("--force", action="store_true", help="Overwrite existing")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode")

    # Plugin-specific options
    parser.add_argument("--width", type=int, default=256, help="Image width (default: 256)")
    parser.add_argument("--height", type=int, default=256, help="Image height (default: 256)")
    parser.add_argument("--count", type=int, default=10, help="Number of items (default: 10)")
    parser.add_argument("--length", type=int, default=16, help="Password length (default: 16)")
    parser.add_argument("--paragraphs", type=int, default=3, help="Number of paragraphs (default: 3)")
    parser.add_argument("--locale", default="en_US", help="Faker locale (default: en_US)")
    parser.add_argument("--seed", type=int, default=None, help="Random seed for reproducibility")
    parser.add_argument("--columns", default="name,email,phone,address", help="Column types for CSV")
    parser.add_argument("--rows", type=int, default=100, help="Number of CSV rows (default: 100)")
    parser.add_argument("--format", default="json", choices=["json", "txt"], help="Output format: json or txt")

    args, _ = parser.parse_known_args()

    # Normalize target
    target = args.target.lower()

    if target not in TARGETS:
        result = {
            "success": False,
            "error": f"Unknown target: {target}. Available: {', '.join(sorted(TARGETS.keys()))}",
        }
        print(json.dumps(result))
        return 1

    # Set random seed if provided
    if args.seed is not None:
        random.seed(args.seed)
        if Faker is not None:
            Faker.seed(args.seed)

    # Determine output path
    fmt = args.format
    ext = get_default_ext(target, fmt)

    if args.output:
        output_path = args.output
        if not output_path.lower().endswith(f".{ext}"):
            output_path = f"{output_path}.{ext}"
    else:
        output_path = f"generated.{ext}"

    # Dry run
    if args.dry_run:
        result = {
            "success": True,
            "output": output_path,
            "extra": {"dry_run": True, "target": target},
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

    # Ensure output directory exists
    output_dir = os.path.dirname(output_path)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # Dispatch to generator
    extra = {"target": target}

    try:
        if target == "random-noise":
            generate_noise(args.width, args.height, output_path)
            extra["width"] = args.width
            extra["height"] = args.height

        elif target == "random-names":
            items = generate_names(args.count, args.locale, output_path, fmt)
            extra["count"] = len(items)
            extra["locale"] = args.locale

        elif target == "random-urls":
            items = generate_urls(args.count, args.locale, output_path, fmt)
            extra["count"] = len(items)
            extra["locale"] = args.locale

        elif target == "random-lorem":
            items = generate_lorem(args.paragraphs, args.locale, output_path, fmt)
            extra["paragraphs"] = len(items)
            extra["locale"] = args.locale

        elif target == "random-uuid":
            items = generate_uuids(args.count, output_path, fmt)
            extra["count"] = len(items)

        elif target == "random-password":
            items = generate_passwords(args.count, args.length, output_path, fmt)
            extra["count"] = len(items)
            extra["length"] = args.length

        elif target == "random-csv":
            col_list = generate_csv_data(args.rows, args.columns, args.locale, output_path)
            extra["rows"] = args.rows
            extra["columns"] = col_list
            extra["locale"] = args.locale

    except Exception as e:
        result = {
            "success": False,
            "error": f"Failed to generate {target}: {e}",
        }
        print(json.dumps(result))
        return 1

    # Success
    output_size = os.path.getsize(output_path)

    if args.seed is not None:
        extra["seed"] = args.seed
    extra["format"] = fmt

    result = {
        "success": True,
        "output": output_path,
        "output_size": output_size,
        "extra": extra,
    }
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
