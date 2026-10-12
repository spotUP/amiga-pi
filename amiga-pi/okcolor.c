/* see okcolor.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "okcolor.h"

static const double LAB_TO_LMS[3][3] = {
    { 1, 0.3963377773761749, 0.2158037573099136 },
    { 1, -0.1055613458156586, -0.0638541728258133 },
    { 1, -0.0894841775298119, -1.2914855480194092 } };
static const double LMS_TO_LIN[3][3] = {
    { 4.0767416360759583, -3.3077115392580629, 0.2309699031821043 },
    { -1.2684379732850315, 2.6097573492876882, -0.341319376002657 },
    { -0.0041960761386756, -0.7034186179359362, 1.7076146940746117 } };
static const double FIT[3][2][5] = {
    { { -1.8817031, -0.80936501, 0, 0, 0 }, { 1.19086277, 1.76576728, 0.59662641, 0.75515197, 0.56771245 } },
    { { 1.8144408, -1.19445267, 0, 0, 0 }, { 0.73956515, -0.45954404, 0.08285427, 0.12541073, -0.14503204 } },
    { { 0.13110758, 1.81333971, 0, 0, 0 }, { 1.35733652, -0.00915799, -1.1513021, -0.50559606, 0.00692167 } } };

#define K1 0.206
#define K2 0.03
#define K3 ((1 + K1) / (1 + K2))

static double cube(double x)
{
    return x * x * x;
}

static void oklab_to_lin(double L, double a, double b, double *out)
{
    double lms[3];
    int i;
    for (i = 0; i < 3; i++)
        lms[i] = cube(LAB_TO_LMS[i][0] * L + LAB_TO_LMS[i][1] * a + LAB_TO_LMS[i][2] * b);
    for (i = 0; i < 3; i++)
        out[i] = LMS_TO_LIN[i][0] * lms[0] + LMS_TO_LIN[i][1] * lms[1] + LMS_TO_LIN[i][2] * lms[2];
}

static int to_srgb(double v)
{
    double e = v > 0.0031308 ? 1.055 * pow(v, 1 / 2.4) - 0.055 : 12.92 * v;
    if (e < 0)
        e = 0;
    if (e > 1)
        e = 1;
    return (int)floor(e * 255 + 0.5);
}

static void slopes(double a, double b, double *k)
{
    int i;
    for (i = 0; i < 3; i++)
        k[i] = LAB_TO_LMS[i][1] * a + LAB_TO_LMS[i][2] * b;
}

static double max_saturation(double a, double b)
{
    int ch = 2, i;
    double s, k[3], base[3], f = 0, f1 = 0, f2 = 0;
    for (i = 0; i < 2; i++)
        if (FIT[i][0][0] * a + FIT[i][0][1] * b > 1) {
            ch = i;
            break;
        }
    s = FIT[ch][1][0] + FIT[ch][1][1] * a + FIT[ch][1][2] * b + FIT[ch][1][3] * a * a + FIT[ch][1][4] * a * b;
    slopes(a, b, k);
    for (i = 0; i < 3; i++) {
        base[i] = 1 + s * k[i];
        f += LMS_TO_LIN[ch][i] * cube(base[i]);
        f1 += LMS_TO_LIN[ch][i] * 3 * k[i] * base[i] * base[i];
        f2 += LMS_TO_LIN[ch][i] * 6 * k[i] * k[i] * base[i];
    }
    return s - (f * f1) / (f1 * f1 - 0.5 * f * f2);
}

static void cusp(double a, double b, double *cl, double *cc)
{
    double s = max_saturation(a, b), lin[3], mx;
    oklab_to_lin(1, s * a, s * b, lin);
    mx = lin[0] > lin[1] ? lin[0] : lin[1];
    if (lin[2] > mx)
        mx = lin[2];
    *cl = cbrt(1 / mx);
    *cc = *cl * s;
}

static double max_chroma(double a, double b, double L, double cl, double cc)
{
    double t, k[3], lms[3], best = 1e300;
    int i;
    if (L <= cl)
        return cc * L / cl;
    t = cc * (L - 1) / (cl - 1);
    slopes(a, b, k);
    for (i = 0; i < 3; i++)
        lms[i] = L + t * k[i];
    for (i = 0; i < 3; i++) {
        double f = -1, f1 = 0, f2 = 0, u, step;
        int j;
        for (j = 0; j < 3; j++) {
            f += LMS_TO_LIN[i][j] * cube(lms[j]);
            f1 += LMS_TO_LIN[i][j] * 3 * k[j] * lms[j] * lms[j];
            f2 += LMS_TO_LIN[i][j] * 6 * k[j] * k[j] * lms[j];
        }
        u = f1 / (f1 * f1 - 0.5 * f * f2);
        step = u >= 0 ? -f * u : 1e300;
        if (step < best)
            best = step;
    }
    return t + best;
}

static void okhsl_rgb(double hue, double s, double l, int *r, int *g, int *b)
{
    double L = (l * l + K1 * l) / (K3 * (l + K2)), lab[3], lin[3];
    lab[0] = L;
    lab[1] = lab[2] = 0;
    if (L > 0 && L < 1 && s > 0) {
        double ang = 2 * 3.14159265358979323846 * fmod(fmod(hue, 360) + 360, 360) / 360, a = cos(ang), bb = sin(ang);
        double pl, pc, cmax, kk, mids, midt, cmid, c0, chroma, t, k1, m;
        cusp(a, bb, &pl, &pc);
        cmax = max_chroma(a, bb, L, pl, pc);
        m = L * (pc / pl);
        t = (1 - L) * (pc / (1 - pl));
        kk = cmax / (m < t ? m : t);
        mids = 0.11516993 + 1 / (7.4477897 + 4.1590124 * bb + a * (-2.19557347 + 1.75198401 * bb + a * (-2.13704948 - 10.02301043 * bb + a * (-4.24894561 + 5.38770819 * bb + 4.69891013 * a))));
        midt = 0.11239642 + 1 / (1.6132032 - 0.68124379 * bb + a * (0.40370612 + 0.90148123 * bb + a * (-0.27087943 + 0.6122399 * bb + a * (0.00299215 - 0.45399568 * bb - 0.14661872 * a))));
        cmid = 0.9 * kk * sqrt(sqrt(1 / (1 / pow(L * mids, 4) + 1 / pow((1 - L) * midt, 4))));
        c0 = sqrt(1 / (1 / pow(L * 0.4, 2) + 1 / pow((1 - L) * 0.8, 2)));
        if (s < 0.8) {
            t = 1.25 * s;
            k1 = 0.8 * c0;
            chroma = t * k1 / (1 - (1 - k1 / cmid) * t);
        } else {
            t = 5 * (s - 0.8);
            k1 = 0.2 * cmid * cmid * 1.25 * 1.25 / c0;
            chroma = cmid + t * k1 / (1 - (1 - k1 / (cmax - cmid)) * t);
        }
        lab[1] = chroma * a;
        lab[2] = chroma * bb;
    }
    oklab_to_lin(lab[0], lab[1], lab[2], lin);
    *r = to_srgb(lin[0]);
    *g = to_srgb(lin[1]);
    *b = to_srgb(lin[2]);
}

static int in_gamut(const double *lin)
{
    int i;
    for (i = 0; i < 3; i++)
        if (lin[i] < -1e-7 || lin[i] > 1 + 1e-7)
            return 0;
    return 1;
}

static void oklch_rgb(double l, double c, double h, int *r, int *g, int *b)
{
    double rad = fmod(fmod(h, 360) + 360, 360) * 3.14159265358979323846 / 180, co = cos(rad), si = sin(rad), lin[3], best[3];
    double low = 0, high = c;
    int i;
    oklab_to_lin(l, c * co, c * si, lin);
    if (!in_gamut(lin)) {
        oklab_to_lin(l, 0, 0, best);
        for (i = 0; i < 20; i++) {
            double ch = (low + high) / 2;
            oklab_to_lin(l, ch * co, ch * si, lin);
            if (in_gamut(lin)) {
                low = ch;
                memcpy(best, lin, sizeof(best));
            } else
                high = ch;
        }
        memcpy(lin, best, sizeof(lin));
    }
    *r = to_srgb(lin[0]);
    *g = to_srgb(lin[1]);
    *b = to_srgb(lin[2]);
}

/* a number at *p (optional sign, digits, a dot, an exponent): 1 and p moved */
static int number(const char **p, double *v)
{
    char *end;
    const char *s = *p;
    if (!((*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '+'))
        return 0;
    *v = strtod(s, &end);
    if (end == s)
        return 0;
    *p = end;
    return 1;
}

static void skip(const char **p)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
}

static int prefix(const char *s, const char *w)
{
    int i;
    for (i = 0; w[i]; i++)
        if ((s[i] | 0x20) != w[i])
            return 0;
    return 1;
}

int pi_okcolor(const char *str, int *r, int *g, int *b)
{
    const char *p = str;
    double a, c, d;
    int pa, pc, pd;
    skip(&p);
    if (prefix(p, "oklch(")) {
        p += 6;
        skip(&p);
        if (!number(&p, &a))
            return -1;
        pa = *p == '%';
        p += pa;
        skip(&p);
        if (!number(&p, &c))
            return -1;
        skip(&p);
        if (!number(&p, &d))
            return -1;
        if (prefix(p, "deg"))
            p += 3;
        skip(&p);
        if (*p != ')')
            return -1;
        if (pa)
            a /= 100;
        if (a < 0 || a > 1 || c < 0)
            return -1;
        oklch_rgb(a, c, d, r, g, b);
        return 1;
    }
    if (prefix(p, "okhsl(")) {
        p += 6;
        skip(&p);
        if (!number(&p, &a))
            return -1;
        if (prefix(p, "deg"))
            p += 3;
        skip(&p);
        if (!number(&p, &c))
            return -1;
        pc = *p == '%';
        p += pc;
        skip(&p);
        if (!number(&p, &d))
            return -1;
        pd = *p == '%';
        p += pd;
        skip(&p);
        if (*p != ')')
            return -1;
        if (pc)
            c /= 100;
        if (pd)
            d /= 100;
        if (c < 0 || c > 1 || d < 0 || d > 1)
            return -1;
        okhsl_rgb(a, c, d, r, g, b);
        return 1;
    }
    return 0;
}
