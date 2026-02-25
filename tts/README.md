# tts

Text-to-speech plugin for uniconv. Converts `.txt` files to audio using cloud AI providers.

## Providers

| Provider | Formats | Default Voice | Default Model |
|---|---|---|---|
| `openai` | mp3, opus, aac, flac, wav | `alloy` | `tts-1` |
| `elevenlabs` | mp3 | `JBFqnCBsd6RMkjVDRZzb` | `eleven_multilingual_v2` |
| `typecast` | mp3, wav | `tc_641c10bfb62ae5eee6db3f9e` (Jenna) | `ssfm-v30` |
| `gemini` | wav | `Kore` | `gemini-2.5-flash` |

## API Keys

`.env` 파일 또는 환경변수로 설정:

```
OPENAI_API_KEY=sk-...
ELEVENLABS_API_KEY=...
TYPECAST_API_KEY=...
GEMINI_API_KEY=...
```

`--api-key` 옵션으로 직접 전달도 가능. 우선순위: `--api-key` > 환경변수/`.env` > 에러

## Usage

```bash
# 기본 사용
uniconv speech.txt openai
uniconv speech.txt "typecast --voice tc_641c10bfb62ae5eee6db3f9e"

# 출력 포맷 지정
uniconv speech.txt openai.wav
uniconv speech.txt typecast.mp3

# 옵션과 함께
uniconv speech.txt "openai --voice nova --speed 1.2"
uniconv speech.txt "elevenlabs --stability 0.8 --similarity-boost 0.9"
uniconv speech.txt "typecast --emotion happy --tempo 1.2"

# 출력 경로 지정
uniconv -o greeting.mp3 hello.txt openai

# 파이프라인: 텍스트 생성 → TTS → 포맷 변환
uniconv - "random-lorem.txt --paragraphs 1 --format txt | typecast | aac"
uniconv - "random-lorem.txt --paragraphs 1 --format txt | openai.wav --voice nova"
```

## Provider-specific Options

### OpenAI

- `--speed <float>` — 재생 속도 (0.25-4.0, default: 1.0)
- `--instructions <string>` — 추가 지시문

### ElevenLabs

- `--stability <float>` — 음성 안정성 (0-1, default: 0.5)
- `--similarity-boost <float>` — 음성 유사도 (0-1, default: 0.75)
- `--style <float>` — 스타일 강도 (0-1, default: 0.0)

### Typecast

- `--language <string>` — 언어 코드 (e.g. `en-us`, `ko-kr`, `auto`)
- `--emotion <string>` — 감정 프리셋 (e.g. `happy`, `sad`, `angry`)
- `--volume <int>` — 볼륨 (50-200, default: 100)
- `--pitch <int>` — 피치 (-12~12 semitones, default: 0)
- `--tempo <float>` — 속도 (0.5-2.0, default: 1.0)
- `--seed <int>` — 랜덤 시드
