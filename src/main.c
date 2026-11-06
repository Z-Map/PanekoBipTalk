/*
 * main.c - DECtalk `say` with optional Morse code mode.
 *
 * Plain TTS by default: text is spoken normally, like DECtalk's classic
 * say sample (dectalk/src/samplosf/src/dtsamples/say.c), including its
 * iconv-based conversion of input to the engine's Windows-1252 charset.
 *
 * With --wpm the text is converted to Morse code first (see morse.c /
 * morse-dectalk-plan.md) and the engine receives a stream made only of
 * [:tone] and [:pause] commands.
 */

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dtk/ttsapi.h>

#include "morse.h"

#define DEFAULT_WPM   15
#define DEFAULT_PITCH 800

#define MIN_WPM   5
#define MAX_WPM   60
#define MIN_PITCH 100
#define MAX_PITCH 5000    /* engine rejects anything above sample rate / 2 */

/* Feed the engine in chunks of at most this many characters. Morse chunks
 * are cut on command boundaries, text chunks on whitespace. */
#define CHUNK_SIZE 8192

#define CONV_ALLOC 4096

enum long_options_ids {
    OPT_WPM = 1000,
    OPT_PITCH,
    OPT_DRY_RUN,
};

/* iconv descriptor used to convert input to the engine's charset. */
static iconv_t tts_cd = (iconv_t)-1;

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "Usage: %s [options] [text...]\n"
        "\n"
        "Speak text through DECtalk. Plain TTS by default; with --wpm the\n"
        "text is converted to Morse code (standard PARIS timing) first and\n"
        "played as tone commands.\n"
        "\n"
        "Options:\n"
        "  --wpm N      Convert to Morse code at N words per minute, %d-%d\n"
        "               (without this option the text is spoken as-is)\n"
        "  --pitch HZ   Morse tone frequency in Hz, %d-%d (default %d,\n"
        "               Morse mode only)\n"
        "  --dry-run    Print what would be sent to the engine instead of\n"
        "               speaking it\n"
        "  -w FILE      Write a wave file instead of using the audio\n"
        "               device\n"
        "  -h, --help   Show this help\n"
        "  --           Treat the remaining arguments as text\n"
        "\n"
        "Text is taken from the command line, or from standard input when\n"
        "no text arguments are given.\n",
        prog, MIN_WPM, MAX_WPM, MIN_PITCH, MAX_PITCH, DEFAULT_PITCH);
}

static void die(const char *msg)
{
    fprintf(stderr, "say: %s\n", msg);
    exit(1);
}

static int parse_int_arg(const char *what, const char *s, int min, int max,
                         int def)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < INT_MIN ||
        v > INT_MAX) {
        fprintf(stderr, "say: invalid %s '%s', using %d\n", what, s, def);
        return def;
    }
    if (v < min) {
        fprintf(stderr, "say: %s %ld below minimum %d, clamping\n", what, v,
                min);
        return min;
    }
    if (v > max) {
        fprintf(stderr, "say: %s %ld above maximum %d, clamping\n", what, v,
                max);
        return max;
    }
    return (int)v;
}

/* Join the text arguments (already past any options) with spaces. */
static char *join_args(int argc, char **argv, int first)
{
    size_t total = 0;
    char *buf;
    int i;

    for (i = first; i < argc; i++)
        total += strlen(argv[i]) + 1;
    buf = malloc(total + 1);
    if (buf == NULL)
        die("out of memory");
    buf[0] = '\0';
    for (i = first; i < argc; i++) {
        if (i > first)
            strcat(buf, " ");
        strcat(buf, argv[i]);
    }
    return buf;
}

/* Read all of standard input. */
static char *read_stdin(void)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    size_t n;

    if (buf == NULL)
        die("out of memory");
    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (cap - len < 2) {
            char *tmp;

            cap *= 2;
            tmp = realloc(buf, cap);
            if (tmp == NULL)
                die("out of memory");
            buf = tmp;
        }
    }
    buf[len] = '\0';
    return buf;
}

/*
 * Prepare the iconv descriptor that converts input from the current
 * locale charset to what the DECtalk API expects (Windows-1252). Ported
 * from the say sample's startup sequence.
 */
static void open_dapi_converter(void)
{
    static const char *const targets[] = {
        "Windows-1252//TRANSLIT//IGNORE",
        "ISO-8859-15//TRANSLIT//IGNORE",
        "ISO-8859-1//TRANSLIT//IGNORE",
        "Windows-1252",
        "ISO-8859-15",
        "ISO-8859-1",
    };
    const char *from;
    size_t i;

    setlocale(LC_CTYPE, "");
    from = nl_langinfo(CODESET);

    for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        tts_cd = iconv_open(targets[i], from);
        if (tts_cd != (iconv_t)-1)
            return;
    }
    perror("say: iconv_open");
    exit(1);
}

/* Convert input text to the engine's charset. Ported from the say
 * sample's convert_string_for_dapi(). Returns a malloc'd string. */
static char *convert_to_dapi(const char *in)
{
    size_t outsize = CONV_ALLOC;
    size_t outleft = outsize;
    size_t inleft = strlen(in);
    size_t r, offset;
    char *out, *outp;
    char *inp = (char *)in;

    out = malloc(outsize + 1);
    if (out == NULL) {
        perror("say: malloc");
        exit(1);
    }
    outp = out;

    do {
        memset(outp, 0, outleft + 1);
        errno = 0;
        r = iconv(tts_cd, &inp, &inleft, &outp, &outleft);
        if (r == (size_t)-1 && errno == E2BIG) {
            offset = outp - out;
            outsize += CONV_ALLOC;
            out = realloc(out, outsize + 1);
            if (out == NULL) {
                perror("say: realloc");
                exit(1);
            }
            outleft += CONV_ALLOC;
            outp = out + offset;
        } else if (r == (size_t)-1) {
            if (inleft > 0) {
                /* skip the unconvertible byte */
                inp++;
                inleft--;
            } else {
                perror("say: iconv");
                exit(1);
            }
        }
    } while (inleft > 0);

    iconv(tts_cd, NULL, NULL, NULL, NULL);
    return out;
}

/*
 * `[:pause N]` sleeps the engine's pipeline without generating samples, so
 * pauses vanish from wave *recordings*. For -w output in Morse mode,
 * rewrite each pause as a 0 Hz tone of the same duration (N cs ->
 * N*10 ms): the engine renders that as exact-duration silence, keeping
 * Morse timing intact in the file. Live playback still uses [:pause]
 * (audible real-time gap).
 */
static char *pauses_to_silence_tones(const char *morse)
{
    /* Each rewrite grows slightly: "[:pause N]" -> "[:tone 0,N0]".
     * Allocate generously; the input is ASCII commands only. */
    size_t cap = strlen(morse) * 2 + 16;
    char *out = malloc(cap);
    size_t used = 0;
    const char *p = morse;

    if (out == NULL)
        return NULL;

    while (*p != '\0') {
        int cs;

        if (strncmp(p, "[:pause ", 8) == 0 &&
            sscanf(p + 8, "%d]", &cs) == 1 && cs >= 0) {
            int n;

            while (*p != ']' && *p != '\0')
                p++;
            if (*p == ']')
                p++;
            n = snprintf(out + used, cap - used, "[:tone 0,%d]", cs * 10);
            if (n < 0 || (size_t)n >= cap - used) {
                free(out);
                return NULL;
            }
            used += (size_t)n;
        } else {
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    return out;
}

/* Feed a Morse command stream in chunks aligned to command boundaries so
 * a [:tone]/[:pause] is never split. */
static MMRESULT speak_morse(LPTTS_HANDLE_T tts, const char *morse)
{
    char *chunk;
    size_t used = 0, i = 0;
    MMRESULT status = MMSYSERR_NOERROR;

    chunk = malloc(CHUNK_SIZE + 1);
    if (chunk == NULL) {
        fprintf(stderr, "say: out of memory\n");
        return MMSYSERR_NOMEM;
    }

    while (morse[i] != '\0') {
        size_t j = i;

        /* find the end of the next complete [:...] command */
        while (morse[j] != '\0' && morse[j] != ']')
            j++;
        if (morse[j] == ']')
            j++;

        if (used > 0 && used + (j - i) > CHUNK_SIZE) {
            chunk[used] = '\0';
            status = TextToSpeechSpeak(tts, chunk, TTS_NORMAL);
            if (status != MMSYSERR_NOERROR)
                break;
            used = 0;
        }
        if (j - i > CHUNK_SIZE) {
            /* cannot happen with our command sizes; guard anyway */
            fprintf(stderr, "say: internal error, oversized command\n");
            status = MMSYSERR_INVALPARAM;
            break;
        }
        memcpy(chunk + used, morse + i, j - i);
        used += j - i;
        i = j;
    }

    if (status == MMSYSERR_NOERROR && used > 0) {
        chunk[used] = '\0';
        status = TextToSpeechSpeak(tts, chunk, TTS_FORCE);
    }

    free(chunk);
    return status;
}

/* Feed plain text in chunks aligned to whitespace (separators are kept
 * with the preceding chunk, so nothing is lost between chunks). */
static MMRESULT speak_text(LPTTS_HANDLE_T tts, const char *text)
{
    char *chunk;
    size_t len = strlen(text), pos = 0;
    MMRESULT status = MMSYSERR_NOERROR;

    chunk = malloc(CHUNK_SIZE + 1);
    if (chunk == NULL) {
        fprintf(stderr, "say: out of memory\n");
        return MMSYSERR_NOMEM;
    }

    while (pos < len) {
        size_t n = len - pos;
        DWORD flags;

        if (n > CHUNK_SIZE) {
            size_t cut = pos + CHUNK_SIZE;

            while (cut > pos && !isspace((unsigned char)text[cut]))
                cut--;
            n = (cut > pos) ? cut - pos + 1 : CHUNK_SIZE;
        }

        flags = (pos + n >= len) ? TTS_FORCE : TTS_NORMAL;
        memcpy(chunk, text + pos, n);
        chunk[n] = '\0';
        status = TextToSpeechSpeak(tts, chunk, flags);
        if (status != MMSYSERR_NOERROR)
            break;
        pos += n;
    }

    free(chunk);
    return status;
}

int main(int argc, char *argv[])
{
    int wpm = DEFAULT_WPM;
    int pitch = DEFAULT_PITCH;
    int morse_mode = 0;
    int pitch_given = 0;
    int dry_run = 0;
    char *wavfile = NULL;
    char *text = NULL;
    char *payload = NULL;       /* what gets fed to the engine */
    LPTTS_HANDLE_T tts = NULL;
    DWORD dev_options;
    MMRESULT status;
    int opt;
    int exit_code = 0;

    static const struct option long_opts[] = {
        { "wpm",     required_argument, NULL, OPT_WPM },
        { "pitch",   required_argument, NULL, OPT_PITCH },
        { "dry-run", no_argument,       NULL, OPT_DRY_RUN },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "w:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_WPM:
            wpm = parse_int_arg("--wpm", optarg, MIN_WPM, MAX_WPM,
                                DEFAULT_WPM);
            morse_mode = 1;
            break;
        case OPT_PITCH:
            pitch = parse_int_arg("--pitch", optarg, MIN_PITCH, MAX_PITCH,
                                  DEFAULT_PITCH);
            pitch_given = 1;
            break;
        case OPT_DRY_RUN:
            dry_run = 1;
            break;
        case 'w':
            wavfile = optarg;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (pitch_given && !morse_mode)
        fprintf(stderr,
                "say: note: --pitch applies to Morse mode (--wpm), "
                "speaking text as-is\n");

    if (optind < argc)
        text = join_args(argc, argv, optind);
    else
        text = read_stdin();

    if (dry_run) {
        if (morse_mode) {
            payload = morse_encode(text, wpm, (unsigned int)pitch);
            if (payload == NULL)
                die("out of memory encoding morse");
        } else {
            payload = text;
        }
        if (wavfile != NULL)
            fprintf(stderr,
                    "say: warning: --dry-run ignores -w, printing commands\n");
        printf("%s\n", payload);
        if (payload != text)
            free(payload);
        free(text);
        return 0;
    }

    if (morse_mode) {
        payload = morse_encode(text, wpm, (unsigned int)pitch);
        if (payload == NULL)
            die("out of memory encoding morse");
        if (payload[0] == '\0') {
            fprintf(stderr,
                    "say: nothing in the input maps to Morse code\n");
            free(payload);
            free(text);
            return 0;
        }
    } else {
        if (text[0] == '\0') {
            fprintf(stderr, "say: no input to speak\n");
            free(text);
            return 0;
        }
    }

    /* Wave output never needs an audio device; live playback shares it. */
    dev_options = (wavfile != NULL) ? DO_NOT_USE_AUDIO_DEVICE
                                    : WAVE_OPEN_SHAREABLE;

    status = TextToSpeechStartup(&tts, WAVE_MAPPER, dev_options, NULL, 0);
    if (status != MMSYSERR_NOERROR) {
        switch (status) {
        case MMSYSERR_NODRIVER:
            fprintf(stderr, "say: could not find any wave devices\n");
            break;
        case MMSYSERR_NOTENABLED:
            fprintf(stderr, "say: DECtalk license not found\n");
            break;
        case MMSYSERR_ALLOCATED:
            fprintf(stderr, "say: DECtalk exceeded its license quota\n");
            break;
        case MMSYSERR_NOMEM:
            fprintf(stderr, "say: DECtalk ran out of memory\n");
            break;
        default:
            fprintf(stderr,
                    "say: TextToSpeechStartup failed with code %d\n", status);
            break;
        }
        free(payload);
        free(text);
        return 1;
    }

    if (wavfile != NULL) {
        status = TextToSpeechOpenWaveOutFile(tts, wavfile, WAVE_FORMAT_1M16);
        if (status != MMSYSERR_NOERROR) {
            fprintf(stderr, "say: cannot open wave file '%s'\n", wavfile);
            TextToSpeechShutdown(tts);
            free(payload);
            free(text);
            return 1;
        }
    }

    if (morse_mode) {
        if (wavfile != NULL) {
            /* pauses carry no samples into recordings; use silence tones */
            char *spoken = pauses_to_silence_tones(payload);

            if (spoken == NULL) {
                fprintf(stderr, "say: out of memory\n");
                exit_code = 1;
            } else {
                free(payload);
                payload = spoken;
            }
        }
        if (exit_code == 0) {
            status = speak_morse(tts, payload);
            if (status != MMSYSERR_NOERROR) {
                fprintf(stderr,
                        "say: TextToSpeechSpeak failed with code %d\n",
                        status);
                exit_code = 1;
            }
        }
    } else {
        open_dapi_converter();
        payload = convert_to_dapi(text);
        status = speak_text(tts, payload);
        if (status != MMSYSERR_NOERROR) {
            fprintf(stderr, "say: TextToSpeechSpeak failed with code %d\n",
                    status);
            exit_code = 1;
        }
        iconv_close(tts_cd);
        tts_cd = (iconv_t)-1;
    }

    /* Make sure everything has come out before closing the file. */
    TextToSpeechSync(tts);

    if (wavfile != NULL) {
        status = TextToSpeechCloseWaveOutFile(tts);
        if (status != MMSYSERR_NOERROR) {
            fprintf(stderr, "say: TextToSpeechCloseWaveOutFile failed with "
                            "code %d\n", status);
            exit_code = 1;
        }
    }

    if (TextToSpeechShutdown(tts) != MMSYSERR_NOERROR)
        fprintf(stderr, "say: TextToSpeechShutdown failed\n");

    free(payload);
    free(text);
    return exit_code;
}
