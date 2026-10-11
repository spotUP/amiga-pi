/* see image.h */
#include <stdlib.h>
#include <string.h>
#include "image.h"
#include "lanczos_tab.h"

/* ---- the type (mime.ts) ---- */

static unsigned long be32(const unsigned char *b)
{
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) | ((unsigned long)b[2] << 8) | b[3];
}

static unsigned long le32(const unsigned char *b)
{
    return b[0] | ((unsigned long)b[1] << 8) | ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
}

static unsigned le16(const unsigned char *b)
{
    return b[0] | ((unsigned)b[1] << 8);
}

static int animated_png(const unsigned char *b, long n)
{
    long off = 8;
    while (off + 8 <= n) {
        unsigned long len = be32(b + off);
        long next;
        if (!memcmp(b + off + 4, "acTL", 4))
            return 1;
        if (!memcmp(b + off + 4, "IDAT", 4))
            return 0;
        if (len > 0x7fffffffUL - 12)
            return 0;
        next = off + 8 + (long)len + 4;
        if (next <= off || next > n)
            return 0;
        off = next;
    }
    return 0;
}

static int is_bmp(const unsigned char *b, long n)
{
    unsigned long size, data, dib;
    unsigned planes, bpp;
    if (n < 26)
        return 0;
    size = le32(b + 2);
    data = le32(b + 10);
    dib = le32(b + 14);
    if (size && size < 26)
        return 0;
    if (data < 14 + dib)
        return 0;
    if (size && data >= size)
        return 0;
    if (dib == 12) {
        planes = le16(b + 22);
        bpp = le16(b + 24);
    } else if (dib >= 40 && dib <= 124) {
        if (n < 30)
            return 0;
        planes = le16(b + 26);
        bpp = le16(b + 28);
    } else
        return 0;
    return planes == 1 && (bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

const char *pi_image_type(const unsigned char *b, long n)
{
    static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (n > 4100)
        n = 4100;               /* pi sniffs the first 4100 bytes */
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
        return n > 3 && b[3] == 0xf7 ? 0 : "image/jpeg";
    if (n >= 8 && !memcmp(b, png, 8))
        return n >= 16 && be32(b + 8) == 13 && !memcmp(b + 12, "IHDR", 4) && !animated_png(b, n) ? "image/png" : 0;
    if (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6)))
        return "image/gif";
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4))
        return "image/webp";
    if (n >= 2 && b[0] == 'B' && b[1] == 'M' && is_bmp(b, n))
        return "image/bmp";
    return 0;
}

/* ---- EXIF orientation (exif-orientation.ts) ---- */

static int tiff_orientation(const unsigned char *b, long n, long t)
{
    int le;
    unsigned long ifd;
    long at, i, cnt;
    if (t + 8 > n)
        return 1;
    le = b[t] == 0x49 && b[t + 1] == 0x49;
    ifd = le ? le32(b + t + 4) : be32(b + t + 4);
    if (ifd > (unsigned long)n)
        return 1;
    at = t + (long)ifd;
    if (at + 2 > n)
        return 1;
    cnt = le ? (long)le16(b + at) : (long)((b[at] << 8) | b[at + 1]);
    for (i = 0; i < cnt; i++) {
        long e = at + 2 + i * 12;
        unsigned tag, v;
        if (e + 12 > n)
            return 1;
        tag = le ? le16(b + e) : (unsigned)((b[e] << 8) | b[e + 1]);
        if (tag == 0x0112) {
            v = le ? le16(b + e + 8) : (unsigned)((b[e + 8] << 8) | b[e + 9]);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
    return 1;
}

int pi_image_orientation(const unsigned char *b, long n)
{
    long off = 2;
    if (n < 2 || b[0] != 0xff || b[1] != 0xd8)
        return 1;
    while (off < n - 1) {
        int m;
        if (b[off] != 0xff)
            return 1;
        m = b[off + 1];
        if (m == 0xff) {
            off++;
            continue;
        }
        if (m == 0xe1) {
            long s = off + 4;
            if (off + 4 >= n || s + 6 > n)
                return 1;
            if (!memcmp(b + s, "Exif\0\0", 6))
                return tiff_orientation(b, n, s + 6);
        }
        if (off + 4 > n)
            return 1;
        off += 2 + ((b[off + 2] << 8) | b[off + 3]);
    }
    return 1;
}

/* the pixels turned as orientation o says: a new buffer (w, h swapped for
 * 5..8) or src itself; 0 out of memory */
static unsigned char *orient(unsigned char *src, long *w, long *h, int o)
{
    long x, y, W = *w, H = *h;
    unsigned char *d;
    if (o < 2 || o > 8)
        return src;
    d = (unsigned char *)malloc((size_t)(W * H * 4));
    if (!d)
        return 0;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            long di;
            switch (o) {
            case 2: di = y * W + (W - 1 - x); break;                    /* fliph */
            case 3: di = (H - 1 - y) * W + (W - 1 - x); break;          /* fliph + flipv */
            case 4: di = (H - 1 - y) * W + x; break;                    /* flipv */
            case 5: di = x * H + y; break;                              /* rotate90 cw, fliph */
            case 6: di = x * H + (H - 1 - y); break;                    /* rotate90 cw */
            case 7: di = (W - 1 - x) * H + (H - 1 - y); break;          /* rotate90 ccw, fliph */
            default: di = (W - 1 - x) * H + y; break;                   /* 8: rotate90 ccw */
            }
            memcpy(d + di * 4, src + (y * W + x) * 4, 4);
        }
    if (o >= 5) {
        *w = H;
        *h = W;
    }
    return d;
}

/* ---- Lanczos3, integer only ---- */

typedef struct lz_taps {
    long *start;
    int *cnt;
    short *w;
    int max;
} lz_taps;

/* the kernel at |d| / fs (d and fs in 1/256 pixels), times 16384 */
static long lz_at(long d, long fs)
{
    long q, r, a, b;
    if (d < 0)
        d = -d;
    q = d * 64 / fs;
    if (q >= 192)
        return 0;
    r = (d * 64) % fs;
    a = lz_tab[q];
    b = lz_tab[q + 1];
    return a + (b - a) * r / fs;
}

static int lz_plan(lz_taps *t, long sn, long dn)
{
    long fs = sn * 256 / dn, i;
    if (fs < 256)
        fs = 256;               /* enlarging: the kernel at its own size */
    t->max = (int)(2 * ((3 * fs + 255) / 256) + 2);
    t->start = (long *)malloc((size_t)dn * sizeof(long));
    t->cnt = (int *)malloc((size_t)dn * sizeof(int));
    t->w = (short *)malloc((size_t)dn * (size_t)t->max * sizeof(short));
    if (!t->start || !t->cnt || !t->w)
        return -1;
    for (i = 0; i < dn; i++) {
        long num = (2 * i + 1) * sn, den = 2 * dn;
        long c8 = (num / den) * 256 + (num % den) * 256 / den;     /* the center, 1/256 px */
        long sup = 3 * fs, lo = (c8 - sup) / 256 - 1, hi = (c8 + sup) / 256 + 1, j, sum = 0, k;
        long wt[512];
        int n = 0;
        if (lo < 0)
            lo = 0;
        if (hi > sn - 1)
            hi = sn - 1;
        for (j = lo; j <= hi && n < t->max && n < 512; j++) {
            wt[n] = lz_at(j * 256 + 128 - c8, fs);
            sum += wt[n++];
        }
        t->start[i] = lo;
        t->cnt[i] = n;
        for (k = 0; k < n; k++)
            t->w[i * t->max + k] = (short)(sum ? wt[k] * 16384 / sum : (k == 0 ? 16384 : 0));
    }
    return 0;
}

static void lz_free(lz_taps *t)
{
    free(t->start);
    free(t->cnt);
    free(t->w);
}

unsigned char *pi_image_scale(const unsigned char *src, long sw, long sh, long dw, long dh)
{
    lz_taps hx, vy;
    unsigned char *mid = 0, *dst = 0;
    long x, y;
    int c, k;
    memset(&hx, 0, sizeof(hx));
    memset(&vy, 0, sizeof(vy));
    if (lz_plan(&hx, sw, dw) || lz_plan(&vy, sh, dh))
        goto done;
    mid = (unsigned char *)malloc((size_t)(dw * sh * 4));
    dst = (unsigned char *)malloc((size_t)(dw * dh * 4));
    if (!mid || !dst) {
        free(dst);
        dst = 0;
        goto done;
    }
    for (y = 0; y < sh; y++)
        for (x = 0; x < dw; x++)
            for (c = 0; c < 4; c++) {
                long acc = 8192;
                const unsigned char *s = src + (y * sw + hx.start[x]) * 4 + c;
                const short *w = hx.w + x * hx.max;
                for (k = 0; k < hx.cnt[x]; k++)
                    acc += (long)w[k] * s[k * 4];
                acc >>= 14;
                mid[(y * dw + x) * 4 + c] = (unsigned char)(acc < 0 ? 0 : acc > 255 ? 255 : acc);
            }
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
            for (c = 0; c < 4; c++) {
                long acc = 8192;
                const unsigned char *s = mid + (vy.start[y] * dw + x) * 4 + c;
                const short *w = vy.w + y * vy.max;
                for (k = 0; k < vy.cnt[y]; k++)
                    acc += (long)w[k] * s[k * dw * 4];
                acc >>= 14;
                dst[(y * dw + x) * 4 + c] = (unsigned char)(acc < 0 ? 0 : acc > 255 ? 255 : acc);
            }
done:
    free(mid);
    lz_free(&hx);
    lz_free(&vy);
    return dst;
}

/* ---- processImage ---- */

/* a WebP's size from its header (VP8, VP8L, VP8X): 0, -1 not readable */
static int webp_size(const unsigned char *b, long n, long *w, long *h)
{
    if (n < 30)
        return -1;
    if (!memcmp(b + 12, "VP8 ", 4) && b[23] == 0x9d && b[24] == 0x01 && b[25] == 0x2a) {
        *w = le16(b + 26) & 0x3fff;
        *h = le16(b + 28) & 0x3fff;
        return 0;
    }
    if (!memcmp(b + 12, "VP8L", 4) && b[20] == 0x2f) {
        unsigned long v = le32(b + 21);
        *w = (long)(v & 0x3fff) + 1;
        *h = (long)((v >> 14) & 0x3fff) + 1;
        return 0;
    }
    if (!memcmp(b + 12, "VP8X", 4)) {
        *w = (long)(b[24] | (b[25] << 8) | ((unsigned long)b[26] << 16)) + 1;
        *h = (long)(b[27] | (b[28] << 8) | ((unsigned long)b[29] << 16)) + 1;
        return 0;
    }
    return -1;
}

static long b64_size(long n)
{
    return (n + 2) / 3 * 4;
}

static void note_add(pi_img *r, const char *s)
{
    long k = (long)strlen(r->note);
    if (k && k < (long)sizeof(r->note) - 1)
        r->note[k++] = '\n';
    r->note[k] = 0;
    strncat(r->note, s, sizeof(r->note) - 1 - (size_t)k);
}

static void put_num(char *o, long v)
{
    char t[16];
    int k = 0;
    if (v < 0) {
        *o++ = '-';
        v = -v;
    }
    do
        t[k++] = (char)('0' + v % 10);
    while ((v /= 10) > 0);
    while (k)
        *o++ = t[--k];
    *o = 0;
}

/* stb's PNG writer's buffer (STBIW_MALLOC: malloc) as ours */
static unsigned char *png_of(const unsigned char *px, long w, long h, long *n)
{
    int len = 0;
    unsigned char *p = pi_stb_png(px, (int)w, (int)h, &len);
    *n = len;
    return p;
}

static int omit(pi_img *r, const char *why)
{
    free(r->data);
    r->data = 0;
    r->n = 0;
    r->note[0] = 0;
    note_add(r, why);
    return 1;
}

int pi_image_process(const unsigned char *b, long n, const char *mime, pi_img *r)
{
    const unsigned char *in = b;
    unsigned char *conv = 0, *px = 0, *op;
    long inn = n, w, h, tw, th;
    int ok = 0, o;
    const char *from = 0;
    memset(r, 0, sizeof(*r));
    r->mime = mime;
    if (!strcmp(mime, "image/bmp")) {
        /* normalizeImage: what is not PNG, JPEG, GIF or WebP becomes PNG */
        int iw, ih;
        unsigned char *bp = pi_stb_load(b, (int)n, &iw, &ih);
        if (bp)
            conv = png_of(bp, iw, ih, &inn);
        pi_stb_free(bp);
        if (!conv)
            return omit(r, "[Image omitted: could not be converted to a supported inline image format.]");
        in = conv;
        from = mime;
        r->mime = "image/png";
    }
    /* resizeImage: the size after the orientation */
    if (!strcmp(r->mime, "image/webp")) {
        if (webp_size(in, inn, &w, &h))
            goto fail;
        r->ow = w;
        r->oh = h;
        if (w <= PI_IMG_MAX_W && h <= PI_IMG_MAX_H && b64_size(inn) < PI_IMG_MAX_B64)
            goto as_is;
        goto fail;              /* no WebP decoder: it cannot be scaled */
    } else {
        int iw, ih;
        px = pi_stb_load(in, (int)inn, &iw, &ih);
        if (!px)
            goto fail;
        w = iw;
        h = ih;
        o = !strcmp(r->mime, "image/jpeg") ? pi_image_orientation(in, inn) : 1;
        op = orient(px, &w, &h, o);
        if (!op)
            goto fail;
        if (op != px) {
            pi_stb_free(px);
            px = op;
        }
    }
    r->ow = w;
    r->oh = h;
    if (w <= PI_IMG_MAX_W && h <= PI_IMG_MAX_H && b64_size(inn) < PI_IMG_MAX_B64)
        goto as_is;
    tw = w;
    th = h;
    if (tw > PI_IMG_MAX_W) {
        th = (th * PI_IMG_MAX_W + tw / 2) / tw;
        tw = PI_IMG_MAX_W;
    }
    if (th > PI_IMG_MAX_H) {
        tw = (tw * PI_IMG_MAX_H + th / 2) / th;
        th = PI_IMG_MAX_H;
    }
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    for (;;) {
        unsigned char *s = pi_image_scale(px, w, h, tw, th), *p;
        long pn = 0, nw, nh;
        if (!s)
            goto fail;
        p = png_of(s, tw, th, &pn);
        free(s);
        if (p && b64_size(pn) < PI_IMG_MAX_B64) {
            char line[200], num[16];
            long sc;
            r->data = p;
            r->n = pn;
            r->mime = "image/png";
            r->w = tw;
            r->h = th;
            if (from) {
                strcpy(line, "[Image converted from ");
                strcat(line, from);
                strcat(line, " to image/png.]");
                note_add(r, line);
            }
            /* formatDimensionNote: originalWidth / width to 2 places */
            sc = (w * 200 + tw) / (2 * tw);
            strcpy(line, "[Image: original ");
            put_num(num, w);
            strcat(line, num);
            strcat(line, "x");
            put_num(num, h);
            strcat(line, num);
            strcat(line, ", displayed at ");
            put_num(num, tw);
            strcat(line, num);
            strcat(line, "x");
            put_num(num, th);
            strcat(line, num);
            strcat(line, ". Multiply coordinates by ");
            put_num(num, sc / 100);
            strcat(line, num);
            strcat(line, ".");
            num[0] = (char)('0' + sc % 100 / 10);
            num[1] = (char)('0' + sc % 10);
            num[2] = 0;
            strcat(line, num);
            strcat(line, " to map to original image.]");
            note_add(r, line);
            ok = 1;
            break;
        }
        free(p);
        if (tw == 1 && th == 1)
            break;
        nw = tw == 1 ? 1 : (tw * 3 / 4 > 1 ? tw * 3 / 4 : 1);
        nh = th == 1 ? 1 : (th * 3 / 4 > 1 ? th * 3 / 4 : 1);
        if (nw == tw && nh == th)
            break;
        tw = nw;
        th = nh;
    }
    pi_stb_free(px);
    free(conv);
    if (!ok)
        return omit(r, "[Image omitted: could not be resized below the inline image size limit.]");
    return 0;
as_is:
    /* within every limit: the bytes as they are (a BMP's PNG) */
    pi_stb_free(px);
    r->data = (unsigned char *)malloc((size_t)inn + 1);
    if (!r->data) {
        free(conv);
        return omit(r, "[Image omitted: could not be resized below the inline image size limit.]");
    }
    memcpy(r->data, in, (size_t)inn);
    r->n = inn;
    r->w = r->ow;
    r->h = r->oh;
    free(conv);
    if (from) {
        char line[120];
        strcpy(line, "[Image converted from ");
        strcat(line, from);
        strcat(line, " to image/png.]");
        note_add(r, line);
    }
    return 0;
fail:
    pi_stb_free(px);
    free(conv);
    return omit(r, "[Image omitted: could not be resized below the inline image size limit.]");
}

void pi_image_free(pi_img *r)
{
    free(r->data);
    r->data = 0;
}
