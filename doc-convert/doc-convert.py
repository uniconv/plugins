#!/usr/bin/env python3
"""
uniconv CLI Plugin: doc-convert

Converts between document formats using LibreOffice and Pandoc.

Usage:
    doc-convert.py --input <file> --target <format> [--output <file>] [options]

Supported formats:
    Office:  docx, doc, odt, xlsx, xls, ods, pptx, ppt, odp
    Korean:  hwp, hwpx (input only)
    Export:  pdf, txt, html, csv, rtf
    Other:   epub, md (markdown)

Plugin options:
    --pages <range>     Page range for PDF output (e.g., '1-5')
    --password <pwd>    Password for protected documents
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Format categories
WORD_FORMATS = {'docx', 'doc', 'odt', 'rtf', 'txt', 'html', 'htm', 'pdf', 'epub', 'hwp', 'hwpx'}
SPREADSHEET_FORMATS = {'xlsx', 'xls', 'ods', 'csv'}
PRESENTATION_FORMATS = {'pptx', 'ppt', 'odp', 'pdf'}
MARKDOWN_FORMATS = {'md', 'markdown'}

# LibreOffice filter mappings for specific conversions
LO_FILTERS = {
    'pdf': {
        'writer': 'writer_pdf_Export',
        'calc': 'calc_pdf_Export',
        'impress': 'impress_pdf_Export',
    },
    'docx': 'MS Word 2007 XML',
    'doc': 'MS Word 97',
    'odt': 'writer8',
    'rtf': 'Rich Text Format',
    'txt': 'Text',
    'html': 'HTML',
    'xlsx': 'Calc MS Excel 2007 XML',
    'xls': 'MS Excel 97',
    'ods': 'calc8',
    'csv': 'Text - txt - csv (StarCalc)',
    'pptx': 'Impress MS PowerPoint 2007 XML',
    'ppt': 'MS PowerPoint 97',
    'odp': 'impress8',
    'epub': 'EPUB',
}


def find_libreoffice():
    """Find the LibreOffice executable."""
    candidates = [
        'libreoffice',
        'soffice',
        '/Applications/LibreOffice.app/Contents/MacOS/soffice',
        '/usr/bin/libreoffice',
        '/usr/local/bin/libreoffice',
        'C:\\Program Files\\LibreOffice\\program\\soffice.exe',
        'C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe',
    ]

    for candidate in candidates:
        if shutil.which(candidate):
            return candidate
        if os.path.isfile(candidate):
            return candidate

    return None


def find_pandoc():
    """Find the Pandoc executable."""
    return shutil.which('pandoc')


def find_hwp5_tool(tool_name):
    """Find a hwp5 CLI tool (e.g., hwp5odt, hwp5txt, hwp5html)."""
    return shutil.which(tool_name)


# Direct hwp5 tool mapping: target format -> (tool name, output extension)
HWP5_DIRECT_TARGETS = {
    'odt': 'hwp5odt',
    'txt': 'hwp5txt',
    'html': 'hwp5html',
}


def convert_hwp_with_pyhwp(input_path, output_path, target_format):
    """
    Convert HWP files using pyhwp (hwp5 tools).

    For odt/txt/html targets, uses the corresponding hwp5 tool directly.
    For other targets (pdf, docx, etc.), converts HWP -> ODT first,
    then passes the ODT to LibreOffice.
    """
    target_format = target_format.lower().lstrip('.')

    # Direct conversion for supported targets
    if target_format in HWP5_DIRECT_TARGETS:
        tool_name = HWP5_DIRECT_TARGETS[target_format]
        tool_path = find_hwp5_tool(tool_name)
        if not tool_path:
            return None  # signal that pyhwp is not available

        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                temp_output = os.path.join(temp_dir, f"output.{target_format}")
                cmd = [tool_path, '--output', temp_output, input_path]
                result = subprocess.run(
                    cmd, capture_output=True, text=True, timeout=300
                )
                if result.returncode != 0:
                    error_msg = result.stderr or result.stdout or "Unknown error"
                    return False, f"hwp5 conversion failed: {error_msg}"
                if not os.path.exists(temp_output):
                    return False, f"{tool_name} produced no output file"
                shutil.move(temp_output, output_path)
                return True, None
        except subprocess.TimeoutExpired:
            return False, f"{tool_name} conversion timed out"
        except Exception as e:
            return False, str(e)

    # Two-step conversion: HWP -> ODT -> target via LibreOffice
    odt_tool = find_hwp5_tool('hwp5odt')
    if not odt_tool:
        return None  # signal that pyhwp is not available

    try:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_odt = os.path.join(temp_dir, "intermediate.odt")
            cmd = [odt_tool, '--output', temp_odt, input_path]
            result = subprocess.run(
                cmd, capture_output=True, text=True, timeout=300
            )
            if result.returncode != 0:
                error_msg = result.stderr or result.stdout or "Unknown error"
                return False, f"hwp5odt conversion failed: {error_msg}"
            if not os.path.exists(temp_odt):
                return False, "hwp5odt produced no output file"

            # Now convert ODT -> target using LibreOffice
            return convert_with_libreoffice(temp_odt, output_path, target_format)
    except subprocess.TimeoutExpired:
        return False, "hwp5odt conversion timed out"
    except Exception as e:
        return False, str(e)


def get_format_category(fmt):
    """Determine the format category."""
    fmt = fmt.lower().lstrip('.')
    if fmt in SPREADSHEET_FORMATS:
        return 'spreadsheet'
    if fmt in PRESENTATION_FORMATS and fmt != 'pdf':
        return 'presentation'
    if fmt in MARKDOWN_FORMATS:
        return 'markdown'
    return 'document'


def convert_with_libreoffice(input_path, output_path, target_format, password=None):
    """Convert using LibreOffice headless mode."""
    lo_path = find_libreoffice()
    if not lo_path:
        return False, "LibreOffice not found. Please install LibreOffice."

    target_format = target_format.lower().lstrip('.')

    # Create a temporary directory for output
    with tempfile.TemporaryDirectory() as temp_dir:
        # Build the command
        cmd = [
            lo_path,
            '--headless',
            '--invisible',
            '--nologo',
            '--nofirststartwizard',
        ]

        # Add password if provided
        if password:
            cmd.extend(['--infilter=:' + password])

        # Determine the output filter
        input_ext = Path(input_path).suffix.lower().lstrip('.')
        input_category = get_format_category(input_ext)

        filter_name = None
        if target_format == 'pdf':
            if input_category == 'spreadsheet':
                filter_name = LO_FILTERS['pdf']['calc']
            elif input_category == 'presentation':
                filter_name = LO_FILTERS['pdf']['impress']
            else:
                filter_name = LO_FILTERS['pdf']['writer']
        elif target_format in LO_FILTERS:
            filter_name = LO_FILTERS[target_format]
            if isinstance(filter_name, dict):
                filter_name = filter_name.get('writer', filter_name.get('calc'))

        # Build convert command
        convert_arg = target_format
        if filter_name:
            convert_arg = f'{target_format}:{filter_name}'

        cmd.extend([
            '--convert-to', convert_arg,
            '--outdir', temp_dir,
            input_path
        ])

        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )

            if result.returncode != 0:
                error_msg = result.stderr or result.stdout or "Unknown error"
                return False, f"LibreOffice conversion failed: {error_msg}"

            # Find the output file
            input_stem = Path(input_path).stem
            expected_output = Path(temp_dir) / f"{input_stem}.{target_format}"

            # LibreOffice might produce slightly different names
            if not expected_output.exists():
                outputs = list(Path(temp_dir).glob(f"*.{target_format}"))
                if outputs:
                    expected_output = outputs[0]
                else:
                    return False, f"Output file not created by LibreOffice"

            # Move to final destination
            shutil.move(str(expected_output), output_path)
            return True, None

        except subprocess.TimeoutExpired:
            return False, "LibreOffice conversion timed out"
        except Exception as e:
            return False, str(e)


def convert_pdf_to_docx(input_path, output_path):
    """Convert PDF to DOCX using pdf2docx library."""
    try:
        from pdf2docx import Converter
    except ImportError:
        return False, "pdf2docx not installed. Run: pip install pdf2docx"

    try:
        cv = Converter(input_path)
        cv.convert(output_path)
        cv.close()
        return True, None
    except Exception as e:
        return False, f"PDF to DOCX conversion failed: {str(e)}"


def convert_with_pandoc(input_path, output_path, input_format, target_format):
    """Convert using Pandoc (for Markdown conversions)."""
    pandoc_path = find_pandoc()
    if not pandoc_path:
        return False, "Pandoc not found. Please install Pandoc for Markdown conversion."

    # Map formats to Pandoc names
    format_map = {
        'md': 'markdown',
        'markdown': 'markdown',
        'html': 'html',
        'htm': 'html',
        'docx': 'docx',
        'odt': 'odt',
        'rtf': 'rtf',
        'txt': 'plain',
        'epub': 'epub',
        'pdf': 'pdf',
    }

    pandoc_input = format_map.get(input_format, input_format)
    pandoc_output = format_map.get(target_format, target_format)

    cmd = [
        pandoc_path,
        '-f', pandoc_input,
        '-t', pandoc_output,
        '-o', output_path,
        input_path
    ]

    # PDF requires a PDF engine
    if target_format == 'pdf':
        cmd.extend(['--pdf-engine=xelatex'])

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120
        )

        if result.returncode != 0:
            error_msg = result.stderr or result.stdout or "Unknown error"
            return False, f"Pandoc conversion failed: {error_msg}"

        return True, None

    except subprocess.TimeoutExpired:
        return False, "Pandoc conversion timed out"
    except Exception as e:
        return False, str(e)


def convert_document(input_path, output_path, target_format, password=None, input_format=None):
    """
    Convert a document to the target format.

    Chooses the appropriate conversion method based on formats.
    """
    # Use provided input_format hint, or detect from file extension
    if input_format:
        input_ext = input_format.lower().lstrip('.')
    else:
        input_ext = Path(input_path).suffix.lower().lstrip('.')
    target_format = target_format.lower().lstrip('.')

    # HWP files: try pyhwp first (LibreOffice on macOS often lacks HWP filter)
    if input_ext == 'hwp':
        hwp_result = convert_hwp_with_pyhwp(input_path, output_path, target_format)
        if hwp_result is not None:
            return hwp_result
        # pyhwp not available — fall through to LibreOffice

    # PDF to DOCX uses pdf2docx library (LibreOffice can't do this)
    if input_ext == 'pdf' and target_format in ('docx', 'doc'):
        return convert_pdf_to_docx(input_path, output_path)

    # Markdown conversions prefer Pandoc
    if input_ext in MARKDOWN_FORMATS or target_format in MARKDOWN_FORMATS:
        pandoc_path = find_pandoc()
        if pandoc_path:
            return convert_with_pandoc(input_path, output_path, input_ext, target_format)
        elif target_format in MARKDOWN_FORMATS:
            return False, "Pandoc is required for Markdown conversion"
        # Fall through to LibreOffice for non-markdown targets

    # Use LibreOffice for all other conversions
    return convert_with_libreoffice(input_path, output_path, target_format, password)


def main():
    parser = argparse.ArgumentParser(description='Convert between document formats')

    # Universal arguments (passed by uniconv core)
    parser.add_argument('--input', required=True, help='Input file path')
    parser.add_argument('--target', required=True, help='Target format')
    parser.add_argument('--output', help='Output file path')
    parser.add_argument('--input-format', help='Input format hint (for temp files)')
    parser.add_argument('--force', action='store_true', help='Overwrite existing')
    parser.add_argument('--dry-run', action='store_true', help='Dry run mode')

    # Plugin-specific options
    parser.add_argument('--pages', default='',
                       help='Page range for PDF output (e.g., "1-5")')
    parser.add_argument('--password', default='',
                       help='Password for protected documents')

    args, _ = parser.parse_known_args()

    # Validate input
    if not os.path.exists(args.input):
        result = {
            "success": False,
            "error": f"Input file not found: {args.input}"
        }
        print(json.dumps(result))
        return 1

    # Determine output path
    target_format = args.target.lower().lstrip('.')
    if args.output:
        output_path = args.output
        # Ensure correct extension
        if not output_path.lower().endswith(f'.{target_format}'):
            output_path = f"{output_path}.{target_format}"
    else:
        base = os.path.splitext(args.input)[0]
        output_path = f"{base}.{target_format}"

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
            "extra": {"dry_run": True}
        }
        print(json.dumps(result))
        return 0

    # Perform conversion
    input_format = getattr(args, 'input_format', None)
    success, error = convert_document(
        args.input,
        output_path,
        target_format,
        password=args.password if args.password else None,
        input_format=input_format
    )

    if success:
        output_size = os.path.getsize(output_path)
        input_size = os.path.getsize(args.input)
        input_ext = input_format if input_format else Path(args.input).suffix.lower().lstrip('.')

        result = {
            "success": True,
            "output": output_path,
            "output_size": output_size,
            "extra": {
                "input_format": input_ext,
                "output_format": target_format,
                "input_size": input_size,
                "compression_ratio": round(output_size / input_size, 2) if input_size > 0 else None
            }
        }
        print(json.dumps(result))
        return 0
    else:
        result = {
            "success": False,
            "error": error
        }
        print(json.dumps(result))
        return 1


if __name__ == '__main__':
    sys.exit(main())
