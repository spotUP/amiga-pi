/* The YAML reading of skills and prompt templates (I8, J5): pi_fm_parse (utils/frontmatter.ts parseFrontmatter over the yaml
 * package) against what pi 1.1.0's own code gives for 1210 files (tests/pi/yaml/cases.json, tools/pi_oracle/gen_yaml.mjs): the
 * error message word for word (position and context lines included) or what name, description, disable-model-invocation and
 * argument-hint are, and the body. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/json.h"
#include "../amiga-pi/frontmatter.h"

static char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)len + 1);
    len = b ? (long)fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    if (b)
        b[len] = 0;
    *n = len;
    return b;
}

static const char *const keys[4] = { "name", "description", "disable-model-invocation", "argument-hint" };

/* what the generator wrote for a value: "n" absent, "t" true, "o" other, "s:<text>" a string */
static void show(const pi_yv *v, char *out, long cap)
{
    if (v->kind == YV_NONE)
        cl_copy(out, "n", cap);
    else if (v->kind == YV_TRUE)
        cl_copy(out, "t", cap);
    else if (v->kind == YV_OTHER)
        cl_copy(out, "o", cap);
    else {
        cl_copy(out, "s:", cap);
        cl_cat(out, v->s ? v->s : "", cap);
    }
}

static void test_cases(void)
{
    char *b;
    long bn, shown = 0, bad = 0, nerr = 0, nok = 0;
    jv all, rec, x, kv;
    jit it, ki;
    b = slurp(PI_TESTS "/pi/yaml/cases.json", &bn);
    CHECK(b != 0);
    if (!b || json_parse(b, bn, &all)) {
        CHECK(0);
        free(b);
        return;
    }
    json_iter(all, &it);
    while (json_next(&it, 0, &rec)) {
        long cn, en = 0, i, bodylen = 0;
        char *content = 0, *want_err = 0, *body = 0, err[2000];
        pi_yv v[4];
        int rc, ok = 1;
        if (json_get(rec, "c", &x))
            content = json_strdup(x, &cn);
        if (!content)
            continue;
        if (json_get(rec, "e", &x))
            want_err = json_strdup(x, &en);
        rc = pi_fm_parse(content, cn, keys, 4, v, &body, &bodylen, err, sizeof(err));
        if (want_err) {
            nerr++;
            ok = rc == -2 && !strcmp(err, want_err);
            if (!ok && shown++ < 8)
                printf("  yaml: want error [%s]\n  got rc %d [%s]\n  for [%s]\n", want_err, rc, rc == -2 ? err : "", content);
        } else {
            char got[1200], wantk[1200], *wb = 0;
            long wbn = 0;
            nok++;
            ok = rc == 0;
            if (json_get(rec, "k", &kv)) {
                json_iter(kv, &ki);
                for (i = 0; ok && i < 4 && json_next(&ki, 0, &x); i++) {
                    json_str(x, wantk, sizeof(wantk));
                    show(&v[i], got, sizeof(got));
                    if (strcmp(got, wantk)) {
                        ok = 0;
                        if (shown++ < 8)
                            printf("  yaml: key %s want [%s] got [%s]\n  for [%s]\n", keys[i], wantk, got, content);
                    }
                }
            }
            if (ok && json_get(rec, "b", &x)) {
                wb = json_strdup(x, &wbn);
                if (!wb || !body || wbn != bodylen || memcmp(wb, body, (size_t)wbn)) {
                    ok = 0;
                    if (shown++ < 8)
                        printf("  yaml: body want [%s] got [%s]\n", wb ? wb : "", body ? body : "");
                }
            }
            free(wb);
        }
        if (!ok)
            bad++;
        pi_yv_free(v, 4);
        free(body);
        free(want_err);
        free(content);
    }
    CHECK_INT(bad, 0);
    CHECK(nerr > 500 && nok > 300);
    free(b);
}

void suite_pi_yaml(void)
{
    test_cases();
}
