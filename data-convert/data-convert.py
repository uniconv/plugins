#!/usr/bin/env python3
"""
uniconv CLI Plugin: data-convert

Converts between structured data formats.

Usage:
    data-convert.py --input <file> --target <format> [--output <file>] [options]

Supported formats:
    JSON, YAML, TOML, XML, CSV, TSV

Plugin options:
    --indent <n>            Indentation spaces (default: 2)
    --no-pretty             Compact output (no indentation)
    --csv-delimiter <c>     CSV field delimiter (default: ",")
    --xml-root <name>       Root element name for XML output (default: "root")
    --xml-item <name>       Element name for list items in XML (default: "item")
"""

import argparse
import csv
import io
import json
import os
import sys
import xml.etree.ElementTree as ET
from collections import OrderedDict
from pathlib import Path

try:
    import yaml
except ImportError:
    yaml = None

try:
    import toml
except ImportError:
    toml = None


# ---------------------------------------------------------------------------
# Parsers: input format -> Python native types
# ---------------------------------------------------------------------------

def parse_json(text):
    return json.loads(text)


def parse_yaml(text):
    if yaml is None:
        raise RuntimeError("pyyaml is not installed. Run: pip install pyyaml")
    return yaml.safe_load(text)


def parse_toml(text):
    if toml is None:
        raise RuntimeError("toml is not installed. Run: pip install toml")
    return toml.loads(text)


def _xml_element_to_dict(elem):
    """Convert an XML element to a Python dict following the conventions:
    - Attributes become @attr keys
    - Text content becomes #text
    - Child elements become nested dicts
    - Repeated same-name children become lists
    """
    result = OrderedDict()

    # Attributes
    for attr_name, attr_value in elem.attrib.items():
        result[f"@{attr_name}"] = attr_value

    # Children
    children_by_tag = OrderedDict()
    for child in elem:
        tag = child.tag
        child_dict = _xml_element_to_dict(child)
        if tag in children_by_tag:
            existing = children_by_tag[tag]
            if not isinstance(existing, list):
                children_by_tag[tag] = [existing]
            children_by_tag[tag].append(child_dict)
        else:
            children_by_tag[tag] = child_dict

    result.update(children_by_tag)

    # Text content
    text = (elem.text or "").strip()
    if text:
        if result:
            result["#text"] = text
        else:
            return text

    # Tail text is ignored (belongs to parent)

    return result if result else ""


def parse_xml(text):
    root = ET.fromstring(text)
    return {root.tag: _xml_element_to_dict(root)}


def _coerce_csv_value(value):
    """Attempt type coercion: int -> float -> string."""
    if value == "":
        return value
    try:
        return int(value)
    except (ValueError, TypeError):
        pass
    try:
        return float(value)
    except (ValueError, TypeError):
        pass
    return value


def parse_csv(text, delimiter=","):
    reader = csv.DictReader(io.StringIO(text), delimiter=delimiter)
    rows = []
    for row in reader:
        rows.append({k: _coerce_csv_value(v) for k, v in row.items()})
    return rows


# ---------------------------------------------------------------------------
# Serializers: Python native types -> output format
# ---------------------------------------------------------------------------

def serialize_json(data, indent=2, pretty=True):
    if pretty:
        return json.dumps(data, indent=indent, ensure_ascii=False)
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def serialize_yaml(data, indent=2, pretty=True):
    if yaml is None:
        raise RuntimeError("pyyaml is not installed. Run: pip install pyyaml")
    return yaml.dump(
        data,
        default_flow_style=not pretty,
        allow_unicode=True,
        indent=indent if pretty else None,
        sort_keys=False,
    )


def serialize_toml(data):
    if toml is None:
        raise RuntimeError("toml is not installed. Run: pip install toml")
    if not isinstance(data, dict):
        raise ValueError("TOML requires a top-level table (dict). The input data is a list or scalar.")
    _check_toml_nulls(data, path="")
    return toml.dumps(data)


def _check_toml_nulls(obj, path):
    """TOML does not support null values. Raise a clear error if any are found."""
    if obj is None:
        raise ValueError(f"TOML does not support null values (found at '{path}')")
    if isinstance(obj, dict):
        for k, v in obj.items():
            _check_toml_nulls(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _check_toml_nulls(v, f"{path}[{i}]")


def _dict_to_xml_element(tag, data, xml_item):
    """Convert a Python dict/list/scalar to an XML element."""
    elem = ET.Element(tag)

    if isinstance(data, dict):
        for key, value in data.items():
            if key.startswith("@"):
                elem.set(key[1:], str(value))
            elif key == "#text":
                elem.text = str(value)
            elif isinstance(value, list):
                for item in value:
                    child = _dict_to_xml_element(key, item, xml_item)
                    elem.append(child)
            elif isinstance(value, dict):
                child = _dict_to_xml_element(key, value, xml_item)
                elem.append(child)
            else:
                child = ET.SubElement(elem, key)
                child.text = str(value) if value is not None else ""
    elif isinstance(data, list):
        for item in data:
            child = _dict_to_xml_element(xml_item, item, xml_item)
            elem.append(child)
    else:
        elem.text = str(data) if data is not None else ""

    return elem


def _indent_xml(elem, level=0, indent_str="  "):
    """Add indentation to XML elements for pretty printing."""
    i = "\n" + level * indent_str
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = i + indent_str
        if not elem.tail or not elem.tail.strip():
            elem.tail = i
        for child in elem:
            _indent_xml(child, level + 1, indent_str)
        if not child.tail or not child.tail.strip():
            child.tail = i
    else:
        if level and (not elem.tail or not elem.tail.strip()):
            elem.tail = i


def serialize_xml(data, xml_root="root", xml_item="item", indent=2, pretty=True):
    if isinstance(data, dict) and len(data) == 1:
        root_tag = list(data.keys())[0]
        root_data = data[root_tag]
        root = _dict_to_xml_element(root_tag, root_data, xml_item)
    elif isinstance(data, dict):
        root = _dict_to_xml_element(xml_root, data, xml_item)
    elif isinstance(data, list):
        root = _dict_to_xml_element(xml_root, data, xml_item)
    else:
        root = ET.Element(xml_root)
        root.text = str(data)

    if pretty:
        _indent_xml(root, indent_str=" " * indent)

    tree = ET.ElementTree(root)
    buf = io.StringIO()
    tree.write(buf, encoding="unicode", xml_declaration=True)
    result = buf.getvalue()
    if pretty:
        result += "\n"
    return result


def serialize_csv(data, delimiter=","):
    if not isinstance(data, list):
        raise ValueError(
            "CSV output requires a list of objects. "
            "The input data is not a list."
        )
    if not data:
        return ""

    for i, row in enumerate(data):
        if not isinstance(row, dict):
            raise ValueError(
                f"CSV output requires a list of flat objects. "
                f"Item at index {i} is not an object."
            )
        for key, value in row.items():
            if isinstance(value, (dict, list)):
                raise ValueError(
                    f"CSV output requires flat objects. "
                    f"Nested structure found at key '{key}' in item {i}."
                )

    fieldnames = list(data[0].keys())
    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=fieldnames, delimiter=delimiter)
    writer.writeheader()
    for row in data:
        writer.writerow({k: v if v is not None else "" for k, v in row.items()})
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Format detection & dispatch
# ---------------------------------------------------------------------------

FORMAT_ALIASES = {
    "yml": "yaml",
    "tsv": "csv",
}

PARSERS = {
    "json": parse_json,
    "yaml": parse_yaml,
    "toml": parse_toml,
    "xml": parse_xml,
    "csv": parse_csv,
}

SERIALIZERS = {
    "json": serialize_json,
    "yaml": serialize_yaml,
    "toml": serialize_toml,
    "xml": serialize_xml,
    "csv": serialize_csv,
}


def detect_format(filepath):
    """Detect format from file extension."""
    ext = Path(filepath).suffix.lower().lstrip(".")
    return FORMAT_ALIASES.get(ext, ext)


def is_tsv_extension(filepath):
    """Check if the file has a .tsv extension."""
    return Path(filepath).suffix.lower() == ".tsv"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Convert between structured data formats")

    # Universal arguments (passed by uniconv core)
    parser.add_argument("--input", required=True, help="Input file path")
    parser.add_argument("--target", required=True, help="Target format")
    parser.add_argument("--output", help="Output file path")
    parser.add_argument("--input-format", help="Input format hint (for temp files)")
    parser.add_argument("--force", action="store_true", help="Overwrite existing")
    parser.add_argument("--dry-run", action="store_true", help="Dry run mode")

    # Plugin-specific options
    parser.add_argument("--indent", type=int, default=2, help="Indentation spaces (default: 2)")
    parser.add_argument("--no-pretty", action="store_true", help="Compact output (no indentation)")
    parser.add_argument("--csv-delimiter", default=",", help='CSV field delimiter (default: ",")')
    parser.add_argument("--xml-root", default="root", help='Root element name for XML output (default: "root")')
    parser.add_argument("--xml-item", default="item", help='Element name for list items in XML (default: "item")')

    args, _ = parser.parse_known_args()

    # Validate input
    if not os.path.exists(args.input):
        result = {
            "success": False,
            "error": f"Input file not found: {args.input}",
        }
        print(json.dumps(result))
        return 1

    # Determine input format
    if args.input_format:
        input_fmt = args.input_format.lower().lstrip(".")
        input_fmt = FORMAT_ALIASES.get(input_fmt, input_fmt)
    else:
        input_fmt = detect_format(args.input)

    if input_fmt not in PARSERS:
        result = {
            "success": False,
            "error": f"Unsupported input format: {input_fmt}",
        }
        print(json.dumps(result))
        return 1

    # Determine target format
    target_fmt = args.target.lower().lstrip(".")
    target_fmt = FORMAT_ALIASES.get(target_fmt, target_fmt)

    if target_fmt not in SERIALIZERS:
        result = {
            "success": False,
            "error": f"Unsupported target format: {target_fmt}",
        }
        print(json.dumps(result))
        return 1

    # Determine output path
    output_ext = "tsv" if target_fmt == "csv" and args.csv_delimiter == "\t" else target_fmt
    if args.output:
        output_path = args.output
        if not output_path.lower().endswith(f".{output_ext}"):
            output_path = f"{output_path}.{output_ext}"
    else:
        base = os.path.splitext(args.input)[0]
        output_path = f"{base}.{output_ext}"

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

    # Read input file
    try:
        with open(args.input, "r", encoding="utf-8") as f:
            input_text = f.read()
    except Exception as e:
        result = {
            "success": False,
            "error": f"Failed to read input file: {e}",
        }
        print(json.dumps(result))
        return 1

    # Handle empty input
    if not input_text.strip():
        data = {} if target_fmt in ("toml", "xml") else []
    else:
        # Parse input
        try:
            parse_fn = PARSERS[input_fmt]
            if input_fmt == "csv":
                delimiter = args.csv_delimiter
                if is_tsv_extension(args.input):
                    delimiter = "\t"
                data = parse_fn(input_text, delimiter=delimiter)
            else:
                data = parse_fn(input_text)
        except Exception as e:
            result = {
                "success": False,
                "error": f"Failed to parse {input_fmt}: {e}",
            }
            print(json.dumps(result))
            return 1

    # Serialize output
    try:
        pretty = not args.no_pretty
        if target_fmt == "json":
            output_text = serialize_json(data, indent=args.indent, pretty=pretty)
        elif target_fmt == "yaml":
            output_text = serialize_yaml(data, indent=args.indent, pretty=pretty)
        elif target_fmt == "toml":
            output_text = serialize_toml(data)
        elif target_fmt == "xml":
            output_text = serialize_xml(
                data,
                xml_root=args.xml_root,
                xml_item=args.xml_item,
                indent=args.indent,
                pretty=pretty,
            )
        elif target_fmt == "csv":
            output_text = serialize_csv(data, delimiter=args.csv_delimiter)
        else:
            raise ValueError(f"No serializer for format: {target_fmt}")
    except Exception as e:
        result = {
            "success": False,
            "error": f"Failed to serialize to {target_fmt}: {e}",
        }
        print(json.dumps(result))
        return 1

    # Write output file
    try:
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(output_text)
    except Exception as e:
        result = {
            "success": False,
            "error": f"Failed to write output file: {e}",
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
            "input_format": input_fmt,
            "output_format": target_fmt,
            "input_size": input_size,
            "compression_ratio": round(output_size / input_size, 2) if input_size > 0 else None,
        },
    }
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
