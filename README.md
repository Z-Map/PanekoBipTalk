# PankoBipTalk

A small command-line front end for the [DECtalk](./dectalk) speech
engine. Speaks text normally; with `--wpm` it turns text into Morse code
and plays it as pure tone commands instead.

## Build

```sh
make
```

The first run also builds the `dectalk` submodule (autotools, a few
minutes). The result is installed as `dectalk/dist/say`, next to the
engine's config and dictionaries. Prerequisites: `build-essential`,
`libasound2-dev`, `libpulse-dev`, `autoconf`, `automake`.

## Usage

```sh
dectalk/dist/say [options] [text...]        # text from arguments
echo "text" | dectalk/dist/say              # or from stdin
```

| Option       | Meaning                                              |
|--------------|------------------------------------------------------|
| `--wpm N`    | Convert to Morse at N words per minute, 5-60; without this, text is spoken as-is |
| `--pitch HZ` | Morse tone frequency, 100-5000 Hz (default 800, Morse mode only) |
| `--dry-run`  | Print what would be sent to the engine instead of playing |
| `-w FILE`    | Write a wave file instead of using the audio device  |
| `-h`         | Help                                                 |

```sh
dectalk/dist/say "hello world"                        # plain TTS
dectalk/dist/say --wpm 20 --pitch 600 "hello world"   # Morse at 20 WPM
dectalk/dist/say --dry-run --wpm 15 "SOS"             # inspect the commands
dectalk/dist/say -w message.wav "hi there"            # TTS to wave file
dectalk/dist/say -w sos.wav --wpm 15 "SOS"            # Morse to wave file
```

Morse mode emits only `[:tone freq,ms]` and `[:pause cs]` commands
(standard PARIS timing, see [docs/morse-dectalk-plan.md](./docs/morse-dectalk-plan.md)).
When writing a wave file, pauses are rewritten as silent `[:tone 0,ms]`
tones, because `[:pause]` produces no samples and would leave the
recording with all its gaps missing.

## Files

- `src/main.c` - CLI wrapper around the DECtalk API (plain TTS + Morse)
- `src/morse.c` / `src/morse.h` - the Morse encoder, dependency-free
- `docs/` - notes on the Morse encoding
- `dectalk/` - the engine, an unmodified git submodule
