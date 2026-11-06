/*
 * morse.h - plain text to DECtalk Morse code encoder.
 *
 * Turns text into a DECtalk command string that plays it as Morse code.
 * The output holds only two commands: `[:tone freq, ms]` for dots and
 * dashes, `[:pause cs]` for silence. Nothing speakable is emitted.
 *
 * Timing follows standard (PARIS) timing: dot = 1 unit, dash = 3 units,
 * symbol gap = 1 unit, letter gap = 3 units, word gap = 7 units, where
 * one unit is 1200 / wpm milliseconds rounded to a multiple of 10.
 */
#ifndef MORSE_H
#define MORSE_H

/*
 * Encode UTF-8 text as a DECtalk Morse command string.
 *
 * Characters are uppercased, accents folded to their base letter (e.g.
 * e-acute -> E), looked up in the Morse table and emitted as tone/pause
 * commands. Characters without a table entry are dropped; runs of
 * whitespace collapse into a single word gap.
 *
 * Returns a malloc(3)-allocated NUL-terminated string, or NULL on
 * allocation failure. Empty input (or nothing mappable) yields "".
 * The caller frees the result.
 */
char *morse_encode(const char *utf8_text, int wpm, unsigned int pitch_hz);

#endif /* MORSE_H */
