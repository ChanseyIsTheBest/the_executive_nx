/* exec_decode.h -- on-device streaming audio decode. MIT, see LICENSE.
 *
 * Adapted from sonicjump_nx/source/sj_decode.c, which solved this exact
 * problem: a soundtrack of .m4a files and a console with no AAC decoder.
 *
 * Backed by ffmpeg (switch-ffmpeg from devkitPro portlibs), so .m4a (AAC in
 * MP4) plays straight from the APK's assets with no PC-side conversion. .ogg,
 * .mp3 and .wav work through the same path, so a user who converted their
 * music anyway is not penalised.
 *
 * Every file call goes through exec_io's lock: this runs on the decode thread,
 * beside the engine's own file access, and devkitPro's newlib keeps a
 * process-wide handle table that is not thread-safe.
 *
 * Output is always interleaved signed 16-bit STEREO at the requested rate.
 */
#ifndef EXEC_DECODE_H
#define EXEC_DECODE_H

#include <stdint.h>

typedef struct ExecDecoder ExecDecoder;

typedef struct {
    int         src_rate;      /* the file's own sample rate  */
    int         src_channels;  /* the file's own channel count */
    int         duration_ms;   /* 0 if the container does not say */
    const char *codec_name;    /* "aac", "vorbis", ... (owned by ffmpeg) */
} ExecDecodeInfo;

/* Open `path`, converting to stereo S16 at `out_rate`. NULL on failure.
 * `info` may be NULL. */
ExecDecoder *exec_decoder_open(const char *path, int out_rate, ExecDecodeInfo *info);

/* Fill `dst` with up to `frames` stereo frames. Returns frames written; a
 * short count means end of stream. */
int exec_decoder_read(ExecDecoder *d, int16_t *dst, int frames);

/* Seek back to the start for looping. Returns 1 on success. */
int exec_decoder_rewind(ExecDecoder *d);

/* Seek to `seconds`, for play()'s start offset. Returns 1 on success.
 * Zero or negative rewinds. */
int exec_decoder_seek(ExecDecoder *d, float seconds);

void exec_decoder_close(ExecDecoder *d);

#endif
