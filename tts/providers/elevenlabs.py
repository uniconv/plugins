"""
ElevenLabs TTS provider.

API: POST https://api.elevenlabs.io/v1/text-to-speech/{voice_id}
Docs: https://elevenlabs.io/docs/api-reference/text-to-speech
"""

import requests

from .base import TTSProvider, raise_api_error


class ElevenLabsProvider(TTSProvider):
    name = "elevenlabs"
    env_key = "ELEVENLABS_API_KEY"
    default_voice = "JBFqnCBsd6RMkjVDRZzb"
    default_model = "eleven_multilingual_v2"
    default_format = "mp3"
    supported_formats = ["mp3"]
    max_text_length = 5000

    def synthesize(self, text, args):
        api_key = self.get_api_key(args)
        fmt = self.resolve_format(args)

        voice = getattr(args, 'voice', None) or self.default_voice
        model = getattr(args, 'model', None) or self.default_model
        stability = getattr(args, 'stability', None)
        if stability is None:
            stability = 0.5
        similarity_boost = getattr(args, 'similarity_boost', None)
        if similarity_boost is None:
            similarity_boost = 0.75
        style = getattr(args, 'style', None)
        if style is None:
            style = 0.0

        payload = {
            "text": text,
            "model_id": model,
            "voice_settings": {
                "stability": stability,
                "similarity_boost": similarity_boost,
                "style": style,
            },
        }

        response = requests.post(
            f"https://api.elevenlabs.io/v1/text-to-speech/{voice}",
            headers={
                "xi-api-key": api_key,
                "Content-Type": "application/json",
                "Accept": "audio/mpeg",
            },
            json=payload,
            timeout=120,
        )

        if response.status_code != 200:
            raise_api_error(self.name, response)

        return response.content, fmt
