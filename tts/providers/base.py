"""
Base class for TTS providers.
"""

import os
from abc import ABC, abstractmethod


def _load_dotenv():
    """Load .env file from cwd, walking up to filesystem root. Sets os.environ."""
    directory = os.getcwd()
    while True:
        env_path = os.path.join(directory, '.env')
        if os.path.isfile(env_path):
            with open(env_path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#') or '=' not in line:
                        continue
                    key, _, value = line.partition('=')
                    key = key.strip()
                    value = value.strip()
                    # Strip surrounding quotes
                    if len(value) >= 2 and value[0] == value[-1] and value[0] in ('"', "'"):
                        value = value[1:-1]
                    # Don't overwrite existing env vars
                    if key not in os.environ:
                        os.environ[key] = value
            return
        parent = os.path.dirname(directory)
        if parent == directory:
            return
        directory = parent


_load_dotenv()


class TTSProvider(ABC):
    """Abstract base class for TTS providers."""

    name = ""
    env_key = ""
    default_voice = ""
    default_model = ""
    default_format = "mp3"
    supported_formats = []
    max_text_length = 0  # 0 = no limit

    def get_api_key(self, args):
        """Resolve API key from --api-key argument, environment variable, or .env file."""
        key = getattr(args, 'api_key', None)
        if key:
            return key
        key = os.environ.get(self.env_key)
        if key:
            return key
        raise ValueError(
            f"API key required for {self.name}. "
            f"Set --api-key, {self.env_key} environment variable, or add it to .env file."
        )

    def resolve_format(self, args):
        """Resolve and validate output format."""
        # Determine format from output extension or default
        fmt = self.default_format
        if args.output:
            ext = os.path.splitext(args.output)[1].lstrip('.').lower()
            if ext:
                fmt = ext
        if fmt not in self.supported_formats:
            raise ValueError(
                f"Unsupported format '{fmt}' for {self.name}. "
                f"Supported: {', '.join(self.supported_formats)}"
            )
        return fmt

    @abstractmethod
    def synthesize(self, text, args):
        """
        Synthesize speech from text.

        Returns:
            tuple: (audio_bytes, format_string)
        """
        pass


def raise_api_error(provider_name, response):
    """Parse and raise a descriptive error from an HTTP error response."""
    status = response.status_code
    try:
        body = response.json()
        # Try common error message locations
        msg = (
            body.get('error', {}).get('message')
            or body.get('detail', {}).get('message')
            or body.get('message')
            or body.get('error')
            or str(body)
        )
    except Exception:
        msg = response.text[:500] if response.text else "Unknown error"

    if status == 401:
        raise ValueError(f"{provider_name}: Invalid API key (401). {msg}")
    elif status == 429:
        raise ValueError(f"{provider_name}: Rate limit exceeded (429). {msg}")
    elif status == 400:
        raise ValueError(f"{provider_name}: Bad request (400). {msg}")
    elif status >= 500:
        raise ValueError(f"{provider_name}: Server error ({status}). {msg}")
    else:
        raise ValueError(f"{provider_name}: HTTP {status}. {msg}")
