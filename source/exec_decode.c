/* exec_decode.c -- streaming audio decode on the Switch, via ffmpeg.
 *
 * MIT licensed. See LICENSE.
 *
 * Adapted from sonicjump_nx/source/sj_decode.c, which solved this exact
 * problem for a soundtrack of a couple of dozen .m4a files.
 *
 * WHY FFMPEG
 * ----------
 * The Executive' music is AAC-LC in an MP4 container and the Switch has
 * no AAC decoder of its own. An earlier version of this port transcoded the
 * tracks to Ogg on the user's PC, and that was a poor deal: it made ffmpeg a
 * setup requirement, it rewrote the user's asset files, and someone who
 * skipped the step got a silent game with no obvious cause.
 *
 * devkitPro ships switch-ffmpeg as a portlib, so we decode on-device instead.
 * The assets stay exactly as they came out of the APK.
 *
 * Using libavformat rather than a bare AAC decoder matters: .m4a is AAC inside
 * MP4, so something has to parse the container, find the audio stream, and
 * hand up whole frames. Doing that by hand is the bulk of the work and all of
 * the bugs.
 *
 * The same path handles .ogg, .mp3 and .wav, so a user who already converted
 * their music (or who prefers the smaller files) is not penalised.
 *
 * TWO CHANGES FROM THE ORIGINAL
 * -----------------------------
 * Every file call goes through exec_io's lock: this runs on the decode thread,
 * beside the engine's own file access, and devkitPro's newlib keeps a
 * process-wide handle table that is not thread-safe.
 *
 * ffmpeg's log is routed into the port's own log. Every one of this game's
 * tracks emits "Could not update timestamps for skipped samples" on open,
 * because the MP4 carries an edit list for the encoder priming, and one such
 * line per track would bury a real failure.
 *
 * OUTPUT CONTRACT
 * ---------------
 * Always interleaved signed 16-bit stereo at the rate the caller asks for.
 * libswresample does the rate, layout and format conversion, so the mixer
 * never has to think about what the source actually was.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exec_decode.h"
#include "exec_io.h"
#include "exec_log.h"
#include <stdarg.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#define OUT_CHANNELS 2

/* --- custom I/O ------------------------------------------------------------
 * avformat_open_input() parses its filename as a URL, so "sdmc:/switch/..."
 * is read as protocol "sdmc" -- which does not exist -- and the open fails
 * before it ever touches the file. That is not a corner case here: sdmc: is
 * the normal device prefix on Switch, and it is exactly what exec_dir() hands
 * out.
 *
 * Rather than mangle paths to suit ffmpeg's parser, hand it an AVIOContext
 * backed by plain fopen(). ffmpeg then never sees the path at all, and the
 * music path reads files the same way as the rest of the port.
 * ------------------------------------------------------------------------ */
#define EXEC_IO_BUFFER 32768

static int exec_io_read(void *opaque, uint8_t *buf, int buf_size)
{
    FILE *f = (FILE *)opaque;
    size_t n = fread_locked(buf, 1, (size_t)buf_size, f);
    if (n == 0) return AVERROR_EOF;
    return (int)n;
}

static int64_t exec_io_seek(void *opaque, int64_t offset, int whence)
{
    FILE *f = (FILE *)opaque;
    long cur, end;

    /* MP4 needs the total size to locate its index; ffmpeg asks via
     * AVSEEK_SIZE rather than by seeking to the end itself. */
    if (whence == AVSEEK_SIZE) {
        cur = ftell_locked(f);
        if (cur < 0 || fseek_locked(f, 0, SEEK_END) != 0) return -1;
        end = ftell_locked(f);
        if (fseek_locked(f, cur, SEEK_SET) != 0) return -1;
        return (int64_t)end;
    }
    if (fseek_locked(f, (long)offset, whence) != 0) return -1;
    return (int64_t)ftell_locked(f);
}

struct ExecDecoder {
    FILE            *file;
    AVIOContext     *avio;
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frame;
    int              stream_index;
    int              out_rate;

    /* Converted samples that did not fit in the caller's buffer last time.
     * swr_convert produces whole frames' worth, which rarely lines up with the
     * mixer's block size, so the remainder has to be carried. */
    int16_t         *pending;
    int              pending_cap;    /* frames */
    int              pending_len;    /* frames */
    int              pending_pos;    /* frames already handed out */

    int              eof;
};

static void free_pending(ExecDecoder *d)
{
    free(d->pending);
    d->pending = NULL;
    d->pending_cap = d->pending_len = d->pending_pos = 0;
}

void exec_decoder_close(ExecDecoder *d)
{
    if (!d) return;
    if (d->swr)   swr_free(&d->swr);
    if (d->frame) av_frame_free(&d->frame);
    if (d->pkt)   av_packet_free(&d->pkt);
    if (d->dec)   avcodec_free_context(&d->dec);
    if (d->fmt)   avformat_close_input(&d->fmt);
    /* avformat_close_input does not free a caller-supplied AVIOContext, and
     * ffmpeg may have replaced the buffer we handed it, so free what the
     * context currently points at rather than the original pointer. */
    if (d->avio) {
        av_freep(&d->avio->buffer);
        avio_context_free(&d->avio);
    }
    if (d->file)  fclose_locked(d->file);
    free_pending(d);
    free(d);
}

/* ffmpeg logs to stderr by default, which on Switch goes nowhere useful, and
 * the AAC decoder is chatty: every one of this game's tracks emits "Could not
 * update timestamps for skipped samples" on open, because the MP4 carries an
 * edit list for the encoder priming. Route it into the port's own log at a
 * level where that stays quiet but a real failure still says why. */
static void exec_av_log(void *avcl, int level, const char *fmt, va_list ap)
{
    (void)avcl;
    if (level > AV_LOG_ERROR) return;
    exec_log_vprint(EXEC_LOG_WARN, "ffmpeg", fmt, ap);
}

ExecDecoder *exec_decoder_open(const char *path, int out_rate, ExecDecodeInfo *info)
{
    static int log_installed;
    if (!log_installed) {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(exec_av_log);
        log_installed = 1;
    }

    ExecDecoder *d;
    const AVCodec *codec = NULL;
    AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
    int rc;

    if (!path || out_rate <= 0) return NULL;

    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->out_rate = out_rate;
    d->stream_index = -1;

    d->file = fopen_locked(path, "rb");
    if (!d->file) {
        exec_decoder_close(d);
        return NULL;
    }
    {
        unsigned char *iobuf = av_malloc(EXEC_IO_BUFFER);
        if (!iobuf) { exec_decoder_close(d); return NULL; }
        d->avio = avio_alloc_context(iobuf, EXEC_IO_BUFFER, 0, d->file,
                                     exec_io_read, NULL, exec_io_seek);
        if (!d->avio) { av_free(iobuf); exec_decoder_close(d); return NULL; }
    }
    d->fmt = avformat_alloc_context();
    if (!d->fmt) { exec_decoder_close(d); return NULL; }
    d->fmt->pb    = d->avio;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* NULL filename: ffmpeg probes the container from the stream, so nothing
     * is ever parsed as a URL. */
    if (avformat_open_input(&d->fmt, NULL, NULL, NULL) < 0) {
        exec_decoder_close(d);
        return NULL;
    }
    if (avformat_find_stream_info(d->fmt, NULL) < 0) {
        exec_decoder_close(d);
        return NULL;
    }

    rc = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (rc < 0 || !codec) {
        exec_decoder_close(d);
        return NULL;
    }
    d->stream_index = rc;

    d->dec = avcodec_alloc_context3(codec);
    if (!d->dec) { exec_decoder_close(d); return NULL; }
    if (avcodec_parameters_to_context(d->dec,
            d->fmt->streams[d->stream_index]->codecpar) < 0) {
        exec_decoder_close(d);
        return NULL;
    }
    /* One thread. The audio is decoded ahead of time on a worker thread
     * already (exec_audio.c), and ffmpeg's own threading would add latency and
     * another set of stacks for no gain on a 23-second music loop. */
    d->dec->thread_count = 1;

    if (avcodec_open2(d->dec, codec, NULL) < 0) {
        exec_decoder_close(d);
        return NULL;
    }

    /* Resample/downmix everything to interleaved S16 stereo at out_rate. */
    rc = swr_alloc_set_opts2(&d->swr,
                             &out_layout, AV_SAMPLE_FMT_S16, out_rate,
                             &d->dec->ch_layout, d->dec->sample_fmt,
                             d->dec->sample_rate,
                             0, NULL);
    if (rc < 0 || !d->swr || swr_init(d->swr) < 0) {
        exec_decoder_close(d);
        return NULL;
    }

    d->pkt   = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->pkt || !d->frame) { exec_decoder_close(d); return NULL; }

    if (info) {
        info->src_rate     = d->dec->sample_rate;
        info->src_channels = d->dec->ch_layout.nb_channels;
        info->codec_name   = codec->name;
        info->duration_ms  = (d->fmt->duration > 0)
                           ? (int)(d->fmt->duration / (AV_TIME_BASE / 1000))
                           : 0;
    }
    return d;
}

/* Grow the carry buffer to hold at least `frames`. */
static int ensure_pending(ExecDecoder *d, int frames)
{
    int16_t *p;
    if (d->pending_cap >= frames) return 1;
    p = realloc(d->pending, (size_t)frames * OUT_CHANNELS * sizeof(int16_t));
    if (!p) return 0;
    d->pending = p;
    d->pending_cap = frames;
    return 1;
}

/* Move up to `want` frames out of the carry buffer. */
static int take_pending(ExecDecoder *d, int16_t *dst, int want)
{
    int avail = d->pending_len - d->pending_pos;
    int n = avail < want ? avail : want;
    if (n <= 0) return 0;
    memcpy(dst, d->pending + (size_t)d->pending_pos * OUT_CHANNELS,
           (size_t)n * OUT_CHANNELS * sizeof(int16_t));
    d->pending_pos += n;
    if (d->pending_pos >= d->pending_len) d->pending_pos = d->pending_len = 0;
    return n;
}

/* Decode one packet's worth into the carry buffer. Returns 1 if it produced
 * samples, 0 at end of stream, -1 on a hard error. */
static int fill_pending(ExecDecoder *d)
{
    for (;;) {
        int rc = avcodec_receive_frame(d->dec, d->frame);

        if (rc == 0) {
            int max_out = (int)swr_get_out_samples(d->swr, d->frame->nb_samples);
            uint8_t *out[1];
            int got;

            if (max_out <= 0) max_out = d->frame->nb_samples + 256;
            if (!ensure_pending(d, max_out)) return -1;

            out[0] = (uint8_t *)d->pending;
            got = swr_convert(d->swr, out, max_out,
                              (const uint8_t **)d->frame->data,
                              d->frame->nb_samples);
            av_frame_unref(d->frame);
            if (got < 0) return -1;
            if (got == 0) continue;          /* swr buffered it; keep going */
            d->pending_len = got;
            d->pending_pos = 0;
            return 1;
        }

        if (rc == AVERROR_EOF) {
            /* Flush whatever libswresample is still holding. */
            int max_out = (int)swr_get_out_samples(d->swr, 0);
            uint8_t *out[1];
            int got;
            if (max_out <= 0) return 0;
            if (!ensure_pending(d, max_out)) return -1;
            out[0] = (uint8_t *)d->pending;
            got = swr_convert(d->swr, out, max_out, NULL, 0);
            if (got <= 0) return 0;
            d->pending_len = got;
            d->pending_pos = 0;
            return 1;
        }

        if (rc != AVERROR(EAGAIN)) return -1;

        /* Decoder wants more input. */
        for (;;) {
            int r = av_read_frame(d->fmt, d->pkt);
            if (r < 0) {
                avcodec_send_packet(d->dec, NULL);   /* signal EOF, then drain */
                d->eof = 1;
                break;
            }
            if (d->pkt->stream_index != d->stream_index) {
                av_packet_unref(d->pkt);
                continue;                            /* skip other streams */
            }
            r = avcodec_send_packet(d->dec, d->pkt);
            av_packet_unref(d->pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) return -1;
            break;
        }
    }
}

int exec_decoder_read(ExecDecoder *d, int16_t *dst, int frames)
{
    int done = 0;
    if (!d || !dst || frames <= 0) return 0;

    while (done < frames) {
        int n = take_pending(d, dst + (size_t)done * OUT_CHANNELS, frames - done);
        if (n > 0) { done += n; continue; }
        {
            int rc = fill_pending(d);
            if (rc <= 0) break;              /* end of stream or error */
        }
    }
    return done;
}

int exec_decoder_seek(ExecDecoder *d, float seconds)
{
    int64_t ts;
    if (!d || seconds <= 0.0f) return exec_decoder_rewind(d);
    /* play()'s first float is seconds and reached MediaPlayer.seekTo(ms) on
     * Android. Timestamps here are in the stream's own time base. */
    ts = (int64_t)(seconds * (float)AV_TIME_BASE);
    ts = av_rescale_q(ts, AV_TIME_BASE_Q,
                      d->fmt->streams[d->stream_index]->time_base);
    if (av_seek_frame(d->fmt, d->stream_index, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    avcodec_flush_buffers(d->dec);
    d->pending_len = d->pending_pos = 0;
    d->eof = 0;
    return 1;
}

int exec_decoder_rewind(ExecDecoder *d)
{
    if (!d) return 0;
    /* Seek to the very start of the stream, not to timestamp 0 of the file:
     * some MP4s carry an edit list and the first audio sample is not at 0. */
    if (av_seek_frame(d->fmt, d->stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    avcodec_flush_buffers(d->dec);
    d->pending_len = d->pending_pos = 0;
    d->eof = 0;
    return 1;
}
