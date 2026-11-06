/*
 * morse.c - plain text to DECtalk Morse code encoder.
 *
 * See morse.h and morse-dectalk-plan.md for the spec this implements.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "morse.h"

/* ---------------------------------------------------------------------- */
/* Morse table: A-Z, 0-9 and punctuation, per morse-dectalk-plan.md        */
/* ---------------------------------------------------------------------- */

struct morse_entry {
    char ch;
    const char *code;
};

static const struct morse_entry MORSE_TABLE[] = {
    {'A', ".-"},     {'B', "-..."},   {'C', "-.-."},   {'D', "-.."},
    {'E', "."},      {'F', "..-."},   {'G', "--."},    {'H', "...."},
    {'I', ".."},     {'J', ".---"},   {'K', "-.-"},    {'L', ".-.."},
    {'M', "--"},     {'N', "-."},     {'O', "---"},    {'P', ".--."},
    {'Q', "--.-"},   {'R', ".-."},    {'S', "..."},    {'T', "-"},
    {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', "-..-"},
    {'Y', "-.--"},   {'Z', "--.."},
    {'0', "-----"},  {'1', ".----"},  {'2', "..---"},  {'3', "...--"},
    {'4', "....-"},  {'5', "....."},  {'6', "-...."},  {'7', "--..."},
    {'8', "---.."},  {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
    {'!', "-.-.--"}, {'(', "-.--."},  {')', "-.--.-"}, {'&', ".-..."},
    {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},  {'+', ".-.-."},
    {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."}, {'$', "...-..-"},
    {'@', ".--.-."},
};

#define MORSE_TABLE_LEN (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

static const char *morse_lookup(char ch)
{
    size_t i;

    for (i = 0; i < MORSE_TABLE_LEN; i++)
        if (MORSE_TABLE[i].ch == ch)
            return MORSE_TABLE[i].code;
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* UTF-8 decoding                                                           */
/* ---------------------------------------------------------------------- */

/*
 * Decode the next code point from *p and advance it. Returns 0 at end of
 * string, 1 otherwise. Invalid bytes are reported as U+FFFD and skipped,
 * which makes them unmappable (and therefore dropped) later on.
 */
static int utf8_next(const char **p, unsigned int *cp)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned char c = s[0];

    if (c == 0)
        return 0;

    if ((c & 0x80) == 0) {
        *cp = c;
        *p += 1;
    } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
              (s[2] & 0x3F);
        *p += 3;
    } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((unsigned)(c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
              ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        *cp = 0xFFFD;
        *p += 1;
    }
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Normalization helpers                                                    */
/* ---------------------------------------------------------------------- */

/* Whitespace collapses into word separators. */
static int is_word_sep(unsigned int cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == '\v' || cp == '\f' ||
           /* common Unicode spaces */
           cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

/* Combining marks are dropped so NFD input folds like NFKD would. */
static int is_combining(unsigned int cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) ||   /* combining diacriticals   */
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||   /* extended combining marks */
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||   /* combining supplement     */
           (cp >= 0x20D0 && cp <= 0x20F0) ||   /* combining symbols        */
           (cp >= 0xFE20 && cp <= 0xFE2F);     /* combining half marks     */
}

/*
 * Fold one code point to uppercase ASCII letters. Writes 1-2 characters
 * into out and returns the count, or 0 when the character has no base
 * letter (accent marks, symbols, other scripts) and should be dropped.
 */
static int fold_codepoint(unsigned int cp, char out[4])
{
    /* ASCII passes through, lowercased letters get uppercased. */
    if (cp < 0x80) {
        out[0] = (cp >= 'a' && cp <= 'z') ? (char)(cp - 0x20) : (char)cp;
        return 1;
    }

    if (is_combining(cp))
        return 0;

    /* Latin-1 Supplement: strip accents from the accented letters. */
    if (cp >= 0x00C0 && cp <= 0x00FF) {
        switch (cp) {
        case 0x00C6: case 0x00E6:            /* ae digraph   */
            out[0] = 'A'; out[1] = 'E'; return 2;
        case 0x00DF:                         /* sharp s      */
            out[0] = 'S'; out[1] = 'S'; return 2;
        case 0x00DE: case 0x00FE:            /* thorn        */
            out[0] = 'T'; out[1] = 'H'; return 2;
        case 0x00D7: case 0x00F7:            /* x, division  */
            return 0;
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
        case 0x00C4: case 0x00C5: case 0x00E0: case 0x00E1:
        case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
            out[0] = 'A'; return 1;
        case 0x00C7: case 0x00E7:
            out[0] = 'C'; return 1;
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
            out[0] = 'E'; return 1;
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
            out[0] = 'I'; return 1;
        case 0x00D0: case 0x00F0:            /* eth          */
            out[0] = 'D'; return 1;
        case 0x00D1: case 0x00F1:
            out[0] = 'N'; return 1;
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
        case 0x00D6: case 0x00D8: case 0x00F2: case 0x00F3:
        case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
            out[0] = 'O'; return 1;
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
            out[0] = 'U'; return 1;
        case 0x00DD: case 0x00FD: case 0x00FF:
            out[0] = 'Y'; return 1;
        default:
            return 0;
        }
    }

    /* Latin Extended-A: table maps both cases straight to base letters. */
    if (cp >= 0x0100 && cp <= 0x017F) {
        static const char *const BASE[0x80] = {
            /* 0100 */ "A", "A", "A", "A", "A", "A", "C", "C",
            /* 0108 */ "C", "C", "C", "C", "C", "C", "D", "D",
            /* 0110 */ "D", "D", "E", "E", "E", "E", "E", "E",
            /* 0118 */ "E", "E", "E", "E", "G", "G", "G", "G",
            /* 0120 */ "G", "G", "G", "G", "H", "H", "H", "H",
            /* 0128 */ "I", "I", "I", "I", "I", "I", "I", "I",
            /* 0130 */ "I", "I", "IJ", "IJ", "J", "J", "K", "K",
            /* 0138 */ "K", "L", "L", "L", "L", "L", "L", "L",
            /* 0140 */ "L", "L", "L", "N", "N", "N", "N", "N",
            /* 0148 */ "N", "N", "N", "N", "O", "O", "O", "O",
            /* 0150 */ "O", "O", "OE", "OE", "R", "R", "R", "R",
            /* 0158 */ "R", "R", "S", "S", "S", "S", "S", "S",
            /* 0160 */ "S", "S", "T", "T", "T", "T", "T", "T",
            /* 0168 */ "U", "U", "U", "U", "U", "U", "U", "U",
            /* 0170 */ "U", "U", "U", "U", "W", "W", "Y", "Y",
            /* 0178 */ "Y", "Z", "Z", "Z", "Z", "Z", "Z", "S",
        };
        const char *base = BASE[cp - 0x0100];
        size_t len = strlen(base);
        memcpy(out, base, len);
        return (int)len;
    }

    /* Everything else (other scripts, symbols, math, ...) is unmapped. */
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Growable output buffer                                                   */
/* ---------------------------------------------------------------------- */

struct sbuf {
    char *s;
    size_t len;
    size_t cap;
};

static int sbuf_grow(struct sbuf *b, size_t needed)
{
    size_t cap = b->cap;
    char *s;

    if (b->len + needed <= cap)
        return 0;
    while (cap < b->len + needed)
        cap = cap ? cap * 2 : 128;
    s = realloc(b->s, cap);
    if (s == NULL)
        return -1;
    b->s = s;
    b->cap = cap;
    return 0;
}

static int sbuf_appendf(struct sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (sbuf_grow(b, (size_t)n + 1) != 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Encoder                                                                  */
/* ---------------------------------------------------------------------- */

/* PARIS timing: one unit is 1200/wpm ms, rounded to a multiple of 10 so
 * every pause stays a whole number of centiseconds. */
static int unit_ms_for(int wpm)
{
    int units = (int)(1200.0 / wpm / 10.0 + 0.5);

    if (units < 1)
        units = 1;
    return units * 10;
}

char *morse_encode(const char *utf8_text, int wpm, unsigned int pitch_hz)
{
    struct sbuf out = { NULL, 0, 0 };
    const char *p = utf8_text;
    int pending_units = 0;      /* gap owed before the next symbol */
    int unit_ms, unit_cs;
    unsigned int cp;

    if (wpm < 1)
        wpm = 1;
    if (wpm > 120)
        wpm = 120;
    unit_ms = unit_ms_for(wpm);
    unit_cs = unit_ms / 10;

    /* Gaps are owed lazily: a word gap only becomes a real [:pause] when a
     * mapped letter follows, which keeps the stream free of leading and
     * trailing pauses and lets unmappable-only words vanish gaplessly. */
    while (utf8_next(&p, &cp)) {
        char folded[4];
        int nfolds, k;

        if (is_word_sep(cp)) {
            if (out.len > 0)
                pending_units = 7;
            continue;
        }

        nfolds = fold_codepoint(cp, folded);
        for (k = 0; k < nfolds; k++) {
            const char *syms = morse_lookup(folded[k]);
            const char *s;

            if (syms == NULL)
                continue;               /* no entry: drop, no gap */
            if (pending_units > 0) {
                if (sbuf_appendf(&out, "[:pause %d]",
                                 pending_units * unit_cs) != 0)
                    goto fail;
                pending_units = 0;
            }
            for (s = syms; *s; s++) {
                int dur = (*s == '-') ? 3 * unit_ms : unit_ms;

                if (s != syms)
                    if (sbuf_appendf(&out, "[:pause %d]", unit_cs) != 0)
                        goto fail;
                if (sbuf_appendf(&out, "[:tone %u,%d]", pitch_hz, dur) != 0)
                    goto fail;
            }
            pending_units = 3;          /* letter gap */
        }
    }

    if (sbuf_grow(&out, 1) != 0)
        goto fail;
    out.s[out.len] = '\0';
    return out.s;

fail:
    free(out.s);
    return NULL;
}
