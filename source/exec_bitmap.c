/* exec_bitmap.c -- MIT licensed. See LICENSE. */
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <webp/decode.h>
#include "exec_bitmap.h"
#include "exec_jni.h"
#include "exec_log.h"

typedef struct { uint32_t w, h, stride, format, flags; } AndroidBitmapInfo;

typedef struct { const uint8_t *p; int len, off; } memsrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  memsrc *s = (memsrc *)png_get_io_ptr(png);
  if (s->off + (int)n > s->len) { png_error(png, "short read"); return; }
  memcpy(out, s->p + s->off, n);
  s->off += (int)n;
}

/* Premultiply in place. Only the libpng path needs this; libwebp does it
 * inside the decoder. Rounded rather than truncated, so a 50%-alpha white
 * pixel comes out 128 and not 127 -- truncation is a visible darkening on
 * large soft-edged sprites. */
static void premultiply(uint8_t *px, int w, int h, int stride) {
  for (int y = 0; y < h; y++) {
    uint8_t *r = px + (size_t)y * stride;
    for (int x = 0; x < w; x++) {
      uint8_t *q = r + x * 4;
      const unsigned a = q[3];
      if (a == 255) continue;
      q[0] = (uint8_t)((q[0] * a + 127) / 255);
      q[1] = (uint8_t)((q[1] * a + 127) / 255);
      q[2] = (uint8_t)((q[2] * a + 127) / 255);
    }
  }
}

/* WebP: what every one of this game's "*.png" assets actually is. */
static exec_bitmap *decode_webp(const uint8_t *data, int len) {
  WebPDecoderConfig cfg;
  if (!WebPInitDecoderConfig(&cfg)) return NULL;

  /* MODE_rgbA, not MODE_RGBA. The lowercase 'a' is libwebp's spelling for
   * premultiplied output, which is what the engine's GL_ONE blend needs. */
  cfg.output.colorspace = MODE_rgbA;
  if (WebPDecode(data, (size_t)len, &cfg) != VP8_STATUS_OK) {
    exec_log(EXEC_LOG_ERROR, "decode: WebP decode failed (%d bytes)", len);
    WebPFreeDecBuffer(&cfg.output);
    return NULL;
  }

  const int w = cfg.output.width, h = cfg.output.height;
  const int src_stride = cfg.output.u.RGBA.stride;
  exec_bitmap *bm = calloc(1, sizeof(*bm));
  if (bm) {
    bm->w = (uint32_t)w; bm->h = (uint32_t)h;
    bm->stride = (uint32_t)w * 4;
    bm->format = EXEC_BITMAP_RGBA_8888;
    bm->pixels = malloc((size_t)bm->stride * (size_t)h);
  }
  if (!bm || !bm->pixels) {
    free(bm);
    WebPFreeDecBuffer(&cfg.output);
    return NULL;
  }
  /* Copy row by row: libwebp's stride is not required to equal width*4, and
   * the engine is handed our own tightly packed stride. */
  for (int y = 0; y < h; y++)
    memcpy((uint8_t *)bm->pixels + (size_t)y * bm->stride,
           cfg.output.u.RGBA.rgba + (size_t)y * src_stride, (size_t)w * 4);
  WebPFreeDecBuffer(&cfg.output);
  return bm;
}

exec_bitmap *exec_bitmap_decode(const uint8_t *data, int len) {
  if (!data || len < 16) {
    exec_log(EXEC_LOG_ERROR, "decode: %d bytes is too short to be an image", len);
    return NULL;
  }

  /* Sniff the container, do not trust the extension -- the assets are named
   * .png and are WebP. */
  if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4))
    return decode_webp(data, len);

  if (png_sig_cmp((png_const_bytep)data, 0, 8)) {
    exec_log(EXEC_LOG_ERROR,
            "decode: not WebP and not PNG (%d bytes, magic %02x%02x%02x%02x)",
            len, data[0], data[1], data[2], data[3]);
    return NULL;
  }
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return NULL;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return NULL; }

  exec_bitmap *bm = NULL;
  png_bytep  *rows = NULL;
  if (setjmp(png_jmpbuf(png))) {
    if (bm) { free(bm->pixels); free(bm); }
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    return NULL;
  }

  memsrc src = { data, len, 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  png_uint_32 w = png_get_image_width(png, info);
  png_uint_32 h = png_get_image_height(png, info);
  int depth = png_get_bit_depth(png, info);
  int color = png_get_color_type(png, info);

  /* Normalise everything to RGBA8888; premultiplied below, to match what
   * BitmapFactory returned on Android and what the engine's blend expects. */
  if (color == PNG_COLOR_TYPE_PALETTE)          png_set_palette_to_rgb(png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))   png_set_tRNS_to_alpha(png);
  if (depth == 16)                               png_set_strip_16(png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  bm = calloc(1, sizeof(*bm));
  bm->w = w; bm->h = h; bm->stride = w * 4; bm->format = EXEC_BITMAP_RGBA_8888;
  bm->pixels = malloc((size_t)bm->stride * h);
  rows = malloc(sizeof(png_bytep) * h);
  for (png_uint_32 y = 0; y < h; y++)
    rows[y] = (png_bytep)bm->pixels + (size_t)y * bm->stride;
  png_read_image(png, rows);
  png_read_end(png, NULL);
  free(rows);
  png_destroy_read_struct(&png, &info, NULL);
  /* libpng gives straight alpha; the engine blends with GL_ONE. */
  premultiply(bm->pixels, (int)bm->w, (int)bm->h, (int)bm->stride);
  return bm;
}

void exec_bitmap_free(exec_bitmap *b) {
  if (!b) return;
  free(b->pixels);
  free(b);
}

int AndroidBitmap_getInfo(void *env, void *jbitmap, void *vinfo) {
  (void)env;
  exec_bitmap *b = jni_bitmap_of(jbitmap);
  AndroidBitmapInfo *i = vinfo;
  if (!b || !i) return -1;             /* ANDROID_BITMAP_RESULT_BAD_PARAMETER */
  i->w = b->w; i->h = b->h; i->stride = b->stride;
  i->format = b->format; i->flags = 0;
  return 0;
}

int AndroidBitmap_lockPixels(void *env, void *jbitmap, void **addr) {
  (void)env;
  exec_bitmap *b = jni_bitmap_of(jbitmap);
  if (!b || !addr) return -1;
  *addr = b->pixels;
  return 0;
}

int AndroidBitmap_unlockPixels(void *env, void *jbitmap) {
  (void)env; (void)jbitmap;
  return 0;
}
