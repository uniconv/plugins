#!/usr/bin/env python3
"""
uniconv CLI Plugin: tts

Text-to-speech using cloud AI providers (OpenAI, ElevenLabs, Typecast, Gemini).

Usage:
    tts.py --input <file> --target <provider> [--output <file>] [options]

Supported targets:
    openai      - OpenAI TTS (mp3, opus, aac, flac, wav)
    elevenlabs  - ElevenLabs TTS (mp3)
    typecast    - Typecast TTS (mp3, wav)
    gemini      - Google Gemini TTS (wav)

Common options:
    --voice <name>     Voice name or ID
    --model <name>     TTS model
    --api-key <key>    API key (overrides env var)

Provider-specific options:
    OpenAI:      --speed, --instructions
    ElevenLabs:  --stability, --similarity-boost, --style
    Typecast:    --language, --emotion, --emotion-intensity,
                 --volume, --pitch, --tempo, --seed
"""

import argparse
import json
import os
import sys


def main():
    parser = argparse.ArgumentParser(description='Text-to-speech via cloud AI providers')

    # Universal arguments (passed by uniconv core to ALL plugins)
    parser.add_argument('--input', required=True, help='Input text file path')
    parser.add_argument('--target', required=True, help='TTS provider name')
    parser.add_argument('--output', help='Output file path')
    parser.add_argument('--force', action='store_true', help='Overwrite existing')
    parser.add_argument('--dry-run', action='store_true', help='Dry run mode')

    # Common options (all providers)
    parser.add_argument('--voice', help='Voice name or ID')
    parser.add_argument('--model', help='TTS model')
    parser.add_argument('--api-key', help='API key (overrides env var)')

    # OpenAI-specific
    parser.add_argument('--speed', type=float, default=1.0, help='Speech speed (0.25-4.0)')
    parser.add_argument('--instructions', help='Additional instructions for OpenAI TTS')

    # ElevenLabs-specific
    parser.add_argument('--stability', type=float, default=0.5, help='Voice stability (0-1)')
    parser.add_argument('--similarity-boost', type=float, default=0.75, help='Voice similarity (0-1)')
    parser.add_argument('--style', type=float, default=0.0, help='Style exaggeration (0-1)')

    # Typecast-specific
    parser.add_argument('--language', help='Language code (e.g. en-us, ko-kr, auto)')
    parser.add_argument('--emotion', help='Emotion tone preset')
    parser.add_argument('--volume', type=int, default=100, help='Volume percentage (50-200)')
    parser.add_argument('--pitch', type=int, default=0, help='Pitch shift in semitones (-12 to 12)')
    parser.add_argument('--tempo', type=float, default=1.0, help='Speech tempo (0.5-2.0)')
    parser.add_argument('--seed', type=int, help='Random seed')

    args, _ = parser.parse_known_args()

    # Check if requests is available
    try:
        import requests  # noqa: F401
    except ImportError:
        print(json.dumps({
            "success": False,
            "error": "requests not installed. Run: pip install requests",
        }))
        return 1

    # Import provider registry (after requests check)
    from providers import get_provider

    # Validate input file
    if not os.path.exists(args.input):
        print(json.dumps({
            "success": False,
            "error": f"Input file not found: {args.input}",
        }))
        return 1

    # Resolve provider
    try:
        provider = get_provider(args.target)
    except ValueError as e:
        print(json.dumps({"success": False, "error": str(e)}))
        return 1

    # Resolve output format
    try:
        fmt = provider.resolve_format(args)
    except ValueError as e:
        print(json.dumps({"success": False, "error": str(e)}))
        return 1

    # Determine output path
    if args.output:
        output_path = args.output
        # Ensure correct extension
        base, ext = os.path.splitext(output_path)
        if not ext:
            output_path = f"{base}.{fmt}"
    else:
        base, _ = os.path.splitext(args.input)
        output_path = f"{base}_{args.target}.{fmt}"

    # Check if output exists
    if os.path.exists(output_path) and not args.force:
        print(json.dumps({
            "success": False,
            "error": f"Output file exists (use --force to overwrite): {output_path}",
        }))
        return 1

    # Dry run
    if args.dry_run:
        print(json.dumps({
            "success": True,
            "output": output_path,
            "extra": {
                "dry_run": True,
                "provider": provider.name,
                "voice": getattr(args, 'voice', None) or provider.default_voice,
                "model": getattr(args, 'model', None) or provider.default_model,
                "format": fmt,
            },
        }))
        return 0

    # Read input text
    try:
        with open(args.input, 'r', encoding='utf-8') as f:
            text = f.read()
    except Exception as e:
        print(json.dumps({"success": False, "error": f"Failed to read input: {e}"}))
        return 1

    if not text.strip():
        print(json.dumps({"success": False, "error": "Input file is empty"}))
        return 1

    # Validate text length
    if provider.max_text_length and len(text) > provider.max_text_length:
        print(json.dumps({
            "success": False,
            "error": (
                f"Text too long for {provider.name}: {len(text)} chars "
                f"(max {provider.max_text_length})"
            ),
        }))
        return 1

    # Synthesize speech
    try:
        audio_bytes, out_fmt = provider.synthesize(text, args)
    except ValueError as e:
        print(json.dumps({"success": False, "error": str(e)}))
        return 1
    except Exception as e:
        print(json.dumps({"success": False, "error": f"{provider.name}: {e}"}))
        return 1

    # Write output
    try:
        os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)
        with open(output_path, 'wb') as f:
            f.write(audio_bytes)
    except Exception as e:
        print(json.dumps({"success": False, "error": f"Failed to write output: {e}"}))
        return 1

    output_size = os.path.getsize(output_path)

    print(json.dumps({
        "success": True,
        "output": output_path,
        "output_size": output_size,
        "extra": {
            "provider": provider.name,
            "voice": getattr(args, 'voice', None) or provider.default_voice,
            "model": getattr(args, 'model', None) or provider.default_model,
            "format": out_fmt,
            "text_length": len(text),
        },
    }))
    return 0


if __name__ == '__main__':
    sys.exit(main())
