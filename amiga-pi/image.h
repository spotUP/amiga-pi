/* image -- pi's read for images (pi f10993bc utils/mime.ts
 * detectSupportedImageMimeType, utils/image-process.ts processImage,
 * utils/image-resize-core.ts, utils/exif-orientation.ts): the type from
 * the file's first bytes; a BMP turned into PNG; an image over 2000x2000 or
 * over 4.5 MB of base64 scaled down (Lanczos3, EXIF orientation applied)
 * and written as PNG, smaller by 3/4 steps until it fits. Decoders: PNG,
 * JPEG, BMP, GIF (stb_image); encoder: PNG (stb_image_write). pi also
 * tries JPEG at falling qualities and decodes WebP; amiga-pi writes PNG
 * only and passes a WebP through when it fits (its size from its header),
 * else omits it.
 * Portable C89, host-tested (tests/test_pi_tools.c). */
#ifndef PI_IMAGE_H
#define PI_IMAGE_H

#define PI_IMG_MAX_W 2000
#define PI_IMG_MAX_H 2000
#define PI_IMG_MAX_B64 4718592L     /* pi: 4.5 MB of base64 */

typedef struct pi_img {
    unsigned char *data;        /* the image to send (malloc'ed), 0 when omitted */
    long n;
    const char *mime;
    char note[400];             /* pi's hints, '\n' between; or why it was omitted */
    long ow, oh, w, h;          /* original and sent size (w, h 0: not decoded) */
} pi_img;

/* base64 text (RFC 4648, padded or not, white space ignored) decoded: a malloc'ed buffer and its length, 0 when it is not base64 */
unsigned char *pi_b64_decode(const char *s, long n, long *out_n);
/* pi's detectSupportedImageMimeType: "image/png" ... "image/bmp", 0 none */
const char *pi_image_type(const unsigned char *b, long n);
/* settings.json images.autoResize (default 1): 0 sends an image as it is (a BMP still becomes a PNG), without the 2000x2000 limit */
void pi_image_set_resize(int on);
/* pi's processImage: 0 sent (r->data), 1 omitted (r->note says why) */
int pi_image_process(const unsigned char *b, long n, const char *mime, pi_img *r);
void pi_image_free(pi_img *r);
/* the EXIF orientation of a JPEG (1..8; 1 none) */
int pi_image_orientation(const unsigned char *b, long n);
/* stb (stb_impl.c): RGBA pixels of an image (malloc'ed, pi_stb_free), 0
 * not decoded; RGBA pixels as a PNG file (malloc'ed), 0 out of memory */
unsigned char *pi_stb_load(const unsigned char *b, int n, int *w, int *h);
void pi_stb_free(void *p);
unsigned char *pi_stb_png(const unsigned char *px, int w, int h, int *n);
/* Lanczos3 scaling of RGBA pixels: a malloc'ed dw x dh image, 0 out of memory */
unsigned char *pi_image_scale(const unsigned char *src, long sw, long sh, long dw, long dh);

#endif
