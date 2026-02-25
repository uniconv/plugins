"""
TTS provider registry.
"""

from .openai import OpenAIProvider
from .elevenlabs import ElevenLabsProvider
from .typecast import TypecastProvider
from .gemini import GeminiProvider

PROVIDERS = {
    'openai': OpenAIProvider,
    'elevenlabs': ElevenLabsProvider,
    'typecast': TypecastProvider,
    'gemini': GeminiProvider,
}


def get_provider(target):
    """Get a provider instance by target name."""
    target_lower = target.lower()
    cls = PROVIDERS.get(target_lower)
    if cls is None:
        supported = ', '.join(sorted(PROVIDERS.keys()))
        raise ValueError(
            f"Unknown provider: {target}. Supported: {supported}"
        )
    return cls()
