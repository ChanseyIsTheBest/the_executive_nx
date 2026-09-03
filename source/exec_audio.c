/* exec_audio.c -- the backend behind ExecutiveAudio. MIT licensed. See LICENSE.
 *
 * The contract is in exec_audio.h and was read out of classes.dex, not guessed.
 * This file is the part that has to be invented: Android had SoundPool and
 * MediaPlayer, and there is no equivalent here.
 *
 * THE SHAPE, AND WHY
 * ------------------
 * Two very different jobs sit behind one API.
 *
 * Sound effects (RIFF PCM s16 mono 44100 Hz, none large) are what
 * SoundPool did: decode once at registerSound, keep resident, play many
 * overlapping copies cheaply. They are decoded whole into memory here, which
 * is a few MB in total and makes playback a pointer walk.
 *
 * Music (AAC-LC in MP4) is what MediaPlayer did: one at a time,
 * minutes long, seekable. Decoding one whole would be ~20 MB and a visible
 * stall on every music change, so it streams.
 *
 * WHICH THREAD TOUCHES FILES
 * --------------------------
 * The mixer runs on an audren callback thread and must never block. Streaming
 * from the SD card on that thread would underrun on the first slow read.
 * So there are three parties:
 *
 *     game thread   registerSound/play/stop/...  -> takes g_lock
 *     decode thread reads + decodes music        -> fills a ring buffer
 *     mixer thread  drains SFX and the ring      -> memory only, never files
 *
 * The decode thread runs beside the engine's own file access, and devkitPro's
 * newlib keeps a process-wide handle table that is not thread-safe. Every file
 * call it makes therefore goes through exec_io.c's lock, the same one the
 * engine's own shims and nx_pointer use. This is not theoretical: the Osmos
 * port's fourth audit found exactly this bug -- an unlocked audio shim beside
 * an engine that initialises audio off-thread.
 *
 * MUSIC DECODES ON THE CONSOLE
 * ----------------------------
 * The tracks are AAC-LC in MP4 and libnx has no decoder for that, so music
 * goes through exec_decode.c, which is ffmpeg (switch-ffmpeg) behind a small
 * streaming interface. The assets are used exactly as they came out of the
 * APK: there is no conversion step and nothing for a player to do on a PC.
 *
 * Using libavformat rather than a bare AAC decoder is the point. A .m4a is
 * AAC inside MP4, so something has to parse the container, find the audio
 * stream and hand up whole frames, and doing that by hand is the bulk of the
 * work and all of the bugs. The same path also plays .ogg, .mp3 and .wav.
 *
 * Build with -DEXEC_NO_MUSIC (or set music_enabled 0 in config.txt) and the
 * whole path compiles out. Sound effects are unaffected either way.
 */

#include <stdlib.h>
#include <malloc.h>       /* memalign: audren mempools must be page-aligned */
#include <string.h>
#include <math.h>
#include <switch.h>

#include "exec_audio.h"
#include "exec_asset.h"
#include "exec_paths.h"
#include "exec_io.h"
#include "exec_log.h"
#include "config.h"

#ifndef EXEC_NO_MUSIC
#include "exec_decode.h"
#endif

#define OUT_RATE      48000
#define OUT_CHANNELS  2
#define FRAMES_PER_BUF 960          /* 20 ms at 48 kHz */
#define NUM_WAVEBUFS  4
#define MAX_ENTRIES   256
#define MAX_VOICES    16            /* ExecutiveAudio's MAX_STREAMS was 16 */

/* Music ring: ~1.5 s of stereo s16, enough to ride out an SD-card hiccup
 * without making a music change feel late. */
#define RING_FRAMES   (OUT_RATE * 3 / 2)

/* ---- entries ------------------------------------------------------------- */

typedef struct {
  int    used;
  int    music;
  int    looping;              /* Entry.looping, initialised to `music`      */
  float  volume;               /* Entry.volume, already clamped to [0,1]     */
  float  pan;                  /* Entry.pan, already clamped to [-1,1]       */
  char   asset[192];           /* post-androidMusicAssetName                 */
  int16_t *pcm;                /* sfx only: decoded s16 stereo @ OUT_RATE    */
  int      frames;
} entry;

typedef struct {
  int   active;
  int   handle;
  int   pos;                   /* frame cursor into entry.pcm                */
  float rate;                  /* SoundPool playback rate, [0.5, 2.0]        */
  float frac;                  /* resampling accumulator                     */
  float left, right;           /* gains, from stereoGains at play time       */
} voice;

static entry g_entries[MAX_ENTRIES];
static voice g_voices[MAX_VOICES];
static int   g_next_handle = 1;      /* ExecutiveAudio.nextHandle started at 1 */

/* RESIDENT SFX ACCOUNTING
 *
 * SoundPool decoded on load and kept the sample, so every registerSound of an
 * effect costs its decoded size for as long as the engine holds the handle.
 * That is the game's model and this port keeps it -- but the arithmetic is
 * worth watching rather than assuming, because it is decided by the assets
 * and not by anything here: a 44100 Hz mono file becomes 48000 Hz stereo s16,
 * which is 2.18x the bytes on disk.
 *
 * Pizza Vs. Skeletons registered 113 effects. The count for this game has not
 * been measured, and an applet-mode homebrew heap is small enough that
 * "silently ran out during level load" is a plausible failure. So the total
 * is tracked and crossing the soft limit says so once, with the number. It
 * does not refuse the allocation: the engine has no way to cope with a
 * registerSound that fails for a reason other than a missing file, and a
 * warning that names the real number is more useful than a game that goes
 * quiet at an arbitrary threshold. */
#define SFX_SOFT_LIMIT_BYTES (24u * 1024u * 1024u)
static size_t g_sfx_bytes;
static int    g_sfx_warned;

static Mutex g_lock;
static int   g_ready;

/* ---- the pan law, copied exactly ----------------------------------------- */

/* ExecutiveAudio.stereoGains:
 *     left  = pan > 0 ? (1 - pan) * vol : vol
 *     right = pan < 0 ? (1 + pan) * vol : vol
 * A linear one-sided attenuation, not a constant-power law. Replacing this
 * with a -3 dB pan law would be "better" and would not match the game. */
static void stereo_gains(float vol, float pan, float *l, float *r) {
  *l = (pan > 0.0f) ? (1.0f - pan) * vol : vol;
  *r = (pan < 0.0f) ? (1.0f + pan) * vol : vol;
}

static float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- WAV ----------------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Decode a RIFF PCM file to s16 stereo at OUT_RATE.
 *
 * This game's effects are 44100 Hz MONO s16, so both the resample and the
 * mono-to-stereo duplication below are live paths here, not dead ones. The
 * chunk walk is general anyway, because
 * because "all of them" was measured on one build of one game and a nearest-
 * neighbour upsample is four lines. Nearest-neighbour is audibly fine here:
 * these are short percussive effects, and SoundPool's own resampler was not
 * better. */
static int16_t *wav_decode(const uint8_t *d, size_t n, int *out_frames) {
  if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;

  uint16_t channels = 0, bits = 0, fmt = 0;
  uint32_t rate = 0;
  const uint8_t *data = NULL;
  size_t data_len = 0;

  size_t off = 12;
  while (off + 8 <= n) {
    const uint32_t sz = rd32(d + off + 4);
    const uint8_t *body = d + off + 8;
    if (body + sz > d + n) break;
    if (!memcmp(d + off, "fmt ", 4) && sz >= 16) {
      fmt = rd16(body); channels = rd16(body + 2);
      rate = rd32(body + 4); bits = rd16(body + 14);
    } else if (!memcmp(d + off, "data", 4)) {
      data = body; data_len = sz;
    }
    off += 8 + sz + (sz & 1);        /* chunks are word-aligned */
  }
  if (fmt != 1 || bits != 16 || !channels || !rate || !data) {
    exec_log(EXEC_LOG_WARN, "wav: unsupported (fmt=%u bits=%u ch=%u rate=%u)",
            fmt, bits, channels, (unsigned)rate);
    return NULL;
  }

  const int in_frames = (int)(data_len / (size_t)(2 * channels));
  const int frames = (rate == OUT_RATE)
                     ? in_frames
                     : (int)((int64_t)in_frames * OUT_RATE / rate);
  int16_t *pcm = malloc((size_t)frames * 2 * sizeof(int16_t));
  if (!pcm) return NULL;

  const int16_t *src = (const int16_t *)data;
  for (int i = 0; i < frames; i++) {
    const int j = (rate == OUT_RATE)
                  ? i : (int)((int64_t)i * rate / OUT_RATE);
    const int k = (j < in_frames ? j : in_frames - 1) * channels;
    pcm[i * 2 + 0] = src[k];
    pcm[i * 2 + 1] = (channels > 1) ? src[k + 1] : src[k];
  }
  *out_frames = frames;
  return pcm;
}

/* ---- music: decode thread + ring ----------------------------------------- */

/* Single producer (decode thread), single consumer (audren mixer thread).
 * `volatile` orders nothing and permits the compiler to reorder the sample
 * stores past the index publish, so the mixer can read a frame the producer
 * has not written yet. Release on the write index and acquire on the read of
 * it is what actually makes the handoff safe, and it costs nothing here. */
static int16_t g_ring[RING_FRAMES * 2];
static uint32_t g_ring_r, g_ring_w;          /* frame indices */

static uint32_t ring_load(const uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void ring_store(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static Thread   g_dec_thread;
static volatile int g_dec_run;
static volatile int g_dec_stop;              /* asks the decoder to unwind */
static Mutex    g_dec_lock;                  /* guards the request fields */
static char     g_dec_asset[192];
static float    g_dec_start;
static volatile int g_dec_loop;
static volatile int g_music_handle;          /* currentMusicHandle          */
static float    g_music_l = 1.0f, g_music_r = 1.0f;

static int ring_count(void) {
  const uint32_t w = ring_load(&g_ring_w), r = ring_load(&g_ring_r);
  return (int)((w - r + RING_FRAMES) % RING_FRAMES);
}
static int ring_space(void) { return RING_FRAMES - 1 - ring_count(); }

static void ring_reset(void) { ring_store(&g_ring_r, 0); ring_store(&g_ring_w, 0); }

static void ring_push(const int16_t *src, int frames) {
  uint32_t w = ring_load(&g_ring_w);
  for (int i = 0; i < frames; i++) {
    g_ring[w * 2 + 0] = src[i * 2 + 0];
    g_ring[w * 2 + 1] = src[i * 2 + 1];
    w = (w + 1) % RING_FRAMES;
  }
  ring_store(&g_ring_w, w);          /* publish the samples, then the index */
}

#ifndef EXEC_NO_MUSIC
/* One track at a time, streamed. exec_decode hands back interleaved stereo
 * S16 already at OUT_RATE, so there is nothing to resample or downmix here. */
static void decode_one(const char *asset, float start_sec, int loop) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", exec_assets_dir(), asset);

  ExecDecodeInfo info;
  ExecDecoder *d = exec_decoder_open(path, OUT_RATE, &info);
  if (!d) {
    exec_log(EXEC_LOG_WARN, "music: cannot open %s", path);
    return;
  }
  exec_log(EXEC_LOG_INFO, "music: %s (%s, %d Hz, %d ch, %d ms)",
          asset, info.codec_name ? info.codec_name : "?",
          info.src_rate, info.src_channels, info.duration_ms);

  if (start_sec > 0.0f) exec_decoder_seek(d, start_sec);

  static int16_t buf[2048 * OUT_CHANNELS];

  for (;;) {
    if (g_dec_stop) break;

    const int got = exec_decoder_read(d, buf, 2048);
    if (got <= 0) {
      if (!loop || g_dec_stop) break;
      if (!exec_decoder_rewind(d)) break;
      continue;
    }

    int written = 0;
    while (written < got && !g_dec_stop) {
      /* Block until the mixer has drained enough. 2 ms is well under the
       * 20 ms the mixer consumes per wavebuf, so this never spins hot. */
      const int room = ring_space();
      if (room <= 0) { svcSleepThread(2000000ULL); continue; }
      const int chunk = (got - written < room) ? (got - written) : room;
      ring_push(buf + written * OUT_CHANNELS, chunk);
      written += chunk;
    }
  }

  exec_decoder_close(d);
}

static void decoder_main(void *unused) {
  (void)unused;
  while (g_dec_run) {
    char asset[192]; float start; int loop;
    mutexLock(&g_dec_lock);
    memcpy(asset, g_dec_asset, sizeof(asset));
    start = g_dec_start; loop = g_dec_loop;
    g_dec_asset[0] = 0;
    mutexUnlock(&g_dec_lock);

    if (!asset[0]) { svcSleepThread(5000000ULL); continue; }

    g_dec_stop = 0;
    ring_reset();
    decode_one(asset, start, loop);
    /* Falling out means the track ended or was stopped. Either way the ring
     * drains and the mixer goes quiet on its own. */
  }
}
#endif /* !EXEC_NO_MUSIC */

/* ---- mixer --------------------------------------------------------------- */

static AudioDriver g_drv;
static AudioDriverWaveBuf g_wavebuf[NUM_WAVEBUFS];
static int16_t *g_mixbuf;
static Thread   g_mix_thread;
static volatile int g_mix_run;
static int      g_mempool;

static void mix_into(int16_t *dst, int frames) {
  static float acc[FRAMES_PER_BUF * 2];
  memset(acc, 0, sizeof(float) * (size_t)frames * 2);

  mutexLock(&g_lock);

  for (int v = 0; v < MAX_VOICES; v++) {
    voice *vo = &g_voices[v];
    if (!vo->active) continue;
    entry *e = &g_entries[vo->handle];
    if (!e->used || !e->pcm) { vo->active = 0; continue; }

    for (int i = 0; i < frames; i++) {
      if (vo->pos >= e->frames) { vo->active = 0; break; }
      const int16_t l = e->pcm[vo->pos * 2 + 0];
      const int16_t r = e->pcm[vo->pos * 2 + 1];
      acc[i * 2 + 0] += (float)l * vo->left;
      acc[i * 2 + 1] += (float)r * vo->right;
      /* SoundPool's rate is a resample ratio, so advance fractionally. */
      vo->frac += vo->rate;
      while (vo->frac >= 1.0f) { vo->frac -= 1.0f; vo->pos++; }
    }
  }

  const float ml = g_music_l;
  const float mr = g_music_r;
  {
    uint32_t r = ring_load(&g_ring_r);
    const uint32_t w = ring_load(&g_ring_w);
    for (int i = 0; i < frames && r != w; i++) {
      acc[i * 2 + 0] += (float)g_ring[r * 2 + 0] * ml;
      acc[i * 2 + 1] += (float)g_ring[r * 2 + 1] * mr;
      r = (r + 1) % RING_FRAMES;
    }
    ring_store(&g_ring_r, r);
  }

  mutexUnlock(&g_lock);

  /* Hard-clip rather than normalise. A limiter would duck the whole mix when
   * one effect peaks, which changes the balance the game was mixed for; the
   * assets are quiet enough that clipping is rare.
   *
   * No master gain: the engine already exposes volume in its own options
   * screen, and a second multiplier outside it only makes the two disagree. */
  for (int i = 0; i < frames * 2; i++) {
    float s = acc[i];
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    dst[i] = (int16_t)s;
  }
}

static void mixer_main(void *unused) {
  (void)unused;
  while (g_mix_run) {
    AudioDriverWaveBuf *wb = NULL;
    for (int i = 0; i < NUM_WAVEBUFS; i++)
      if (g_wavebuf[i].state == AudioDriverWaveBufState_Free ||
          g_wavebuf[i].state == AudioDriverWaveBufState_Done) { wb = &g_wavebuf[i]; break; }
    if (!wb) { audrvUpdate(&g_drv); audrenWaitFrame(); continue; }

    int16_t *dst = g_mixbuf + (wb - g_wavebuf) * FRAMES_PER_BUF * OUT_CHANNELS;
    mix_into(dst, FRAMES_PER_BUF);
    armDCacheFlush(dst, (size_t)FRAMES_PER_BUF * OUT_CHANNELS * sizeof(int16_t));

    audrvVoiceAddWaveBuf(&g_drv, 0, wb);
    audrvVoiceStart(&g_drv, 0);
    audrvUpdate(&g_drv);
    audrenWaitFrame();
  }
}

/* ---- lifecycle ----------------------------------------------------------- */

static const AudioRendererConfig g_arcfg = {
  .output_rate     = AudioRendererOutputRate_48kHz,
  .num_voices      = 4,
  .num_effects     = 0,
  .num_sinks       = 1,
  .num_mix_objs    = 1,
  .num_mix_buffers = 2,
};

int exec_audio_init(void) {
  mutexInit(&g_lock);
  mutexInit(&g_dec_lock);

  if (R_FAILED(audrenInitialize(&g_arcfg))) {
    exec_log(EXEC_LOG_ERROR, "audrenInitialize failed");
    return -1;
  }
  if (R_FAILED(audrvCreate(&g_drv, &g_arcfg, OUT_CHANNELS))) {
    audrenExit();
    return -1;
  }

  const size_t bufsz = (size_t)FRAMES_PER_BUF * OUT_CHANNELS *
                       sizeof(int16_t) * NUM_WAVEBUFS;
  const size_t poolsz = (bufsz + 0xFFF) & ~0xFFFUL;
  g_mixbuf = memalign(0x1000, poolsz);
  if (!g_mixbuf) { audrvClose(&g_drv); audrenExit(); return -1; }
  memset(g_mixbuf, 0, poolsz);

  g_mempool = audrvMemPoolAdd(&g_drv, g_mixbuf, poolsz);
  audrvMemPoolAttach(&g_drv, g_mempool);

  static const u8 sink_ch[] = { 0, 1 };
  audrvDeviceSinkAdd(&g_drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_ch);
  audrvUpdate(&g_drv);
  audrenStartAudioRenderer();

  audrvVoiceInit(&g_drv, 0, OUT_CHANNELS, PcmFormat_Int16, OUT_RATE);
  audrvVoiceSetDestinationMix(&g_drv, 0, AUDREN_FINAL_MIX_ID);
  audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 0, 0);
  audrvVoiceSetMixFactor(&g_drv, 0, 1.0f, 1, 1);
  audrvVoiceStart(&g_drv, 0);

  for (int i = 0; i < NUM_WAVEBUFS; i++) {
    g_wavebuf[i].data_raw          = g_mixbuf;
    g_wavebuf[i].size              = poolsz;
    g_wavebuf[i].start_sample_offset = i * FRAMES_PER_BUF;
    g_wavebuf[i].end_sample_offset   = (i + 1) * FRAMES_PER_BUF;
    g_wavebuf[i].state             = AudioDriverWaveBufState_Free;
  }

  g_mix_run = 1;
  /* Priority 0x2C is just above the main thread's 0x2E. Lower numbers are
   * HIGHER priority on Switch -- the Osmos port had this backwards once and
   * starved the thread it was trying to favour. */
  if (R_FAILED(threadCreate(&g_mix_thread, mixer_main, NULL, NULL, 0x8000, 0x2C, -2)) ||
      R_FAILED(threadStart(&g_mix_thread))) {
    exec_log(EXEC_LOG_ERROR, "audio: mixer thread failed to start");
    g_mix_run = 0;
    return -1;
  }

#ifndef EXEC_NO_MUSIC
  {
    g_dec_run = 1;
    if (R_FAILED(threadCreate(&g_dec_thread, decoder_main, NULL, NULL, 0x20000, 0x2D, -2)) ||
        R_FAILED(threadStart(&g_dec_thread))) {
      exec_log(EXEC_LOG_WARN, "audio: decoder thread failed; music disabled");
      g_dec_run = 0;
    }
  }
#endif

  g_ready = 1;
  exec_log(EXEC_LOG_INFO, "audio ready: %d Hz stereo, %d voices", OUT_RATE, MAX_VOICES);
  return 0;
}

void exec_audio_exit(void) {
  if (!g_ready) return;
  g_ready = 0;

#ifndef EXEC_NO_MUSIC
  if (g_dec_run) {
    g_dec_run = 0; g_dec_stop = 1;
    threadWaitForExit(&g_dec_thread);
    threadClose(&g_dec_thread);
  }
#endif
  g_mix_run = 0;
  threadWaitForExit(&g_mix_thread);
  threadClose(&g_mix_thread);

  audrvVoiceStop(&g_drv, 0);
  audrvUpdate(&g_drv);
  audrvClose(&g_drv);
  audrenExit();
  free(g_mixbuf);

  for (int i = 0; i < MAX_ENTRIES; i++) free(g_entries[i].pcm);
  g_sfx_bytes = 0; g_sfx_warned = 0;
}

void exec_audio_update(void) { /* the mixer owns its own thread */ }

void exec_audio_host_pause(void) {
  if (g_ready) audrvVoiceStop(&g_drv, 0);
}
void exec_audio_host_resume(void) {
  if (g_ready) audrvVoiceStart(&g_drv, 0);
}

/* ---- the seven methods --------------------------------------------------- */

/* ExecutiveAudio.androidMusicAssetName: a trailing ".ima4" becomes ".m4a" IF that
 * asset exists, else the name is left alone. The engine only ever asks for
 * .ima4, and the APK ships only .m4a, so in practice every music name is
 * rewritten -- but the existence check is what the Java did.
 *
 * ".ogg" is tried first only so that a player who converted their own music
 * is not overridden. Nobody has to: exec_decode plays the .m4a directly. */
static void music_asset_name(const char *in, char *out, size_t cap) {
  const size_t n = in ? strlen(in) : 0;
  if (n > 5 && !strcmp(in + n - 5, ".ima4")) {
    snprintf(out, cap, "%.*s.ogg", (int)(n - 5), in);
    if (exec_asset_exists(out)) return;
    snprintf(out, cap, "%.*s.m4a", (int)(n - 5), in);
    if (exec_asset_exists(out)) return;
  }
  snprintf(out, cap, "%s", in ? in : "");
}

int exec_audio_register(const char *name, int is_music) {
  if (!g_ready || !name) return 0;

  mutexLock(&g_lock);
  int h = 0;
  for (int i = g_next_handle; i < MAX_ENTRIES; i++)
    if (!g_entries[i].used) { h = i; break; }
  if (!h) { mutexUnlock(&g_lock); exec_log(EXEC_LOG_WARN, "audio: out of handles"); return 0; }

  entry *e = &g_entries[h];
  memset(e, 0, sizeof(*e));
  e->used    = 1;
  e->music   = is_music;
  e->looping = is_music;          /* Entry.looping = isMusic */
  e->volume  = 1.0f;
  e->pan     = 0.0f;

  if (is_music) music_asset_name(name, e->asset, sizeof(e->asset));
  else          snprintf(e->asset, sizeof(e->asset), "%s", name);

  g_next_handle = h + 1;
  mutexUnlock(&g_lock);

  if (!is_music) {
    /* SoundPool.load decoded up front and kept the sample resident. */
    void *data = NULL; size_t len = 0;
    if (!exec_asset_read_all(e->asset, &data, &len)) {
      exec_log(EXEC_LOG_WARN, "Could not load sfx %s", e->asset);
      mutexLock(&g_lock); e->used = 0; mutexUnlock(&g_lock);
      return 0;                    /* registerSound returns 0 on failure */
    }
    int frames = 0;
    int16_t *pcm = wav_decode(data, len, &frames);
    free(data);
    if (!pcm) {
      mutexLock(&g_lock); e->used = 0; mutexUnlock(&g_lock);
      return 0;
    }
    mutexLock(&g_lock);
    e->pcm = pcm; e->frames = frames;
    g_sfx_bytes += (size_t)frames * 2 * sizeof(int16_t);
    const size_t total = g_sfx_bytes;
    const int say = (!g_sfx_warned && total > SFX_SOFT_LIMIT_BYTES);
    if (say) g_sfx_warned = 1;
    mutexUnlock(&g_lock);
    if (say)
      exec_log(EXEC_LOG_WARN,
              "audio: resident sound effects have passed %u MB (now %u MB). "
              "If loading fails or the game runs out of memory later, this is "
              "the first thing to look at.",
              (unsigned)(SFX_SOFT_LIMIT_BYTES >> 20), (unsigned)(total >> 20));
  }

  return h;
}

void exec_audio_release(int h) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;
  exec_audio_stop(h);
  mutexLock(&g_lock);
  if (g_entries[h].pcm) {
    const size_t was = (size_t)g_entries[h].frames * 2 * sizeof(int16_t);
    g_sfx_bytes = (g_sfx_bytes > was) ? g_sfx_bytes - was : 0;
  }
  free(g_entries[h].pcm);
  memset(&g_entries[h], 0, sizeof(g_entries[h]));
  if (h < g_next_handle) g_next_handle = h;
  mutexUnlock(&g_lock);
}

void exec_audio_play(int h, float start, float volume, float pan, float rate) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;

  mutexLock(&g_lock);
  entry *e = &g_entries[h];
  if (!e->used) { mutexUnlock(&g_lock); return; }

  /* play() stores the clamped values on the entry before branching, and
   * setVolume/setPan later mutate the same fields. */
  e->volume = clampf(volume, 0.0f, 1.0f);
  e->pan    = clampf(pan,   -1.0f, 1.0f);

  if (e->music) {
    stereo_gains(e->volume, e->pan, &g_music_l, &g_music_r);
    g_music_handle = h;
    mutexUnlock(&g_lock);
#ifndef EXEC_NO_MUSIC
    if (g_dec_run) {
      g_dec_stop = 1;               /* unwind whatever is playing */
      mutexLock(&g_dec_lock);
      snprintf(g_dec_asset, sizeof(g_dec_asset), "%s", e->asset);
      g_dec_start = (start > 0.0f) ? start : 0.0f;
      g_dec_loop  = e->looping;     /* playMusic loops; playOneShotMusic does not */
      mutexUnlock(&g_dec_lock);
    }
#else
    (void)start;
#endif
    return;
  }

  /* SoundPool.play(id, left, right, priority, loop, rate) */
  voice *vo = NULL;
  for (int i = 0; i < MAX_VOICES; i++)
    if (!g_voices[i].active) { vo = &g_voices[i]; break; }
  if (!vo) {
    /* SoundPool dropped the lowest-priority stream; every effect here is
     * priority 1, so the oldest is as good a choice as the game ever got. */
    vo = &g_voices[0];
    for (int i = 1; i < MAX_VOICES; i++)
      if (g_voices[i].pos > vo->pos) vo = &g_voices[i];
  }
  vo->handle = h;
  vo->pos    = 0;
  vo->frac   = 0.0f;
  vo->rate   = clampf(rate, 0.5f, 2.0f);
  stereo_gains(e->volume, e->pan, &vo->left, &vo->right);
  vo->active = 1;
  mutexUnlock(&g_lock);
}

void exec_audio_stop(int h) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;
  mutexLock(&g_lock);
  entry *e = &g_entries[h];
  if (e->used && e->music) {
    if (g_music_handle == h) {
      g_music_handle = 0;
#ifndef EXEC_NO_MUSIC
      g_dec_stop = 1;
#endif
      /* Silence the residue rather than resetting the ring.
       *
       * ring_reset moves BOTH indices, and this runs on the game thread while
       * the decode thread may be mid-push and the mixer mid-drain. The
       * indices are always taken modulo RING_FRAMES so nothing goes out of
       * bounds, but the three-way write is still a way to hand the mixer
       * frames from the track that was just stopped. Zeroing the gain makes
       * whatever is left in the ring inaudible, and the decoder resets the
       * ring itself before the next track -- on the thread that owns the
       * write index, which is where that belongs. */
      g_music_l = g_music_r = 0.0f;
    }
  } else {
    for (int i = 0; i < MAX_VOICES; i++)
      if (g_voices[i].active && g_voices[i].handle == h) g_voices[i].active = 0;
  }
  mutexUnlock(&g_lock);
}

void exec_audio_set_repeats(int h, int repeats) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;
  mutexLock(&g_lock);
  if (g_entries[h].used) {
    g_entries[h].looping = repeats;
    if (g_entries[h].music && g_music_handle == h) g_dec_loop = repeats;
  }
  mutexUnlock(&g_lock);
}

void exec_audio_set_volume(int h, float v) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;
  mutexLock(&g_lock);
  entry *e = &g_entries[h];
  if (e->used) {
    e->volume = clampf(v, 0.0f, 1.0f);
    if (e->music && g_music_handle == h)
      stereo_gains(e->volume, e->pan, &g_music_l, &g_music_r);
    else
      for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i].active && g_voices[i].handle == h)
          stereo_gains(e->volume, e->pan, &g_voices[i].left, &g_voices[i].right);
  }
  mutexUnlock(&g_lock);
}

void exec_audio_set_pan(int h, float p) {
  if (!g_ready || h <= 0 || h >= MAX_ENTRIES) return;
  mutexLock(&g_lock);
  entry *e = &g_entries[h];
  if (e->used) {
    e->pan = clampf(p, -1.0f, 1.0f);
    if (e->music && g_music_handle == h)
      stereo_gains(e->volume, e->pan, &g_music_l, &g_music_r);
    else
      for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i].active && g_voices[i].handle == h)
          stereo_gains(e->volume, e->pan, &g_voices[i].left, &g_voices[i].right);
  }
  mutexUnlock(&g_lock);
}
