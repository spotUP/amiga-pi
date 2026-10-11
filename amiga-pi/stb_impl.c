/* stb_impl -- stb_image and stb_image_write (vendor/stb, public domain)
 * compiled once, in their own file: the vendored code builds with its own
 * warnings relaxed (Makefile), amiga-pi's code (image.c) stays under the
 * strict flags. Only the formats pi's read takes: PNG, JPEG, BMP, GIF in;
 * PNG out. Both are static here (STB_*_STATIC), so the compiler drops what
 * is not reached from the three functions below -- among it stb's JPEG and
 * HDR writers, whose floating point would need mathieeesingbas.library on
 * a 68020 without an FPU. */
#include <stdlib.h>
#include <string.h>
#include "image.h"
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STBIW_NO_JPEG
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

unsigned char *pi_stb_load(const unsigned char *b, int n, int *w, int *h)
{
    int comp;
    return stbi_load_from_memory(b, n, w, h, &comp, 4);
}

void pi_stb_free(void *p)
{
    stbi_image_free(p);
}

unsigned char *pi_stb_png(const unsigned char *px, int w, int h, int *n)
{
    return stbi_write_png_to_mem(px, w * 4, w, h, 4, n);
}
