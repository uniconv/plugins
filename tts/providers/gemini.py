"""
Gemini TTS provider.

API: POST https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent
Docs: https://ai.google.dev/gemini-api/docs/text-generation
"""

import base64

import requests

from .base import TTSProvider, raise_api_error


class GeminiProvider(TTSProvider):
    name = "gemini"
    env_key = "GEMINI_API_KEY"
    default_voice = "Kore"
    default_model = "gemini-2.5-flash"
    default_format = "wav"
    supported_formats = ["wav"]
    max_text_length = 5000

    def synthesize(self, text, args):
        api_key = self.get_api_key(args)
        fmt = self.resolve_format(args)

        voice = getattr(args, 'voice', None) or self.default_voice
        model = getattr(args, 'model', None) or self.default_model

        payload = {
            "contents": [
                {
                    "parts": [
                        {
                            "text": text,
                        }
                    ]
                }
            ],
            "generationConfig": {
                "responseModalities": ["AUDIO"],
                "speechConfig": {
                    "voiceConfig": {
                        "prebuiltVoiceConfig": {
                            "voiceName": voice,
                        }
                    }
                }
            }
        }

        url = (
            f"https://generativelanguage.googleapis.com/v1beta/"
            f"models/{model}:generateContent?key={api_key}"
        )

        response = requests.post(
            url,
            headers={"Content-Type": "application/json"},
            json=payload,
            timeout=120,
        )

        if response.status_code != 200:
            raise_api_error(self.name, response)

        result = response.json()

        # Extract audio data from response
        try:
            candidates = result["candidates"]
            parts = candidates[0]["content"]["parts"]
            audio_part = None
            for part in parts:
                if "inlineData" in part:
                    audio_part = part["inlineData"]
                    break
            if not audio_part:
                raise KeyError("No inlineData found")
        except (KeyError, IndexError) as e:
            raise ValueError(
                f"gemini: Unexpected response structure — {e}"
            )

        audio_bytes = base64.b64decode(audio_part["data"])
        return audio_bytes, fmt
