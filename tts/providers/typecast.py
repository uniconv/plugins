"""
Typecast TTS provider.

API: POST https://api.typecast.ai/v1/text-to-speech
Docs: https://typecast.ai/docs/api-reference/endpoint/text-to-speech/text-to-speech
"""

import requests

from .base import TTSProvider, raise_api_error


class TypecastProvider(TTSProvider):
    name = "typecast"
    env_key = "TYPECAST_API_KEY"
    default_voice = "tc_641c10bfb62ae5eee6db3f9e"  # Jenna
    default_model = "ssfm-v30"
    default_format = "mp3"
    supported_formats = ["mp3", "wav"]
    max_text_length = 2000

    def synthesize(self, text, args):
        api_key = self.get_api_key(args)
        fmt = self.resolve_format(args)

        voice = getattr(args, 'voice', None) or self.default_voice
        model = getattr(args, 'model', None) or self.default_model
        language = getattr(args, 'language', None)
        emotion = getattr(args, 'emotion', None)
        volume = getattr(args, 'volume', None)
        if volume is None:
            volume = 100
        pitch = getattr(args, 'pitch', None)
        if pitch is None:
            pitch = 0
        tempo = getattr(args, 'tempo', None)
        if tempo is None:
            tempo = 1.0
        seed = getattr(args, 'seed', None)

        payload = {
            "voice_id": voice,
            "text": text,
            "model": model,
            "output": {
                "volume": int(volume),
                "audio_pitch": int(pitch),
                "audio_tempo": tempo,
                "audio_format": fmt,
            },
        }
        if language:
            payload["language"] = language
        if emotion:
            payload["prompt"] = {"emotion_type": emotion}
        if seed is not None:
            payload["seed"] = seed

        response = requests.post(
            "https://api.typecast.ai/v1/text-to-speech",
            headers={
                "X-API-KEY": api_key,
                "Content-Type": "application/json",
            },
            json=payload,
            timeout=120,
        )

        if response.status_code != 200:
            raise_api_error(self.name, response)

        return response.content, fmt
