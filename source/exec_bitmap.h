/* exec_bitmap.h -- BitmapFactory.decodeByteArray plus the AndroidBitmap_* trio.
 *
 * The engine's texture path is: read the asset into memory, hand the bytes to
 * BitmapFactory.decodeByteArray, then AndroidBitmap_getInfo / lockPixels /
 * unlockPixels and glTexImage2D. So the decoder has to live here.
 *
 * A .png EXTENSION IS NOT EVIDENCE OF A PNG
 * -----------------------------------------
 * BitmapFactory sniffs the container and ignores the name, so an Android game
 * has no reason to keep the two in step -- and Pizza Vs. Skeletons did not:
 * all 1508 of its "*.png" assets were lossless WebP (RIFF/WEBP/VP8L), with
 * not one PNG signature among them.
 *
 * THIS GAME'S IMAGE TREE HAS NOT BEEN MEASURED. The decoder below sniffs the
 * container too and handles both, so the port is correct either way; if you
 * want to know what your copy holds, tools/prepare_game.sh runs `file` over
 * three of them and prints the answer.
 *
 * The engine's own .ci4 containers are decoded by the engine and never reach
 * this path at all -- create_texture_ci4 is separate.
 *
 * ALPHA MUST BE PREMULTIPLIED
 * ---------------------------
 * Not a preference -- read out of the engine. GLTexture::create_blend picks
 * the source blend factor from a global `premultAlpha` byte:
 *
 *     ldrb  w9, [x10]                  ; premultAlpha
 *     cmp   w9, #0
 *     mov   w9, #0x302                 ; GL_SRC_ALPHA
 *     csinc w22, w9, wzr, eq           ; flag ? GL_ONE : GL_SRC_ALPHA
 *
 * and the single site that writes that byte does `mov w9, #1; strb w9, [x8]`.
 * So the engine always blends with GL_ONE, which is only correct for
 * premultiplied textures -- which is what Android's BitmapFactory returns by
 * default. libwebp's MODE_rgbA premultiplies inside the decoder, so both
 * paths below hand back exactly what the engine was built against.
 *
 * The engine also checks the format and nothing else: create_bitmap does
 * `ldr w8,[sp,#0x24]; cmp w8,#1`, which is info.format ==
 * ANDROID_BITMAP_FORMAT_RGBA_8888. It never reads info.flags, so it does not
 * adapt -- it assumes the platform's convention.
 *
 * The engine's own .ima4 and .ci4 containers are decoded by the engine and
 * never reach BitmapFactory.
 */
#ifndef EXEC_BITMAP_H
#define EXEC_BITMAP_H
#include <stdint.h>

typedef struct { uint32_t w, h, stride, format; void *pixels; } exec_bitmap;

/* ANDROID_BITMAP_FORMAT_RGBA_8888 */
#define EXEC_BITMAP_RGBA_8888 1

exec_bitmap *exec_bitmap_decode(const uint8_t *data, int len);
void        exec_bitmap_free(exec_bitmap *b);

/* Resolver targets. The jobject handed in is a pool handle from exec_jni.c;
 * exec_bitmap_of() is how that becomes a exec_bitmap *. */
int  AndroidBitmap_getInfo(void *env, void *jbitmap, void *info);
int  AndroidBitmap_lockPixels(void *env, void *jbitmap, void **addr);
int  AndroidBitmap_unlockPixels(void *env, void *jbitmap);
#endif
