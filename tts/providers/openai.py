"""
OpenAI TTS provider.

API: POST https://api.openai.com/v1/audio/speech
Docs: https://platform.openai.com/docs/api-reference/audio/createSpeech
"""

import requests

from .base import TTSProvider, raise_api_error


class OpenAIProvider(TTSProvider):
    name = "openai"
    env_key = "OPENAI_API_KEY"
    default_voice = "alloy"
    default_model = "tts-1"
    default_format = "mp3"
    supported_formats = ["mp3", "opus", "aac", "flac", "wav"]
    max_text_length = 4096

    def synthesize(self, text, args):
        api_key = self.get_api_key(args)
        fmt = self.resolve_format(args)

        voice = getattr(args, 'voice', None) or self.default_voice
        model = getattr(args, 'model', None) or self.default_model
        speed = getattr(args, 'speed', None) or 1.0
        instructions = getattr(args, 'instructions', None)

        payload = {
            "model": model,
            "input": text,
            "voice": voice,
            "response_format": fmt,
            "speed": speed,
        }
        if instructions:
            payload["instructions"] = instructions

        response = requests.post(
            "https://api.openai.com/v1/audio/speech",
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            json=payload,
            timeout=120,
        )

        if response.status_code != 200:
            raise_api_error(self.name, response)

        return response.content, fmt
