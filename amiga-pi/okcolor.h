/* okcolor -- the colour spellings pi's themes use beyond hex (plan N6; pi-tui colors.js and oklab.js, Bjorn
 * Ottosson's OKHSL and Oklab, MIT): "okhsl(295 50% 67%)" and "oklch(70% 0.1 150)", to sRGB the way pi converts them
 * (OKLCH out of the gamut: chroma reduced at the hue, 20 bisection steps). The one place amiga-pi uses floating
 * point for colour; theme.c takes it as its pitheme_ext hook. Portable C89 with math.h. */
#ifndef PI_OKCOLOR_H
#define PI_OKCOLOR_H

/* 1 and *r, *g, *b (0-255) when s is an okhsl() or oklch() colour, 0 when it is another spelling, -1 when it is one
 * with a bad value (a saturation or lightness outside 0..1, a negative chroma) */
int pi_okcolor(const char *s, int *r, int *g, int *b);

#endif
