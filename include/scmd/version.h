#ifndef SCMD_VERSION_H
#define SCMD_VERSION_H

#define SCMD_VERSION "0.13.0"
#define SCMD_CS2_PROFILE "cs2-2026"
/* Source/CS2 console commands are tokenized through a 512-byte buffer.
 * The tokenizer rejects command strings whose byte length is >= 511, so
 * 510 bytes is the largest command we may emit for the cs2-2026 profile. */
#define SCMD_CS2_MAX_COMMAND_BYTES 510u

#endif
