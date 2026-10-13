/* oauth -- see oauth.h. pi references are to vendor/pi-src/packages/ai/src/auth/oauth/ (pi 1.1.0, abe508e1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oauth.h"
#include "sha256.h"
#include "claude/util.h"
#include "llm/creds.h"

/* ---- small things ---- */

/* err := the parts joined (up to ten; a 0 part and the ones after it are left out). No varargs: vbcc's va_start warns */
static void say(char *err, long cap, const char *a, const char *b, const char *c, const char *d, const char *e,
                const char *f, const char *g, const char *h, const char *i, const char *j)
{
    const char *parts[10];
    int k;
    parts[0] = a;
    parts[1] = b;
    parts[2] = c;
    parts[3] = d;
    parts[4] = e;
    parts[5] = f;
    parts[6] = g;
    parts[7] = h;
    parts[8] = i;
    parts[9] = j;
    err[0] = 0;
    for (k = 0; k < 10 && parts[k]; k++)
        cl_cat(err, parts[k], cap);
}

static const char *status_text(int st)
{
    switch (st) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

static void num(long v, char *out)
{
    cl_ltoa(v, out);
}

/* a body made printable for a message: cut at the first line break's worth, 300 bytes at most */
static void body_text(const jw *b, char *out, long cap)
{
    long n = b->n < cap - 1 ? b->n : cap - 1;
    if (n > 0)
        memcpy(out, b->p, (size_t)n);
    else
        n = 0;
    out[n] = 0;
}

typedef struct hres {
    int status;
    jw body;
} hres;

static int ok(const hres *r)
{
    return r->status >= 200 && r->status < 300;
}

static int exch(oa_io *io, const char *method, const char *url, const char *hdrs, const char *body, hres *r, char *err,
                long cap)
{
    jw_init(&r->body);
    r->status = 0;
    if (io->cancelled && io->cancelled(io->u)) {
        cl_copy(err, "Login cancelled", cap);
        return OA_CANCEL;
    }
    if (io->http(io->u, method, url, hdrs, body, body ? (long)strlen(body) : 0, &r->status, &r->body, err, cap)) {
        jw_free(&r->body);
        if (io->cancelled && io->cancelled(io->u)) {
            cl_copy(err, "Login cancelled", cap);
            return OA_CANCEL;
        }
        return OA_FAIL;
    }
    return OA_OK;
}

static void hfree(hres *r)
{
    jw_free(&r->body);
}

/* the answer's body as a JSON object: 1 with *o */
static int as_obj(const hres *r, jv *o)
{
    return r->body.p && json_parse(r->body.p, r->body.n, o) == 0 && json_type(*o) == J_OBJ;
}

/* a string member: malloc'ed copy, 0 when absent or not a string */
static char *jstr(jv o, const char *key)
{
    jv v;
    if (!json_get(o, key, &v) || json_type(v) != J_STR)
        return 0;
    return json_strdup(v, 0);
}

/* a string member that is not empty */
static char *jstr_ne(jv o, const char *key)
{
    char *s = jstr(o, key);
    if (s && !*s) {
        free(s);
        return 0;
    }
    return s;
}

/* a positive finite number member, 0 when absent / not a number / not positive */
static long jpos(jv o, const char *key)
{
    jv v;
    if (!json_get(o, key, &v) || json_type(v) != J_NUM)
        return 0;
    return json_long(v, 0) > 0 ? json_long(v, 0) : 0;
}

static int has_ws_only(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            return 0;
    return 1;
}

/* the form body the way URLSearchParams.toString() writes it: bytes outside [*-._0-9A-Za-z] as %XX, a space as + */
void oa_form_add(jw *w, const char *name, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    int pass;
    const char *s;
    if (w->n)
        jw_raw(w, "&", 1);
    for (pass = 0; pass < 2; pass++) {
        for (s = pass ? value : name; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '*' || c == '-' ||
                c == '.' || c == '_')
                jw_raw(w, (const char *)&c, 1);
            else if (c == ' ')
                jw_raw(w, "+", 1);
            else {
                char t[3];
                t[0] = '%';
                t[1] = hex[c >> 4];
                t[2] = hex[c & 15];
                jw_raw(w, t, 3);
            }
        }
        if (!pass)
            jw_raw(w, "=", 1);
    }
}

void oa_write_ms(jw *w, long sec, int ms)
{
    char d[24];
    cl_ltoa(sec, d);
    jw_rawz(w, d);
    d[0] = (char)('0' + ms / 100 % 10);
    d[1] = (char)('0' + ms / 10 % 10);
    d[2] = (char)('0' + ms % 10);
    d[3] = 0;
    jw_rawz(w, d);
}

/* the stored `expires` (epoch ms) as a double: 1 when readable */
static int expires_of(jv cred, double *out)
{
    jv v;
    char t[48];
    if (!json_get(cred, "expires", &v) || json_type(v) != J_NUM || v.n >= (long)sizeof(t))
        return 0;
    memcpy(t, v.p, (size_t)v.n);
    t[v.n] = 0;
    *out = strtod(t, 0);
    return 1;
}

int oa_expires_soon(jv cred, llm_ts now, long min_s)
{
    double e, t;
    if (!expires_of(cred, &e))
        return 0;
    t = ((double)now.sec + (double)min_s) * 1000.0 + (double)now.ms;
    return t >= e;
}

/* ---- base64url, PKCE ---- */

static void b64url(const unsigned char *p, long n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    long i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned long v = ((unsigned long)p[i] << 16) | ((unsigned long)p[i + 1] << 8) | p[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
        out[o++] = t[v & 63];
    }
    if (n - i == 1) {
        unsigned long v = (unsigned long)p[i] << 16;
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
    } else if (n - i == 2) {
        unsigned long v = ((unsigned long)p[i] << 16) | ((unsigned long)p[i + 1] << 8);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63];
    }
    out[o] = 0;
}

/* n random bytes as base64url (n <= 32; out holds 44) */
static int rand_b64(oa_io *io, int n, char *out)
{
    unsigned char b[32];
    if (!io->random || io->random(io->u, b, n)) {
        memset(b, 0, sizeof(b));
        return -1;
    }
    b64url(b, n, out);
    memset(b, 0, sizeof(b));
    return 0;
}

/* n random bytes as lower-case hex (n <= 16) */
static int rand_hex(oa_io *io, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    unsigned char b[16];
    int i;
    if (!io->random || io->random(io->u, b, n))
        return -1;
    for (i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
    memset(b, 0, sizeof(b));
    return 0;
}

/* pkce.ts generatePKCE: a 32 byte verifier (base64url) and its SHA-256 challenge */
static int pkce(oa_io *io, char *verifier, char *challenge)
{
    pi_sha256 c;
    unsigned char d[32];
    if (rand_b64(io, 32, verifier))
        return -1;
    pi_sha256_init(&c);
    pi_sha256_add(&c, verifier, (long)strlen(verifier));
    pi_sha256_end(&c, d);
    b64url(d, 32, challenge);
    return 0;
}

static const char *const no_random = "no secure random source on this system";

/* ---- URLs ---- */

typedef struct purl {
    char scheme[16], host[160], path[300];
    long port;                  /* -1: the scheme's default */
    const char *query;          /* after '?', up to '#' or the end */
    long qn;
} purl;

/* new URL(s) for an absolute http(s) URL: 1, 0 when it throws */
static int url_parse(const char *s, purl *u)
{
    const char *p = s, *q;
    long n = 0;
    memset(u, 0, sizeof(*u));
    u->port = -1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    for (q = p; *q && *q != ':' && n < 15; q++)
        u->scheme[n++] = (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
    if (*q != ':' || !n)
        return 0;
    p = q + 1;
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https"))
        return 0;
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    n = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && n < (long)sizeof(u->host) - 1)
        u->host[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p++ + 32 : *p++);
    u->host[n] = 0;
    if (!n)
        return 0;
    {
        /* a host name: letters, digits and . - _ (new URL() throws on a space, a bracket without its pair, ...) */
        long j;
        for (j = 0; j < n; j++) {
            char c = u->host[j];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
                return 0;
        }
    }
    if (*p == ':') {
        long port = 0;
        int d = 0;
        p++;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p++ - '0');
            d++;
        }
        if (d)
            u->port = port;
        if (u->port == (!strcmp(u->scheme, "https") ? 443 : 80))
            u->port = -1;
    }
    n = 0;
    while (*p && *p != '?' && *p != '#' && n < (long)sizeof(u->path) - 1)
        u->path[n++] = *p++;
    u->path[n] = 0;
    if (!u->path[0])
        cl_copy(u->path, "/", sizeof(u->path));
    if (*p == '?') {
        p++;
        u->query = p;
        while (*p && *p != '#')
            p++;
        u->qn = (long)(p - u->query);
    }
    return 1;
}

static int url_origin_is(const purl *u, const char *scheme, const char *host, long port)
{
    return !strcmp(u->scheme, scheme) && !strcmp(u->host, host) && u->port == port;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* searchParams.get(name) of a query string: 1 with the decoded value in out */
static int qget(const char *q, long qn, const char *name, char *out, long cap)
{
    long i = 0, nl = (long)strlen(name);
    while (i < qn) {
        long e = i, k = 0;
        while (e < qn && q[e] != '&')
            e++;
        if (e - i >= nl && !memcmp(q + i, name, (size_t)nl) && (e - i == nl || q[i + nl] == '=')) {
            long j = i + nl + (e - i > nl ? 1 : 0);
            while (j < e && k < cap - 1) {
                if (q[j] == '+') {
                    out[k++] = ' ';
                    j++;
                } else if (q[j] == '%' && j + 2 < e + 0 + 1 && hexv(q[j + 1]) >= 0 && j + 2 < qn + 0 + 1 &&
                           hexv(q[j + 2]) >= 0) {
                    out[k++] = (char)(hexv(q[j + 1]) * 16 + hexv(q[j + 2]));
                    j += 3;
                } else
                    out[k++] = q[j++];
            }
            out[k] = 0;
            return 1;
        }
        i = e + 1;
    }
    return 0;
}

/* ---- the device code poller (device-code.ts) ---- */

enum { DP_PENDING, DP_SLOW, DP_FAIL, DP_DONE };

typedef struct dpr {
    int st;
    long interval;              /* DP_SLOW: the server's interval in seconds, 0 none */
    char msg[400];              /* DP_FAIL */
} dpr;

typedef int (*poll_fn)(void *c, dpr *r);

static long elapsed_ms(oa_io *io, llm_ts t0)
{
    llm_ts t = io->now(io->u);
    return (t.sec - t0.sec) * 1000L + (long)(t.ms - t0.ms);
}

/* pollOAuthDeviceCodeFlow: 0 when poll answered DP_DONE, else OA_FAIL / OA_CANCEL with the message */
static int device_poll(oa_io *io, long interval_s, long expires_s, int wait_first, poll_fn f, void *c, char *err, long cap)
{
    llm_ts t0 = io->now(io->u);
    long deadline = expires_s > 0 ? expires_s * 1000L : -1;
    long interval_ms = (interval_s < 0 ? 5 : interval_s) * 1000L;
    int slow = 0;
    if (interval_ms < 1000)
        interval_ms = 1000;
    if (wait_first) {
        long rem = deadline < 0 ? interval_ms : deadline - elapsed_ms(io, t0);
        if (rem > 0 && io->sleep(io->u, rem < interval_ms ? rem : interval_ms)) {
            cl_copy(err, "Login cancelled", cap);
            return OA_CANCEL;
        }
    }
    while (deadline < 0 || elapsed_ms(io, t0) < deadline) {
        dpr r;
        long rem;
        if (io->cancelled && io->cancelled(io->u)) {
            cl_copy(err, "Login cancelled", cap);
            return OA_CANCEL;
        }
        memset(&r, 0, sizeof(r));
        r.interval = 0;
        {
            int rc = f(c, &r);
            if (rc == OA_CANCEL) {
                cl_copy(err, "Login cancelled", cap);
                return OA_CANCEL;
            }
            if (rc) {
                cl_copy(err, r.msg, cap);
                return OA_FAIL;
            }
        }
        if (r.st == DP_DONE)
            return 0;
        if (r.st == DP_FAIL) {
            cl_copy(err, r.msg, cap);
            return OA_FAIL;
        }
        if (r.st == DP_SLOW) {
            slow++;
            if (r.interval > 0) {
                interval_ms = r.interval * 1000L;
                if (interval_ms < 1000)
                    interval_ms = 1000;
            } else {
                interval_ms += 5000;
            }
        }
        rem = deadline < 0 ? interval_ms : deadline - elapsed_ms(io, t0);
        if (rem <= 0)
            break;
        if (io->sleep(io->u, rem < interval_ms ? rem : interval_ms)) {
            cl_copy(err, "Login cancelled", cap);
            return OA_CANCEL;
        }
    }
    cl_copy(err, slow > 0 ? "Device flow timed out after one or more slow_down responses. This is often caused by clock "
                            "drift in WSL or VM environments. Please sync or restart the VM clock and try again."
                          : "Device flow timed out",
            cap);
    return OA_FAIL;
}

/* ---- the entry ---- */

static void e_open(jw *e)
{
    jw_rawz(e, "{\"type\":\"oauth\"");
}

static void e_str(jw *e, const char *key, const char *val)
{
    jw_rawz(e, ",\"");
    jw_rawz(e, key);
    jw_rawz(e, "\":");
    jw_strz(e, val);
}

/* "expires": now + add_s seconds - margin_s seconds, in milliseconds */
static void e_expires(jw *e, llm_ts now, long add_s, long margin_s)
{
    jw_rawz(e, ",\"expires\":");
    oa_write_ms(e, now.sec + add_s - margin_s, now.ms);
}

/* ---- 1. openai: Sign in with ChatGPT (openai-chatgpt.ts) ---- */

#define CG_AUTHORIZE "https://auth.openai.com/api/accounts/authorize"
#define CG_TOKEN "https://auth.openai.com/api/accounts/oauth/token"
#define CG_RESOURCE "https://api.openai.com/v1"
#define CG_REDIRECT "http://127.0.0.1:1455/auth/callback"
#define CG_SCOPE "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
#define CG_DIRECT "chatgpt.tokens.use.direct"
#define CG_FORM "accept: application/json\r\ncontent-type: application/x-www-form-urlencoded\r\n"

/* requestToken: POST form, the answer as an object. Errors as pi's. */
static int cg_token(oa_io *io, const char *form, hres *r, jv *o, char *err, long cap)
{
    char b[320], n[16];
    int rc = exch(io, "POST", CG_TOKEN, CG_FORM, form, r, err, cap);
    if (rc)
        return rc;
    if (!ok(r)) {
        body_text(&r->body, b, sizeof(b));
        num(r->status, n);
        say(err, cap, "OpenAI OAuth token request failed (", n, "): ", b[0] ? b : status_text(r->status), 0, 0, 0, 0, 0, 0);
        hfree(r);
        return OA_FAIL;
    }
    if (!as_obj(r, o)) {
        cl_copy(err, "OpenAI OAuth token response must be an object", cap);
        hfree(r);
        return OA_FAIL;
    }
    return 0;
}

/* credentialFromTokenResponse */
static int cg_entry(oa_io *io, jv t, const char *client_id, jw *entry, char *err, long cap)
{
    char *access = jstr_ne(t, "access_token"), *refresh = 0, *scope = 0;
    jv v;
    long ex = 0;
    int rc = OA_FAIL, has_direct = 0;
    const char *p;
    if (access && has_ws_only(access)) {
        free(access);
        access = 0;
    }
    if (!access) {
        cl_copy(err, "OpenAI OAuth token response has invalid access_token", cap);
        return OA_FAIL;
    }
    refresh = jstr_ne(t, "refresh_token");
    if (!refresh) {
        cl_copy(err, "OpenAI OAuth token response has invalid refresh_token", cap);
        goto out;
    }
    scope = jstr_ne(t, "scope");
    if (!scope) {
        cl_copy(err, "OpenAI OAuth token response has invalid scope", cap);
        goto out;
    }
    if (!json_get(t, "expires_in", &v) || json_type(v) != J_NUM || (ex = json_long(v, 0)) <= 0) {
        cl_copy(err, "OpenAI OAuth token response has invalid expires_in", cap);
        goto out;
    }
    for (p = scope; *p;) {
        const char *e;
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        for (e = p; *e && *e != ' ' && *e != '\t' && *e != '\n'; e++)
            ;
        if (e - p == (long)strlen(CG_DIRECT) && !memcmp(p, CG_DIRECT, (size_t)(e - p)))
            has_direct = 1;
        p = e;
    }
    if (!has_direct) {
        cl_copy(err, "OpenAI OAuth grant did not include " CG_DIRECT, cap);
        goto out;
    }
    e_open(entry);
    e_str(entry, "access", access);
    e_str(entry, "refresh", refresh);
    e_expires(entry, io->now(io->u), ex, 180);
    e_str(entry, "clientId", client_id);
    jw_rawz(entry, ",\"scopes\":[");
    {
        int k = 0;
        for (p = scope; *p;) {
            const char *e;
            while (*p == ' ' || *p == '\t' || *p == '\n')
                p++;
            for (e = p; *e && *e != ' ' && *e != '\t' && *e != '\n'; e++)
                ;
            if (e > p) {
                if (k++)
                    jw_raw(entry, ",", 1);
                jw_str(entry, p, (long)(e - p));
            }
            p = e;
        }
    }
    jw_rawz(entry, "]}");
    rc = 0;
out:
    free(access);
    free(refresh);
    free(scope);
    return rc;
}

static int uuid_ok(const char *s)
{
    int i;
    if (!s || strlen(s) != 36)
        return 0;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return 0;
        } else if (hexv((unsigned char)s[i]) < 0)
            return 0;
    }
    return 1;
}

static int login_openai(oa_io *io, jw *entry, char *err, long cap)
{
    char verifier[48], challenge[48], state[48], nonce[48], host[48], line[1500], code[900], cid[200], st[200], e[200];
    jw url, form;
    hres r;
    jv t;
    purl u;
    int rc;
    long i;
    if (!uuid_ok(io->device_id)) {
        cl_copy(err, "Sign in with ChatGPT requires a device ID (UUID) for this installation", cap);
        return OA_FAIL;
    }
    cl_copy(host, "urn:uuid:", sizeof(host));
    cl_cat(host, io->device_id, sizeof(host));
    for (i = 9; host[i]; i++)
        if (host[i] >= 'A' && host[i] <= 'Z')
            host[i] = (char)(host[i] + 32);
    if (pkce(io, verifier, challenge) || rand_b64(io, 32, state) || rand_b64(io, 32, nonce)) {
        cl_copy(err, no_random, cap);
        return OA_FAIL;
    }
    jw_init(&url);
    jw_rawz(&url, CG_AUTHORIZE "?");
    {
        jw q;
        jw_init(&q);
        oa_form_add(&q, "client_id", "dynamic_agent_client");
        oa_form_add(&q, "agent_name_hint", io->agent_name ? io->agent_name : "Pi");
        oa_form_add(&q, "ext_agent_host_id", host);
        oa_form_add(&q, "response_type", "code");
        oa_form_add(&q, "redirect_uri", CG_REDIRECT);
        oa_form_add(&q, "resource", CG_RESOURCE);
        oa_form_add(&q, "scope", CG_SCOPE);
        oa_form_add(&q, "state", state);
        oa_form_add(&q, "code_challenge", challenge);
        oa_form_add(&q, "code_challenge_method", "S256");
        oa_form_add(&q, "nonce", nonce);
        jw_raw(&url, q.p, q.n);
        jw_free(&q);
    }
    io->info(io->u, OA_I_AUTH_URL, url.p,
             "Complete sign-in in your browser. If the callback does not complete, paste the final redirect URL here.");
    jw_free(&url);
    if (io->prompt(io->u, OA_P_MANUAL, "Complete login in your browser, or paste the final redirect URL here:",
                   CG_REDIRECT, line, sizeof(line))) {
        cl_copy(err, "Login cancelled", cap);
        return OA_CANCEL;
    }
    /* authorizationResultFromManualInput */
    if (!url_parse(line, &u)) {
        cl_copy(err, "Paste the full callback URL from the browser", cap);
        return OA_FAIL;
    }
    if (!url_origin_is(&u, "http", "127.0.0.1", 1455) || strcmp(u.path, "/auth/callback")) {
        cl_copy(err, "The pasted callback URL must start with " CG_REDIRECT, cap);
        return OA_FAIL;
    }
    if (u.query && qget(u.query, u.qn, "error", e, sizeof(e)) && e[0]) {
        say(err, cap, "ChatGPT authorization failed: ", e, 0, 0, 0, 0, 0, 0, 0, 0);
        return OA_FAIL;
    }
    if (!u.query || !qget(u.query, u.qn, "code", code, sizeof(code)) || !code[0]) {
        cl_copy(err, "Missing authorization code", cap);
        return OA_FAIL;
    }
    if (!qget(u.query, u.qn, "state", st, sizeof(st)) || !st[0]) {
        cl_copy(err, "Missing OAuth state", cap);
        return OA_FAIL;
    }
    if (strcmp(st, state)) {
        cl_copy(err, "OAuth state mismatch", cap);
        return OA_FAIL;
    }
    cid[0] = 0;
    qget(u.query, u.qn, "client_id", cid, sizeof(cid));
    {
        /* .trim() */
        long a = 0, b = (long)strlen(cid);
        while (a < b && (cid[a] == ' ' || cid[a] == '\t'))
            a++;
        while (b > a && (cid[b - 1] == ' ' || cid[b - 1] == '\t'))
            b--;
        memmove(cid, cid + a, (size_t)(b - a));
        cid[b - a] = 0;
    }
    if (!cid[0]) {
        cl_copy(err, "OpenAI OAuth registration callback did not contain an issued client ID", cap);
        return OA_FAIL;
    }
    io->info(io->u, OA_I_PROGRESS, "Exchanging authorization code for tokens...", 0);
    jw_init(&form);
    oa_form_add(&form, "grant_type", "authorization_code");
    oa_form_add(&form, "client_id", cid);
    oa_form_add(&form, "code", code);
    oa_form_add(&form, "code_verifier", verifier);
    oa_form_add(&form, "redirect_uri", CG_REDIRECT);
    oa_form_add(&form, "resource", CG_RESOURCE);
    rc = cg_token(io, form.p, &r, &t, err, cap);
    jw_free(&form);
    memset(verifier, 0, sizeof(verifier));
    if (rc)
        return rc;
    {
        jv idv;
        char *idt = json_get(t, "id_token", &idv) && json_type(idv) == J_STR ? json_strdup(idv, 0) : 0;
        int bad = !idt || has_ws_only(idt);
        free(idt);
        if (bad) {
            cl_copy(err, "OpenAI OAuth token response did not contain an ID token", cap);
            hfree(&r);
            return OA_FAIL;
        }
    }
    rc = cg_entry(io, t, cid, entry, err, cap);
    hfree(&r);
    return rc;
}

static int refresh_openai(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char *cid = jstr(cred, "clientId"), *rt = jstr(cred, "refresh");
    jw form;
    hres r;
    jv t;
    int rc;
    if (!cid || has_ws_only(cid)) {
        free(cid);
        free(rt);
        cl_copy(err, "Stored OpenAI OAuth credential does not contain an issued client ID; reconnect ChatGPT", cap);
        return OA_FAIL;
    }
    jw_init(&form);
    oa_form_add(&form, "grant_type", "refresh_token");
    oa_form_add(&form, "client_id", cid);
    oa_form_add(&form, "refresh_token", rt ? rt : "");
    oa_form_add(&form, "resource", CG_RESOURCE);
    rc = cg_token(io, form.p, &r, &t, err, cap);
    jw_free(&form);
    if (!rc) {
        rc = cg_entry(io, t, cid, entry, err, cap);
        hfree(&r);
    }
    if (rt)
        memset(rt, 0, strlen(rt));
    free(rt);
    free(cid);
    return rc;
}

/* ---- 2. openai-codex (openai-codex.ts) ---- */

#define CX_CLIENT "app_EMoamEEZ73f0CkXaXp7hrann"
#define CX_BASE "https://auth.openai.com"
#define CX_TOKEN CX_BASE "/oauth/token"
#define CX_REDIRECT "http://localhost:1455/auth/callback"
#define CX_DEVICE_CODE CX_BASE "/api/accounts/deviceauth/usercode"
#define CX_DEVICE_TOKEN CX_BASE "/api/accounts/deviceauth/token"
#define CX_DEVICE_URI CX_BASE "/codex/device"
#define CX_DEVICE_REDIRECT CX_BASE "/deviceauth/callback"
#define CX_FORM "Content-Type: application/x-www-form-urlencoded\r\n"
#define CX_JSON "Content-Type: application/json\r\n"

/* readTokenResponse + credentialsFromToken: the entry {type, access, refresh, expires, accountId}. op: "exchange" or
 * "refresh" */
static int cx_entry(oa_io *io, hres *r, const char *op, jw *entry, char *err, long cap)
{
    char b[320], n[16], acct[200];
    jv t, v;
    char *access, *refresh;
    long ex = 0;
    if (!ok(r)) {
        body_text(&r->body, b, sizeof(b));
        num(r->status, n);
        say(err, cap, "OpenAI Codex token ", op, " failed (", n, "): ", b[0] ? b : status_text(r->status), 0, 0, 0, 0);
        return OA_FAIL;
    }
    if (!as_obj(r, &t)) {
        body_text(&r->body, b, sizeof(b));
        say(err, cap, "OpenAI Codex token ", op, " response missing fields: ", b, 0, 0, 0, 0, 0, 0);
        return OA_FAIL;
    }
    access = jstr_ne(t, "access_token");
    refresh = jstr_ne(t, "refresh_token");
    if (!access || !refresh || !json_get(t, "expires_in", &v) || json_type(v) != J_NUM) {
        body_text(&r->body, b, sizeof(b));
        say(err, cap, "OpenAI Codex token ", op, " response missing fields: ", b, 0, 0, 0, 0, 0, 0);
        free(access);
        free(refresh);
        return OA_FAIL;
    }
    ex = json_long(v, 0);
    if (llm_jwt_account_id(access, acct, sizeof(acct))) {
        cl_copy(err, "Failed to extract accountId from token", cap);
        free(access);
        free(refresh);
        return OA_FAIL;
    }
    e_open(entry);
    e_str(entry, "access", access);
    e_str(entry, "refresh", refresh);
    e_expires(entry, io->now(io->u), ex, 0);
    e_str(entry, "accountId", acct);
    jw_rawz(entry, "}");
    memset(access, 0, strlen(access));
    memset(refresh, 0, strlen(refresh));
    free(access);
    free(refresh);
    return 0;
}

static int cx_exchange(oa_io *io, const char *code, const char *verifier, const char *redirect, jw *entry, char *err,
                       long cap)
{
    jw form;
    hres r;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "grant_type", "authorization_code");
    oa_form_add(&form, "client_id", CX_CLIENT);
    oa_form_add(&form, "code", code);
    oa_form_add(&form, "code_verifier", verifier);
    oa_form_add(&form, "redirect_uri", redirect);
    rc = exch(io, "POST", CX_TOKEN, CX_FORM, form.p, &r, err, cap);
    jw_free(&form);
    if (rc)
        return rc;
    rc = cx_entry(io, &r, "exchange", entry, err, cap);
    hfree(&r);
    return rc;
}

typedef struct cx_dev {
    oa_io *io;
    char id[400], user[120];
    char auth_code[900], verifier[200];
} cx_dev;

/* pollOpenAICodexDeviceAuth's poll */
static int cx_poll(void *c, dpr *pr)
{
    cx_dev *d = (cx_dev *)c;
    jw body;
    hres r;
    char err[200], b[320], n[16];
    int rc;
    jw_init(&body);
    jw_rawz(&body, "{\"device_auth_id\":");
    jw_strz(&body, d->id);
    jw_rawz(&body, ",\"user_code\":");
    jw_strz(&body, d->user);
    jw_rawz(&body, "}");
    rc = exch(d->io, "POST", CX_DEVICE_TOKEN, CX_JSON, body.p, &r, err, sizeof(err));
    jw_free(&body);
    if (rc) {
        cl_copy(pr->msg, err, sizeof(pr->msg));
        return rc;
    }
    if (ok(&r)) {
        jv o;
        char *ac, *cv;
        if (!as_obj(&r, &o) || !(ac = jstr_ne(o, "authorization_code"))) {
            body_text(&r.body, b, sizeof(b));
            say(pr->msg, sizeof(pr->msg), "Invalid OpenAI Codex device auth token response: ", b, 0, 0, 0, 0, 0, 0, 0, 0);
            pr->st = DP_FAIL;
            hfree(&r);
            return 0;
        }
        cv = jstr_ne(o, "code_verifier");
        if (!cv) {
            free(ac);
            body_text(&r.body, b, sizeof(b));
            say(pr->msg, sizeof(pr->msg), "Invalid OpenAI Codex device auth token response: ", b, 0, 0, 0, 0, 0, 0, 0, 0);
            pr->st = DP_FAIL;
            hfree(&r);
            return 0;
        }
        cl_copy(d->auth_code, ac, sizeof(d->auth_code));
        cl_copy(d->verifier, cv, sizeof(d->verifier));
        free(ac);
        free(cv);
        pr->st = DP_DONE;
        hfree(&r);
        return 0;
    }
    if (r.status == 403 || r.status == 404) {
        pr->st = DP_PENDING;
        hfree(&r);
        return 0;
    }
    {
        /* the error code: {"error": "x"} or {"error": {"code": "x"}} */
        jv o, e, c2;
        char code[96];
        code[0] = 0;
        if (as_obj(&r, &o) && json_get(o, "error", &e)) {
            if (json_type(e) == J_STR)
                json_str(e, code, sizeof(code));
            else if (json_type(e) == J_OBJ && json_get(e, "code", &c2) && json_type(c2) == J_STR)
                json_str(c2, code, sizeof(code));
        }
        if (!strcmp(code, "deviceauth_authorization_pending")) {
            pr->st = DP_PENDING;
        } else if (!strcmp(code, "slow_down")) {
            pr->st = DP_SLOW;
        } else {
            body_text(&r.body, b, sizeof(b));
            num(r.status, n);
            say(pr->msg, sizeof(pr->msg), "OpenAI Codex device auth failed with status ", n, b[0] ? ": " : "", b, 0, 0, 0, 0, 0, 0);
            pr->st = DP_FAIL;
        }
    }
    hfree(&r);
    return 0;
}

static int cx_login_device(oa_io *io, jw *entry, char *err, long cap)
{
    jw body;
    hres r;
    jv o;
    cx_dev d;
    char b[320], n[16];
    long interval = 0;
    int rc;
    memset(&d, 0, sizeof(d));
    d.io = io;
    jw_init(&body);
    jw_rawz(&body, "{\"client_id\":\"" CX_CLIENT "\"}");
    rc = exch(io, "POST", CX_DEVICE_CODE, CX_JSON, body.p, &r, err, cap);
    jw_free(&body);
    if (rc)
        return rc;
    if (!ok(&r)) {
        if (r.status == 404) {
            cl_copy(err, "OpenAI Codex device code login is not enabled for this server. Use browser login or verify the server URL.",
                    cap);
        } else {
            body_text(&r.body, b, sizeof(b));
            num(r.status, n);
            say(err, cap, "OpenAI Codex device code request failed with status ", n, b[0] ? ": " : "", b, 0, 0, 0, 0, 0, 0);
        }
        hfree(&r);
        return OA_FAIL;
    }
    {
        char *did = 0, *uc = 0;
        jv iv;
        int good = 0;
        if (as_obj(&r, &o)) {
            did = jstr_ne(o, "device_auth_id");
            uc = jstr_ne(o, "user_code");
            if (json_get(o, "interval", &iv)) {
                if (json_type(iv) == J_NUM && json_long(iv, -1) >= 0) {
                    interval = json_long(iv, 0);
                    good = 1;
                } else if (json_type(iv) == J_STR) {
                    char tmp[32];
                    json_str(iv, tmp, sizeof(tmp));
                    if (tmp[0] >= '0' && tmp[0] <= '9') {
                        interval = atol(tmp);
                        good = 1;
                    }
                }
            }
        }
        if (!did || !uc || !good) {
            body_text(&r.body, b, sizeof(b));
            say(err, cap, "Invalid OpenAI Codex device code response: ", b, 0, 0, 0, 0, 0, 0, 0, 0);
            free(did);
            free(uc);
            hfree(&r);
            return OA_FAIL;
        }
        cl_copy(d.id, did, sizeof(d.id));
        cl_copy(d.user, uc, sizeof(d.user));
        free(did);
        free(uc);
    }
    hfree(&r);
    io->info(io->u, OA_I_DEVICE_CODE, d.user, CX_DEVICE_URI);
    rc = device_poll(io, interval, 15 * 60, 0, cx_poll, &d, err, cap);
    if (rc)
        return rc;
    return cx_exchange(io, d.auth_code, d.verifier, CX_DEVICE_REDIRECT, entry, err, cap);
}

static int cx_login_browser(oa_io *io, jw *entry, char *err, long cap)
{
    char verifier[48], challenge[48], state[40], line[1500], code[900], st[200];
    jw url;
    const char *p;
    int has_state = 0;
    if (pkce(io, verifier, challenge) || rand_hex(io, 16, state)) {
        cl_copy(err, no_random, cap);
        return OA_FAIL;
    }
    jw_init(&url);
    jw_rawz(&url, CX_BASE "/oauth/authorize?");
    {
        jw q;
        jw_init(&q);
        oa_form_add(&q, "response_type", "code");
        oa_form_add(&q, "client_id", CX_CLIENT);
        oa_form_add(&q, "redirect_uri", CX_REDIRECT);
        oa_form_add(&q, "scope", "openid profile email offline_access");
        oa_form_add(&q, "code_challenge", challenge);
        oa_form_add(&q, "code_challenge_method", "S256");
        oa_form_add(&q, "state", state);
        oa_form_add(&q, "id_token_add_organizations", "true");
        oa_form_add(&q, "codex_cli_simplified_flow", "true");
        oa_form_add(&q, "originator", io->agent_name ? io->agent_name : "pi");
        jw_raw(&url, q.p, q.n);
        jw_free(&q);
    }
    io->info(io->u, OA_I_AUTH_URL, url.p, "A browser window should open. Complete login to finish.");
    jw_free(&url);
    if (io->prompt(io->u, OA_P_MANUAL, "Complete login in your browser, or paste the authorization code / redirect URL here:",
                   CX_REDIRECT, line, sizeof(line))) {
        cl_copy(err, "Login cancelled", cap);
        return OA_CANCEL;
    }
    /* parseAuthorizationInput */
    code[0] = st[0] = 0;
    {
        long a = 0, b = (long)strlen(line);
        purl u;
        while (a < b && (line[a] == ' ' || line[a] == '\t' || line[a] == '\n' || line[a] == '\r'))
            a++;
        while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == '\n' || line[b - 1] == '\r'))
            b--;
        memmove(line, line + a, (size_t)(b - a));
        line[b - a] = 0;
        if (!line[0]) {
            ;
        } else if (url_parse(line, &u)) {
            if (u.query) {
                qget(u.query, u.qn, "code", code, sizeof(code));
                has_state = qget(u.query, u.qn, "state", st, sizeof(st));
            }
        } else if ((p = strchr(line, '#')) != 0) {
            /* value.split("#", 2): the code and the state */
            long cl = (long)(p - line);
            const char *s2 = p + 1, *h2 = strchr(s2, '#');
            long sl = h2 ? (long)(h2 - s2) : (long)strlen(s2);
            if (cl >= (long)sizeof(code))
                cl = (long)sizeof(code) - 1;
            memcpy(code, line, (size_t)cl);
            code[cl] = 0;
            if (sl >= (long)sizeof(st))
                sl = (long)sizeof(st) - 1;
            memcpy(st, s2, (size_t)sl);
            st[sl] = 0;
            has_state = 1;
        } else if (strstr(line, "code=")) {
            has_state = qget(line, (long)strlen(line), "state", st, sizeof(st));
            qget(line, (long)strlen(line), "code", code, sizeof(code));
        } else
            cl_copy(code, line, sizeof(code));
    }
    if (has_state && st[0] && strcmp(st, state)) {
        cl_copy(err, "State mismatch", cap);
        return OA_FAIL;
    }
    if (!code[0]) {
        cl_copy(err, "Missing authorization code", cap);
        return OA_FAIL;
    }
    {
        int rc = cx_exchange(io, code, verifier, CX_REDIRECT, entry, err, cap);
        memset(verifier, 0, sizeof(verifier));
        return rc;
    }
}

static int login_codex(oa_io *io, jw *entry, char *err, long cap)
{
    static const char *const labels[2] = { "Browser login (default)", "Device code login (headless)" };
    int c = io->select(io->u, "Select OpenAI Codex login method:", labels, 2);
    if (c < 0) {
        cl_copy(err, "Login cancelled", cap);
        return OA_CANCEL;
    }
    return c == 1 ? cx_login_device(io, entry, err, cap) : cx_login_browser(io, entry, err, cap);
}

static int refresh_codex(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char *rt = jstr(cred, "refresh");
    jw form;
    hres r;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "grant_type", "refresh_token");
    oa_form_add(&form, "refresh_token", rt ? rt : "");
    oa_form_add(&form, "client_id", CX_CLIENT);
    rc = exch(io, "POST", CX_TOKEN, CX_FORM, form.p, &r, err, cap);
    jw_free(&form);
    if (rt)
        memset(rt, 0, strlen(rt));
    free(rt);
    if (rc) {
        if (rc == OA_FAIL) {
            char t[300];
            cl_copy(t, err, sizeof(t));
            say(err, cap, "OpenAI Codex token refresh error: ", t, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        return rc;
    }
    rc = cx_entry(io, &r, "refresh", entry, err, cap);
    hfree(&r);
    return rc;
}

/* ---- 3. github-copilot (github-copilot.ts) ---- */

#define GH_UA "GitHubCopilotChat/0.35.0"
#define GH_HEADERS "User-Agent: " GH_UA "\r\nEditor-Version: vscode/1.107.0\r\nEditor-Plugin-Version: copilot-chat/0.35.0\r\nCopilot-Integration-Id: vscode-chat\r\n"
#define GH_CLIENT "Iv1.b507a08c87ecfe98"

/* normalizeDomain: the host name of "company.ghe.com" or "https://company.ghe.com/x": 1 with it in out, 0 invalid / blank */
static int gh_domain(const char *in, char *out, long cap)
{
    char t[300];
    purl u;
    long a = 0, b = (long)strlen(in);
    while (a < b && (in[a] == ' ' || in[a] == '\t' || in[a] == '\n' || in[a] == '\r'))
        a++;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t' || in[b - 1] == '\n' || in[b - 1] == '\r'))
        b--;
    if (a == b || b - a >= 280)
        return 0;
    memcpy(t, in + a, (size_t)(b - a));
    t[b - a] = 0;
    if (!strstr(t, "://")) {
        char t2[300];
        cl_copy(t2, "https://", sizeof(t2));
        cl_cat(t2, t, sizeof(t2));
        cl_copy(t, t2, sizeof(t));
    }
    if (!url_parse(t, &u))
        return 0;
    cl_copy(out, u.host, cap);
    return 1;
}

static void gh_urls(const char *dom, char *dev, char *tok, char *cop, long cap)
{
    cl_copy(dev, "https://", cap);
    cl_cat(dev, dom, cap);
    cl_cat(dev, "/login/device/code", cap);
    cl_copy(tok, "https://", cap);
    cl_cat(tok, dom, cap);
    cl_cat(tok, "/login/oauth/access_token", cap);
    cl_copy(cop, "https://api.", cap);
    cl_cat(cop, dom, cap);
    cl_cat(cop, "/copilot_internal/v2/token", cap);
}

/* fetchJson's failure: "<status> <statusText>: <text>" */
static void gh_http_err(const hres *r, char *err, long cap)
{
    char n[16], b[300];
    num(r->status, n);
    body_text(&r->body, b, sizeof(b));
    say(err, cap, n, " ", status_text(r->status), ": ", b, 0, 0, 0, 0, 0);
}

/* getBaseUrlFromToken / getGitHubCopilotBaseUrl */
static void gh_base(const char *token, const char *domain, char *out, long cap)
{
    const char *p = strstr(token, "proxy-ep=");
    if (p) {
        long n = 0;
        char host[200];
        p += 9;
        while (p[n] && p[n] != ';' && n < (long)sizeof(host) - 1) {
            host[n] = p[n];
            n++;
        }
        host[n] = 0;
        if (n) {
            cl_copy(out, "https://", cap);
            if (!strncmp(host, "proxy.", 6)) {
                cl_cat(out, "api.", cap);
                cl_cat(out, host + 6, cap);
            } else
                cl_cat(out, host, cap);
            return;
        }
    }
    if (domain && *domain) {
        cl_copy(out, "https://copilot-api.", cap);
        cl_cat(out, domain, cap);
        return;
    }
    cl_copy(out, "https://api.individual.githubcopilot.com", cap);
}

/* refreshGitHubCopilotAccessToken: the entry {type, refresh, access, expires, enterpriseUrl?} start; the closing brace
 * is not written. access (the Copilot token) is also returned for the model calls */
static int gh_copilot_token(oa_io *io, const char *gh_token, const char *domain, jw *entry, char *access, long acap,
                            char *err, long cap)
{
    char dev[300], tok[300], cop[300], hdr[2400];
    hres r;
    jv o, v;
    char *t;
    long exp_at;
    int rc;
    gh_urls(domain && *domain ? domain : "github.com", dev, tok, cop, sizeof(dev));
    cl_copy(hdr, "Accept: application/json\r\nAuthorization: Bearer ", sizeof(hdr));
    cl_cat(hdr, gh_token, sizeof(hdr));
    cl_cat(hdr, "\r\n" GH_HEADERS, sizeof(hdr));
    rc = exch(io, "GET", cop, hdr, 0, &r, err, cap);
    memset(hdr, 0, sizeof(hdr));
    if (rc)
        return rc;
    if (!ok(&r)) {
        gh_http_err(&r, err, cap);
        hfree(&r);
        return OA_FAIL;
    }
    if (!as_obj(&r, &o)) {
        cl_copy(err, "Invalid Copilot token response", cap);
        hfree(&r);
        return OA_FAIL;
    }
    t = jstr(o, "token");
    if (!t || !json_get(o, "expires_at", &v) || json_type(v) != J_NUM) {
        free(t);
        cl_copy(err, "Invalid Copilot token response fields", cap);
        hfree(&r);
        return OA_FAIL;
    }
    exp_at = json_long(v, 0);
    e_open(entry);
    e_str(entry, "refresh", gh_token);
    e_str(entry, "access", t);
    jw_rawz(entry, ",\"expires\":");
    oa_write_ms(entry, exp_at - 300, 0);
    if (domain && *domain)
        e_str(entry, "enterpriseUrl", domain);
    cl_copy(access, t, acap);
    memset(t, 0, strlen(t));
    free(t);
    hfree(&r);
    return 0;
}

typedef struct gh_models {
    char **avail;
    int navail;
    char **policy;
    int npolicy;
} gh_models;

static void gh_models_free(gh_models *m)
{
    int i;
    for (i = 0; i < m->navail; i++)
        free(m->avail[i]);
    for (i = 0; i < m->npolicy; i++)
        free(m->policy[i]);
    free(m->avail);
    free(m->policy);
    memset(m, 0, sizeof(*m));
}

static int push(char ***v, int *n, const char *s, long len)
{
    char **g = (char **)realloc(*v, (size_t)(*n + 1) * sizeof(char *));
    char *c;
    if (!g)
        return -1;
    *v = g;
    c = (char *)malloc((size_t)len + 1);
    if (!c)
        return -1;
    memcpy(c, s, (size_t)len);
    c[len] = 0;
    g[(*n)++] = c;
    return 0;
}

/* parseGitHubCopilotModelCatalog: 0, -1 "Invalid Copilot models response" */
static int gh_catalog(oa_io *io, const hres *r, int allow_fallback, gh_models *m)
{
    jv o, data, it_v;
    jit it;
    int any_picker = 0, i;
    struct acct {
        char *id;
        int picker;
        int state;              /* 0 none, 1 disabled, 2 enabled, 3 unconfigured, 4 other */
    } *a = 0;
    int na = 0;
    memset(m, 0, sizeof(*m));
    if (!as_obj(r, &o) || !json_get(o, "data", &data) || json_type(data) != J_ARR)
        return -1;
    json_iter(data, &it);
    while (json_next(&it, 0, &it_v)) {
        jv idv, caps, sup, tc, pk, pol, st;
        struct acct *g;
        if (json_type(it_v) != J_OBJ || !json_get(it_v, "id", &idv) || json_type(idv) != J_STR)
            continue;
        if (json_get(it_v, "capabilities", &caps) && json_type(caps) == J_OBJ && json_get(caps, "supports", &sup) &&
            json_type(sup) == J_OBJ && json_get(sup, "tool_calls", &tc) && json_type(tc) == J_FALSE)
            continue;
        g = (struct acct *)realloc(a, (size_t)(na + 1) * sizeof(*a));
        if (!g)
            break;
        a = g;
        a[na].id = json_strdup(idv, 0);
        a[na].picker = json_get(it_v, "model_picker_enabled", &pk) && json_type(pk) == J_TRUE;
        a[na].state = 0;
        if (json_get(it_v, "policy", &pol) && json_type(pol) == J_OBJ && json_get(pol, "state", &st) &&
            json_type(st) == J_STR)
            a[na].state = json_streq(st, "disabled") ? 1 : json_streq(st, "enabled") ? 2 : json_streq(st, "unconfigured") ? 3 : 4;
        na++;
    }
    for (i = 0; i < na; i++)
        if (a[i].picker && a[i].state != 1) {
            any_picker = 1;
            break;
        }
    for (i = 0; i < na; i++) {
        int pick = a[i].picker && a[i].state != 1;
        if (any_picker || !allow_fallback ? pick : a[i].state == 2)
            push(&m->avail, &m->navail, a[i].id, (long)strlen(a[i].id));
    }
    for (i = 0; i < na; i++)
        if (a[i].state == 3 && io->known_model && io->known_model(io->u, "github-copilot", a[i].id) &&
            (a[i].picker || (allow_fallback && !any_picker)))
            push(&m->policy, &m->npolicy, a[i].id, (long)strlen(a[i].id));
    for (i = 0; i < na; i++)
        free(a[i].id);
    free(a);
    return 0;
}

/* fetchGitHubCopilotModels with one 5 s attempt per request and, with retries, up to two more after a 429 */
static int gh_models_fetch(oa_io *io, const char *copilot_token, const char *domain, int retries, gh_models *m,
                           char *err, long cap)
{
    char base[300], url[360], hdr[2600];
    int attempt, rc, fallback;
    hres r;
    gh_base(copilot_token, domain, base, sizeof(base));
    fallback = !strcmp(base, "https://api.individual.githubcopilot.com");
    cl_copy(url, base, sizeof(url));
    cl_cat(url, "/models", sizeof(url));
    cl_copy(hdr, "Accept: application/json\r\nAuthorization: Bearer ", sizeof(hdr));
    cl_cat(hdr, copilot_token, sizeof(hdr));
    cl_cat(hdr, "\r\n" GH_HEADERS "X-GitHub-Api-Version: 2026-06-01\r\n", sizeof(hdr));
    for (attempt = 0;; attempt++) {
        rc = exch(io, "GET", url, hdr, 0, &r, err, cap);
        if (rc) {
            memset(hdr, 0, sizeof(hdr));
            return rc;
        }
        if (r.status == 429 && attempt < retries) {
            hfree(&r);
            if (io->sleep(io->u, 500L << attempt)) {
                cl_copy(err, "Login cancelled", cap);
                memset(hdr, 0, sizeof(hdr));
                return OA_CANCEL;
            }
            continue;
        }
        break;
    }
    memset(hdr, 0, sizeof(hdr));
    if (!ok(&r)) {
        gh_http_err(&r, err, cap);
        hfree(&r);
        return OA_FAIL;
    }
    if (gh_catalog(io, &r, fallback, m)) {
        cl_copy(err, "Invalid Copilot models response", cap);
        hfree(&r);
        return OA_FAIL;
    }
    hfree(&r);
    return 0;
}

/* enableGitHubCopilotModel: 1 enabled, 0 not, -1 stop (429 after the retries), -2 cancelled */
static int gh_enable(oa_io *io, const char *token, const char *domain, const char *id)
{
    char base[300], url[420], hdr[2600], err[200];
    int attempt, rc;
    hres r;
    gh_base(token, domain, base, sizeof(base));
    cl_copy(url, base, sizeof(url));
    cl_cat(url, "/models/", sizeof(url));
    cl_cat(url, id, sizeof(url));
    cl_cat(url, "/policy", sizeof(url));
    cl_copy(hdr, "Content-Type: application/json\r\nAuthorization: Bearer ", sizeof(hdr));
    cl_cat(hdr, token, sizeof(hdr));
    cl_cat(hdr, "\r\n" GH_HEADERS "openai-intent: chat-policy\r\nx-interaction-type: chat-policy\r\n", sizeof(hdr));
    for (attempt = 0;; attempt++) {
        rc = exch(io, "POST", url, hdr, "{\"state\":\"enabled\"}", &r, err, sizeof(err));
        if (rc == OA_CANCEL) {
            memset(hdr, 0, sizeof(hdr));
            return -2;
        }
        if (rc) {
            memset(hdr, 0, sizeof(hdr));
            return 0;
        }
        if (r.status == 429 && attempt < 2) {
            hfree(&r);
            if (io->sleep(io->u, 500L << attempt)) {
                memset(hdr, 0, sizeof(hdr));
                return -2;
            }
            continue;
        }
        break;
    }
    memset(hdr, 0, sizeof(hdr));
    rc = r.status == 429 ? -1 : ok(&r);
    hfree(&r);
    return rc;
}

typedef struct gh_dev {
    oa_io *io;
    char tok_url[300], device_code[200];
    char token[400];
} gh_dev;

static int gh_poll(void *c, dpr *pr)
{
    gh_dev *d = (gh_dev *)c;
    jw form;
    hres r;
    char err[200];
    jv o, v;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "client_id", GH_CLIENT);
    oa_form_add(&form, "device_code", d->device_code);
    oa_form_add(&form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    rc = exch(d->io, "POST", d->tok_url,
              "Accept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\nUser-Agent: " GH_UA "\r\n",
              form.p, &r, err, sizeof(err));
    jw_free(&form);
    if (rc) {
        cl_copy(pr->msg, err, sizeof(pr->msg));
        return rc;
    }
    if (!ok(&r)) {
        gh_http_err(&r, pr->msg, sizeof(pr->msg));
        hfree(&r);
        return OA_FAIL;
    }
    if (as_obj(&r, &o) && json_get(o, "access_token", &v) && json_type(v) == J_STR) {
        json_str(v, d->token, sizeof(d->token));
        pr->st = DP_DONE;
    } else if (as_obj(&r, &o) && json_get(o, "error", &v) && json_type(v) == J_STR) {
        char ec[96], desc[200];
        jv iv, dv;
        json_str(v, ec, sizeof(ec));
        if (!strcmp(ec, "authorization_pending")) {
            pr->st = DP_PENDING;
        } else if (!strcmp(ec, "slow_down")) {
            pr->st = DP_SLOW;
            pr->interval = json_get(o, "interval", &iv) && json_type(iv) == J_NUM ? json_long(iv, 0) : 0;
        } else {
            desc[0] = 0;
            if (json_get(o, "error_description", &dv) && json_type(dv) == J_STR)
                json_str(dv, desc, sizeof(desc));
            say(pr->msg, sizeof(pr->msg), "Device flow failed: ", ec, desc[0] ? ": " : "", desc, 0, 0, 0, 0, 0, 0);
            pr->st = DP_FAIL;
        }
    } else {
        cl_copy(pr->msg, "Invalid device token response", sizeof(pr->msg));
        pr->st = DP_FAIL;
    }
    hfree(&r);
    return 0;
}

static int login_copilot(oa_io *io, jw *entry, char *err, long cap)
{
    char input[300], domain[160], dev[300], tok[300], cop[300], access[800];
    char *dcode = 0, *ucode = 0, *vuri = 0;
    hres r;
    jw form, base;
    jv o, v;
    gh_dev d;
    gh_models m;
    int rc, ent = 0;
    long interval = -1, expires = 0;
    if (io->prompt(io->u, OA_P_TEXT, "GitHub Enterprise URL/domain (blank for github.com)", "company.ghe.com", input,
                   sizeof(input))) {
        cl_copy(err, "Login cancelled", cap);
        return OA_CANCEL;
    }
    ent = gh_domain(input, domain, sizeof(domain));
    if (!ent && !has_ws_only(input)) {
        cl_copy(err, "Invalid GitHub Enterprise URL/domain", cap);
        return OA_FAIL;
    }
    if (!ent)
        domain[0] = 0;
    gh_urls(ent ? domain : "github.com", dev, tok, cop, sizeof(dev));
    jw_init(&form);
    oa_form_add(&form, "client_id", GH_CLIENT);
    oa_form_add(&form, "scope", "read:user");
    rc = exch(io, "POST", dev,
              "Accept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\nUser-Agent: " GH_UA "\r\n",
              form.p, &r, err, cap);
    jw_free(&form);
    if (rc)
        return rc;
    if (!ok(&r)) {
        gh_http_err(&r, err, cap);
        hfree(&r);
        return OA_FAIL;
    }
    if (!as_obj(&r, &o)) {
        cl_copy(err, "Invalid device code response", cap);
        hfree(&r);
        return OA_FAIL;
    }
    dcode = jstr(o, "device_code");
    ucode = jstr(o, "user_code");
    vuri = jstr(o, "verification_uri");
    if (!dcode || !ucode || !vuri || (json_get(o, "interval", &v) && json_type(v) != J_NUM) ||
        !json_get(o, "expires_in", &v) || json_type(v) != J_NUM) {
        cl_copy(err, "Invalid device code response fields", cap);
        goto bad;
    }
    expires = json_long(v, 0);
    if (json_get(o, "interval", &v))
        interval = json_long(v, 0);
    {
        purl u;
        if (!url_parse(vuri, &u)) {
            cl_copy(err, "Untrusted verification_uri in device code response", cap);
            goto bad;
        }
    }
    hfree(&r);
    memset(&d, 0, sizeof(d));
    d.io = io;
    cl_copy(d.tok_url, tok, sizeof(d.tok_url));
    cl_copy(d.device_code, dcode, sizeof(d.device_code));
    io->info(io->u, OA_I_DEVICE_CODE, ucode, vuri);
    free(dcode);
    free(ucode);
    free(vuri);
    dcode = ucode = vuri = 0;
    rc = device_poll(io, interval, expires, 1, gh_poll, &d, err, cap);
    if (rc)
        return rc;
    jw_init(&base);
    rc = gh_copilot_token(io, d.token, ent ? domain : 0, &base, access, sizeof(access), err, cap);
    if (rc) {
        jw_free(&base);
        memset(&d, 0, sizeof(d));
        return rc;
    }
    memset(&d, 0, sizeof(d));
    rc = gh_models_fetch(io, access, ent ? domain : 0, 2, &m, err, cap);
    if (rc) {
        jw_free(&base);
        memset(access, 0, sizeof(access));
        return rc;
    }
    {
        char **en = 0;
        int nen = 0, i, j;
        if (m.npolicy > 0) {
            io->info(io->u, OA_I_PROGRESS, "Enabling models...", 0);
            for (i = 0; i < m.npolicy; i++) {
                int e = gh_enable(io, access, ent ? domain : 0, m.policy[i]);
                if (e == -2) {
                    cl_copy(err, "Login cancelled", cap);
                    rc = OA_CANCEL;
                    break;
                }
                if (e == -1)
                    break;
                if (e == 1)
                    push(&en, &nen, m.policy[i], (long)strlen(m.policy[i]));
            }
        }
        if (!rc) {
            int k = 0;
            jw_raw(entry, base.p, base.n);
            jw_rawz(entry, ",\"availableModelIds\":[");
            for (i = 0; i < m.navail + nen; i++) {
                const char *id = i < m.navail ? m.avail[i] : en[i - m.navail];
                int dup = 0;
                for (j = 0; j < i; j++)
                    if (!strcmp(j < m.navail ? m.avail[j] : en[j - m.navail], id))
                        dup = 1;
                if (dup)
                    continue;
                if (k++)
                    jw_raw(entry, ",", 1);
                jw_strz(entry, id);
            }
            jw_rawz(entry, "]}");
        }
        for (i = 0; i < nen; i++)
            free(en[i]);
        free(en);
    }
    gh_models_free(&m);
    jw_free(&base);
    memset(access, 0, sizeof(access));
    return rc;
bad:
    free(dcode);
    free(ucode);
    free(vuri);
    hfree(&r);
    return OA_FAIL;
}

static int refresh_copilot(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char *gh = jstr(cred, "refresh"), *ent = jstr(cred, "enterpriseUrl");
    char domain[160], access[800];
    int has = ent && ent[0] && gh_domain(ent, domain, sizeof(domain));
    jw base;
    gh_models m;
    int rc, i;
    free(ent);
    if (!gh)
        gh = (char *)calloc(1, 1);
    jw_init(&base);
    rc = gh_copilot_token(io, gh, has ? domain : 0, &base, access, sizeof(access), err, cap);
    memset(gh, 0, strlen(gh));
    free(gh);
    if (rc) {
        jw_free(&base);
        return rc;
    }
    rc = gh_models_fetch(io, access, has ? domain : 0, 0, &m, err, cap);
    memset(access, 0, sizeof(access));
    if (rc) {
        jw_free(&base);
        return rc;
    }
    jw_raw(entry, base.p, base.n);
    jw_rawz(entry, ",\"availableModelIds\":[");
    for (i = 0; i < m.navail; i++) {
        if (i)
            jw_raw(entry, ",", 1);
        jw_strz(entry, m.avail[i]);
    }
    jw_rawz(entry, "]}");
    gh_models_free(&m);
    jw_free(&base);
    return 0;
}

/* ---- 4. kimi-coding (kimi-coding.ts) ---- */

#define KM_CLIENT "17e5f671-d194-4dfb-9706-5516cb48c098"
#define KM_FORM "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"

/* getOauthHost: KIMI_CODE_OAUTH_HOST, KIMI_OAUTH_HOST, else auth.kimi.com, without trailing slashes */
static void km_host(oa_io *io, char *out, long cap)
{
    long n = -1;
    out[0] = 0;
    if (io->getenv) {
        n = io->getenv(io->u, "KIMI_CODE_OAUTH_HOST", out, cap);
        if (n <= 0)
            n = io->getenv(io->u, "KIMI_OAUTH_HOST", out, cap);
    }
    if (n <= 0)
        cl_copy(out, "https://auth.kimi.com", cap);
    n = (long)strlen(out);
    while (n > 0 && out[n - 1] == '/')
        out[--n] = 0;
}

static int http_url_ok(const char *s)
{
    purl u;
    return s && *s && url_parse(s, &u);
}

/* parseTokenResponse: appends access / refresh / expires members (after the opening) to the entry */
static int km_entry(oa_io *io, jv t, const char *op, const hres *r, jw *entry, char *err, long cap)
{
    char *a = jstr_ne(t, "access_token"), *rf = jstr_ne(t, "refresh_token");
    jv v;
    long ex = 0;
    char b[320];
    if (!a || !rf || !json_get(t, "expires_in", &v) || json_type(v) != J_NUM || (ex = json_long(v, 0)) <= 0) {
        free(a);
        free(rf);
        body_text(&r->body, b, sizeof(b));
        say(err, cap, "Kimi Code token ", op, " response missing fields: ", b, 0, 0, 0, 0, 0, 0);
        return OA_FAIL;
    }
    e_open(entry);
    e_str(entry, "access", a);
    e_str(entry, "refresh", rf);
    e_expires(entry, io->now(io->u), ex, 0);
    jw_rawz(entry, "}");
    memset(a, 0, strlen(a));
    memset(rf, 0, strlen(rf));
    free(a);
    free(rf);
    return 0;
}

typedef struct km_dev {
    oa_io *io;
    char host[200], code[300];
    jw *entry;
} km_dev;

static int km_poll(void *c, dpr *pr)
{
    km_dev *d = (km_dev *)c;
    jw form;
    hres r;
    char err[200], url[300], b[320], n[16];
    jv o, v;
    int rc;
    cl_copy(url, d->host, sizeof(url));
    cl_cat(url, "/api/oauth/token", sizeof(url));
    jw_init(&form);
    oa_form_add(&form, "client_id", KM_CLIENT);
    oa_form_add(&form, "device_code", d->code);
    oa_form_add(&form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    rc = exch(d->io, "POST", url, KM_FORM, form.p, &r, err, sizeof(err));
    jw_free(&form);
    if (rc) {
        cl_copy(pr->msg, err, sizeof(pr->msg));
        return rc;
    }
    num(r.status, n);
    if (r.status >= 500) {
        body_text(&r.body, b, sizeof(b));
        say(pr->msg, sizeof(pr->msg), "Kimi Code device token request failed with status ", n, b[0] ? ": " : "", b, 0, 0, 0, 0, 0, 0);
        pr->st = DP_FAIL;
        hfree(&r);
        return 0;
    }
    if (as_obj(&r, &o) && ok(&r) && json_get(o, "access_token", &v) && json_type(v) == J_STR) {
        char em[300];
        if (km_entry(d->io, o, "poll", &r, d->entry, em, sizeof(em))) {
            cl_copy(pr->msg, em, sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else
            pr->st = DP_DONE;
    } else {
        char ec[96], desc[200], dsc[240];
        jv dv;
        ec[0] = desc[0] = 0;
        if (as_obj(&r, &o)) {
            if (json_get(o, "error", &v) && json_type(v) == J_STR)
                json_str(v, ec, sizeof(ec));
            if (json_get(o, "error_description", &dv) && json_type(dv) == J_STR) {
                json_str(dv, desc, sizeof(desc));
                cl_copy(dsc, ": ", sizeof(dsc));
                cl_cat(dsc, desc, sizeof(dsc));
            } else
                dsc[0] = 0;
        } else
            dsc[0] = 0;
        if (!strcmp(ec, "authorization_pending")) {
            pr->st = DP_PENDING;
        } else if (!strcmp(ec, "slow_down")) {
            pr->st = DP_SLOW;
            pr->interval = as_obj(&r, &o) && json_get(o, "interval", &v) && json_type(v) == J_NUM ? json_long(v, 0) : 0;
        } else if (!strcmp(ec, "expired_token")) {
            cl_copy(pr->msg, "Kimi Code device authorization expired. Please restart login.", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else if (!strcmp(ec, "access_denied")) {
            cl_copy(pr->msg, "Kimi Code login was denied.", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else {
            say(pr->msg, sizeof(pr->msg), "Kimi Code device token request failed (status ", n, ")", ec[0] ? ": " : "", ec, dsc, 0, 0, 0, 0);
            pr->st = DP_FAIL;
        }
    }
    hfree(&r);
    return 0;
}

static int login_kimi(oa_io *io, jw *entry, char *err, long cap)
{
    km_dev d;
    char url[300], b[320], n[16];
    jw form;
    hres r;
    jv o, v;
    char *dc = 0, *uc = 0, *vu = 0, *vc = 0;
    long interval, expires;
    int rc;
    memset(&d, 0, sizeof(d));
    d.io = io;
    d.entry = entry;
    km_host(io, d.host, sizeof(d.host));
    cl_copy(url, d.host, sizeof(url));
    cl_cat(url, "/api/oauth/device_authorization", sizeof(url));
    jw_init(&form);
    oa_form_add(&form, "client_id", KM_CLIENT);
    rc = exch(io, "POST", url, KM_FORM, form.p, &r, err, cap);
    jw_free(&form);
    if (rc)
        return rc;
    if (!ok(&r)) {
        body_text(&r.body, b, sizeof(b));
        num(r.status, n);
        say(err, cap, "Kimi Code device authorization failed with status ", n, b[0] ? ": " : "", b, 0, 0, 0, 0, 0, 0);
        hfree(&r);
        return OA_FAIL;
    }
    if (as_obj(&r, &o)) {
        dc = jstr(o, "device_code");
        uc = jstr(o, "user_code");
        vu = jstr(o, "verification_uri");
        vc = jstr(o, "verification_uri_complete");
    }
    if (!dc || !uc || !vu || !vc || !http_url_ok(vc) || !http_url_ok(vu)) {
        body_text(&r.body, b, sizeof(b));
        say(err, cap, "Invalid Kimi Code device authorization response: ", b, 0, 0, 0, 0, 0, 0, 0, 0);
        free(dc);
        free(uc);
        free(vu);
        free(vc);
        hfree(&r);
        return OA_FAIL;
    }
    interval = json_get(o, "interval", &v) && json_type(v) == J_NUM && json_long(v, 0) > 0 ? json_long(v, 0) : 5;
    expires = json_get(o, "expires_in", &v) && json_type(v) == J_NUM && json_long(v, 0) > 0 ? json_long(v, 0) : 15 * 60;
    hfree(&r);
    cl_copy(d.code, dc, sizeof(d.code));
    io->info(io->u, OA_I_DEVICE_CODE, uc, vc);
    free(dc);
    free(uc);
    free(vu);
    free(vc);
    return device_poll(io, interval, expires, 1, km_poll, &d, err, cap);
}

static int refresh_kimi(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char host[200], url[300], *rt = jstr(cred, "refresh"), b[320], n[16];
    int attempt, rc = OA_FAIL;
    char last[300];
    last[0] = 0;
    km_host(io, host, sizeof(host));
    cl_copy(url, host, sizeof(url));
    cl_cat(url, "/api/oauth/token", sizeof(url));
    for (attempt = 0; attempt <= 3; attempt++) {
        jw form;
        hres r;
        jv o;
        int x;
        if (attempt > 0 && io->sleep(io->u, 1000L << (attempt - 1))) {
            cl_copy(err, "Kimi Code token refresh aborted", cap);
            rc = OA_CANCEL;
            goto out;
        }
        if (io->cancelled && io->cancelled(io->u)) {
            cl_copy(err, "Kimi Code token refresh aborted", cap);
            rc = OA_CANCEL;
            goto out;
        }
        jw_init(&form);
        oa_form_add(&form, "client_id", KM_CLIENT);
        oa_form_add(&form, "grant_type", "refresh_token");
        oa_form_add(&form, "refresh_token", rt ? rt : "");
        x = exch(io, "POST", url, KM_FORM, form.p, &r, last, sizeof(last));
        jw_free(&form);
        if (x)
            continue;           /* a network failure: lastError, the next attempt */
        num(r.status, n);
        if (ok(&r)) {
            if (as_obj(&r, &o))
                rc = km_entry(io, o, "refresh", &r, entry, err, cap);
            else
                cl_copy(err, "Kimi Code token refresh response missing fields: null", cap);
            hfree(&r);
            goto out;
        }
        {
            char ec[96], desc[200];
            ec[0] = desc[0] = 0;
            if (as_obj(&r, &o)) {
                jv v;
                if (json_get(o, "error", &v) && json_type(v) == J_STR)
                    json_str(v, ec, sizeof(ec));
                if (json_get(o, "error_description", &v) && json_type(v) == J_STR)
                    json_str(v, desc, sizeof(desc));
            }
            if (r.status == 401 || r.status == 403 || !strcmp(ec, "invalid_grant")) {
                say(err, cap, "Kimi Code token refresh unauthorized (status ", n, ")", desc[0] ? ": " : "", desc, 0, 0, 0, 0, 0);
                hfree(&r);
                goto out;
            }
            if ((r.status == 429 || r.status >= 500) && attempt < 3) {
                say(last, sizeof(last), "Kimi Code token refresh failed with status ", n, 0, 0, 0, 0, 0, 0, 0, 0);
                hfree(&r);
                continue;
            }
            body_text(&r.body, b, sizeof(b));
            say(err, cap, "Kimi Code token refresh failed with status ", n, b[0] ? ": " : "", b, 0, 0, 0, 0, 0, 0);
            hfree(&r);
            goto out;
        }
    }
    if (last[0])
        cl_copy(err, last, cap);
    else
        cl_copy(err, "Kimi Code token refresh failed", cap);
out:
    if (rt)
        memset(rt, 0, strlen(rt));
    free(rt);
    return rc;
}

/* ---- 5. xai (xai.ts) ---- */

#define XA_CLIENT "b1a00492-073a-47ea-816f-4c329264a828"
#define XA_SCOPE "openid profile email offline_access grok-cli:access api:access"
#define XA_DEVICE "https://auth.x.ai/oauth2/device/code"
#define XA_TOKEN "https://auth.x.ai/oauth2/token"
#define XA_FORM "Accept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n"

/* postForm: the answer as an object (or xAI's "returned invalid JSON") */
static int xa_post(oa_io *io, const char *url, const jw *form, hres *r, jv *o, char *err, long cap)
{
    int rc = exch(io, "POST", url, XA_FORM, form->p, r, err, cap);
    char n[16];
    if (rc)
        return rc;
    if (!as_obj(r, o)) {
        num(r->status, n);
        say(err, cap, "xAI OAuth returned invalid JSON (HTTP ", n, ")", 0, 0, 0, 0, 0, 0, 0);
        hfree(r);
        return OA_FAIL;
    }
    return 0;
}

/* requestFailure: "xAI OAuth <action> failed (HTTP n): error: description" */
static void xa_fail(const char *action, const hres *r, jv o, char *err, long cap)
{
    char n[16], e[160], d[240];
    jv v;
    e[0] = d[0] = 0;
    if (json_get(o, "error", &v) && json_type(v) == J_STR)
        json_str(v, e, sizeof(e));
    if (json_get(o, "error_description", &v) && json_type(v) == J_STR)
        json_str(v, d, sizeof(d));
    num(r->status, n);
    say(err, cap, "xAI OAuth ", action, " failed (HTTP ", n, ")", e[0] || d[0] ? ": " : "", e, e[0] && d[0] ? ": " : "", d, 0);
}

/* credentialsFromTokenResponse: the entry; prev: the refresh token a refresh keeps when the answer has none */
static int xa_entry(oa_io *io, jv o, const char *prev, jw *entry, char *err, long cap)
{
    char *a = jstr_ne(o, "access_token"), *rf = 0;
    jv v;
    long ex = 3600;
    if (!a) {
        cl_copy(err, "Invalid xAI OAuth response field: access_token", cap);
        return OA_FAIL;
    }
    if (!json_get(o, "refresh_token", &v) && prev && *prev) {
        rf = (char *)malloc(strlen(prev) + 1);
        if (rf)
            strcpy(rf, prev);
    } else
        rf = jstr_ne(o, "refresh_token");
    if (!rf) {
        cl_copy(err, "Invalid xAI OAuth response field: refresh_token", cap);
        free(a);
        return OA_FAIL;
    }
    if (json_get(o, "expires_in", &v)) {
        if (json_type(v) != J_NUM || json_long(v, 0) <= 0) {
            cl_copy(err, "Invalid xAI OAuth response field: expires_in", cap);
            free(a);
            free(rf);
            return OA_FAIL;
        }
        ex = json_long(v, 0);
    }
    e_open(entry);
    e_str(entry, "access", a);
    e_str(entry, "refresh", rf);
    e_expires(entry, io->now(io->u), ex, 5 * 60);
    jw_rawz(entry, "}");
    memset(a, 0, strlen(a));
    memset(rf, 0, strlen(rf));
    free(a);
    free(rf);
    return 0;
}

typedef struct xa_dev {
    oa_io *io;
    char code[300];
    jw *entry;
} xa_dev;

static int xa_poll(void *c, dpr *pr)
{
    xa_dev *d = (xa_dev *)c;
    jw form;
    hres r;
    jv o, v;
    char err[200];
    int rc;
    jw_init(&form);
    oa_form_add(&form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    oa_form_add(&form, "client_id", XA_CLIENT);
    oa_form_add(&form, "device_code", d->code);
    rc = xa_post(d->io, XA_TOKEN, &form, &r, &o, err, sizeof(err));
    jw_free(&form);
    if (rc) {
        cl_copy(pr->msg, err, sizeof(pr->msg));
        return rc;
    }
    if (ok(&r)) {
        if (xa_entry(d->io, o, 0, d->entry, err, sizeof(err))) {
            cl_copy(pr->msg, err, sizeof(pr->msg));
            hfree(&r);
            return OA_FAIL;
        }
        pr->st = DP_DONE;
    } else {
        char ec[96];
        ec[0] = 0;
        if (json_get(o, "error", &v) && json_type(v) == J_STR)
            json_str(v, ec, sizeof(ec));
        if (!strcmp(ec, "authorization_pending")) {
            pr->st = DP_PENDING;
        } else if (!strcmp(ec, "slow_down")) {
            pr->st = DP_SLOW;
            pr->interval = json_get(o, "interval", &v) && json_type(v) == J_NUM ? json_long(v, 0) : 0;
        } else if (!strcmp(ec, "access_denied") || !strcmp(ec, "authorization_denied")) {
            cl_copy(pr->msg, "xAI device authorization was denied", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else if (!strcmp(ec, "expired_token")) {
            cl_copy(pr->msg, "xAI device code expired", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else {
            xa_fail("device token polling", &r, o, pr->msg, sizeof(pr->msg));
            pr->st = DP_FAIL;
        }
    }
    hfree(&r);
    return 0;
}

/* an https URL for the user to open: new URL(raw) must parse and be https; the href (a bare host gets its "/") */
static int https_only(const char *raw, char *out, long cap)
{
    purl u;
    if (!url_parse(raw, &u) || strcmp(u.scheme, "https"))
        return 0;
    cl_copy(out, raw, cap);
    return 1;
}

static int login_xai(oa_io *io, jw *entry, char *err, long cap)
{
    jw form;
    hres r;
    jv o, v;
    xa_dev d;
    char *dc, *uc, *vu, *vc = 0;
    char show[400];
    long interval = -1, expires;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "client_id", XA_CLIENT);
    oa_form_add(&form, "scope", XA_SCOPE);
    oa_form_add(&form, "referrer", "pi");
    rc = xa_post(io, XA_DEVICE, &form, &r, &o, err, cap);
    jw_free(&form);
    if (rc)
        return rc;
    if (!ok(&r)) {
        xa_fail("device authorization", &r, o, err, cap);
        hfree(&r);
        return OA_FAIL;
    }
    /* parseDeviceCode */
    dc = jstr_ne(o, "device_code");
    uc = jstr_ne(o, "user_code");
    vu = jstr_ne(o, "verification_uri");
    if (!dc) {
        cl_copy(err, "Invalid xAI OAuth response field: device_code", cap);
        goto bad;
    }
    if (!uc) {
        cl_copy(err, "Invalid xAI OAuth response field: user_code", cap);
        goto bad;
    }
    if (!vu) {
        cl_copy(err, "Invalid xAI OAuth response field: verification_uri", cap);
        goto bad;
    }
    if (!https_only(vu, show, sizeof(show))) {
        cl_copy(err, "Untrusted verification URI in xAI OAuth response", cap);
        goto bad;
    }
    vc = jstr_ne(o, "verification_uri_complete");
    if (vc) {
        if (!https_only(vc, show, sizeof(show))) {
            cl_copy(err, "Untrusted verification URI in xAI OAuth response", cap);
            goto bad;
        }
    }
    if (json_get(o, "interval", &v) && json_type(v) == J_NUM && json_long(v, 0) > 0)
        interval = json_long(v, 0);
    expires = jpos(o, "expires_in");
    if (!expires) {
        cl_copy(err, "Invalid xAI OAuth response field: expires_in", cap);
        goto bad;
    }
    memset(&d, 0, sizeof(d));
    d.io = io;
    d.entry = entry;
    cl_copy(d.code, dc, sizeof(d.code));
    io->info(io->u, OA_I_DEVICE_CODE, uc, vc ? vc : vu);
    free(dc);
    free(uc);
    free(vu);
    free(vc);
    hfree(&r);
    return device_poll(io, interval, expires, 1, xa_poll, &d, err, cap);
bad:
    free(dc);
    free(uc);
    free(vu);
    free(vc);
    hfree(&r);
    return OA_FAIL;
}

static int refresh_xai(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char *rt = jstr(cred, "refresh");
    jw form;
    hres r;
    jv o;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "grant_type", "refresh_token");
    oa_form_add(&form, "client_id", XA_CLIENT);
    oa_form_add(&form, "refresh_token", rt ? rt : "");
    rc = xa_post(io, XA_TOKEN, &form, &r, &o, err, cap);
    jw_free(&form);
    if (!rc) {
        if (!ok(&r)) {
            xa_fail("token refresh", &r, o, err, cap);
            rc = OA_FAIL;
        } else
            rc = xa_entry(io, o, rt ? rt : "", entry, err, cap);
        hfree(&r);
    }
    if (rt)
        memset(rt, 0, strlen(rt));
    free(rt);
    return rc;
}

/* ---- 6. meta (meta.ts) ---- */

#define MT_CLIENT "1031625952748946"
#define MT_AUTH "https://auth.meta.com"
#define MT_FORM "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"
#define MT_MINT "https://api.meta.ai/muse-code/key"

/* errorDetail: ": <text>" of the first of error_description, detail, message, error that is a non-blank string */
static void mt_detail(const hres *r, char *out, long cap)
{
    static const char *const keys[4] = { "error_description", "detail", "message", "error" };
    jv o, v;
    int i;
    out[0] = 0;
    if (!as_obj(r, &o))
        return;
    for (i = 0; i < 4; i++)
        if (json_get(o, keys[i], &v) && json_type(v) == J_STR) {
            char t[240];
            long a = 0, b;
            json_str(v, t, sizeof(t));
            b = (long)strlen(t);
            while (a < b && (t[a] == ' ' || t[a] == '\t' || t[a] == '\n'))
                a++;
            while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '\n'))
                b--;
            if (b > a) {
                cl_copy(out, ": ", cap);
                t[b] = 0;
                cl_cat(out, t + a, cap);
                return;
            }
        }
}

/* mintApiKey: the entry for the identity token */
static int mt_mint(oa_io *io, const char *identity, jw *entry, char *err, long cap)
{
    char hdr[2600], n[16], det[300], *key;
    hres r;
    jv o;
    int rc;
    cl_copy(hdr, "Accept: application/json\r\nAuthorization: Bearer ", sizeof(hdr));
    cl_cat(hdr, identity, sizeof(hdr));
    cl_cat(hdr, "\r\nContent-Type: application/json\r\nx-api-version: 1.0.0\r\n", sizeof(hdr));
    rc = exch(io, "POST", MT_MINT, hdr, "{}", &r, err, cap);
    memset(hdr, 0, sizeof(hdr));
    if (rc)
        return rc;
    num(r.status, n);
    mt_detail(&r, det, sizeof(det));
    if (r.status == 401 || r.status == 403) {
        say(err, cap, "Meta session expired (status ", n, "). Run `/login meta` to sign in again.", det, 0, 0, 0, 0, 0, 0);
        hfree(&r);
        return OA_FAIL;
    }
    if (!ok(&r)) {
        say(err, cap, "Meta API key mint failed with status ", n, det, 0, 0, 0, 0, 0, 0, 0);
        hfree(&r);
        return OA_FAIL;
    }
    key = as_obj(&r, &o) ? jstr_ne(o, "api_key") : 0;
    if (!key) {
        char *au = as_obj(&r, &o) ? jstr(o, "action_url") : 0;
        if (au && !http_url_ok(au)) {
            free(au);
            au = 0;
        }
        say(err, cap, "Meta did not issue an API key.", au ? " Complete setup at " : "", au ? au : "", 0, 0, 0, 0, 0, 0, 0);
        free(au);
        hfree(&r);
        return OA_FAIL;
    }
    e_open(entry);
    e_str(entry, "refresh", identity);
    e_str(entry, "access", key);
    jw_rawz(entry, ",\"expires\":");
    {
        llm_ts t = io->now(io->u);
        oa_write_ms(entry, t.sec + 24L * 60 * 60, t.ms);
    }
    jw_rawz(entry, "}");
    memset(key, 0, strlen(key));
    free(key);
    hfree(&r);
    return 0;
}

typedef struct mt_dev {
    oa_io *io;
    char code[300], token[1600];
} mt_dev;

static int mt_poll(void *c, dpr *pr)
{
    mt_dev *d = (mt_dev *)c;
    jw form;
    hres r;
    char err[200], n[16], det[300];
    jv o, v;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    oa_form_add(&form, "device_code", d->code);
    oa_form_add(&form, "client_id", MT_CLIENT);
    rc = exch(d->io, "POST", MT_AUTH "/oidc/device/token/", MT_FORM, form.p, &r, err, sizeof(err));
    jw_free(&form);
    if (rc) {
        cl_copy(pr->msg, err, sizeof(pr->msg));
        return rc;
    }
    num(r.status, n);
    mt_detail(&r, det, sizeof(det));
    if (ok(&r) && as_obj(&r, &o) && json_get(o, "access_token", &v) && json_type(v) == J_STR && v.n > 2) {
        json_str(v, d->token, sizeof(d->token));
        pr->st = DP_DONE;
    } else {
        char ec[96];
        ec[0] = 0;
        if (as_obj(&r, &o) && json_get(o, "error", &v) && json_type(v) == J_STR)
            json_str(v, ec, sizeof(ec));
        if (!strcmp(ec, "authorization_pending")) {
            pr->st = DP_PENDING;
        } else if (!strcmp(ec, "slow_down")) {
            pr->st = DP_SLOW;
            pr->interval = jpos(o, "interval");
        } else if (!strcmp(ec, "access_denied")) {
            cl_copy(pr->msg, "Meta login was denied.", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else if (!strcmp(ec, "expired_token")) {
            cl_copy(pr->msg, "Meta device authorization expired. Please restart login.", sizeof(pr->msg));
            pr->st = DP_FAIL;
        } else {
            say(pr->msg, sizeof(pr->msg), "Meta device token request failed with status ", n, det, 0, 0, 0, 0, 0, 0, 0);
            pr->st = DP_FAIL;
        }
    }
    hfree(&r);
    return 0;
}

static int login_meta(oa_io *io, jw *entry, char *err, long cap)
{
    jw form;
    hres r;
    jv o;
    mt_dev d;
    char det[300], n[16], b[320], show[400];
    char *dc = 0, *uc = 0, *vc = 0, *vu = 0;
    long interval, expires;
    int rc;
    jw_init(&form);
    oa_form_add(&form, "client_id", MT_CLIENT);
    rc = exch(io, "POST", MT_AUTH "/oidc/device/authorization/", MT_FORM, form.p, &r, err, cap);
    jw_free(&form);
    if (rc)
        return rc;
    num(r.status, n);
    if (!ok(&r)) {
        mt_detail(&r, det, sizeof(det));
        say(err, cap, "Meta device authorization failed with status ", n, det, 0, 0, 0, 0, 0, 0, 0);
        hfree(&r);
        return OA_FAIL;
    }
    if (as_obj(&r, &o)) {
        dc = jstr_ne(o, "device_code");
        uc = jstr_ne(o, "user_code");
        vc = jstr(o, "verification_uri_complete");
        vu = jstr(o, "verification_uri");
    }
    if (dc && uc) {
        if (vc && http_url_ok(vc))
            cl_copy(show, vc, sizeof(show));
        else if (vu && http_url_ok(vu))
            cl_copy(show, vu, sizeof(show));
        else
            show[0] = 0;
    } else
        show[0] = 0;
    if (!dc || !uc || !show[0]) {
        body_text(&r.body, b, sizeof(b));
        say(err, cap, "Invalid Meta device authorization response: ", b, 0, 0, 0, 0, 0, 0, 0, 0);
        free(dc);
        free(uc);
        free(vc);
        free(vu);
        hfree(&r);
        return OA_FAIL;
    }
    interval = jpos(o, "interval");
    expires = jpos(o, "expires_in");
    memset(&d, 0, sizeof(d));
    d.io = io;
    cl_copy(d.code, dc, sizeof(d.code));
    io->info(io->u, OA_I_DEVICE_CODE, uc, show);
    free(dc);
    free(uc);
    free(vc);
    free(vu);
    hfree(&r);
    rc = device_poll(io, interval ? interval : -1, expires ? expires : -1, 1, mt_poll, &d, err, cap);
    if (rc)
        return rc;
    io->info(io->u, OA_I_PROGRESS, "Enabling Meta Model API access...", 0);
    rc = mt_mint(io, d.token, entry, err, cap);
    memset(&d, 0, sizeof(d));
    return rc;
}

static int refresh_meta(oa_io *io, jv cred, jw *entry, char *err, long cap)
{
    char *id = jstr(cred, "refresh");
    int rc = mt_mint(io, id ? id : "", entry, err, cap);
    if (id)
        memset(id, 0, strlen(id));
    free(id);
    return rc;
}

/* ---- the table ---- */

/* sorted by name, as the selector sorts (localeCompare, case-insensitive here): Anthropic, Azure, GitHub Copilot,
 * Google, Kimi For Coding, Meta, OpenAI, OpenAI Codex (legacy), xAI */
static const oa_provider providers[] = {
    { "anthropic", "Anthropic", 0, 0, "Anthropic API key", 0, 0, 0 },
    { "azure", "Azure", 0, 0, "Azure OpenAI API key", 0, 0, 0 },
    { "github-copilot", "GitHub Copilot", "GitHub Copilot", 0, "GitHub Copilot token", 1, login_copilot, refresh_copilot },
    { "google", "Google", 0, 0, "Gemini API key", 0, 0, 0 },
    { "kimi-coding", "Kimi For Coding", "Kimi Code (subscription)", "Sign in with Kimi Code", "Kimi API key", 1, login_kimi,
      refresh_kimi },
    { "meta", "Meta", "Meta (Muse subscription)", "Sign in with Meta", "Meta Model API key", 1, login_meta, refresh_meta },
    { "openai", "OpenAI", "OpenAI (ChatGPT subscription)", "Sign in with ChatGPT", "OpenAI API key", 1, login_openai,
      refresh_openai },
    { "openai-codex", "OpenAI Codex (legacy)", "OpenAI (ChatGPT Plus/Pro)", 0, 0, 1, login_codex, refresh_codex },
    { "xai", "xAI", "xAI (Grok/X subscription)", "Sign in with SuperGrok or X Premium", "xAI API key", 1, login_xai,
      refresh_xai }
};

int oa_count(void)
{
    return (int)(sizeof(providers) / sizeof(providers[0]));
}

const oa_provider *oa_nth(int i)
{
    return i >= 0 && i < oa_count() ? &providers[i] : 0;
}

const oa_provider *oa_find(const char *id)
{
    int i;
    for (i = 0; i < oa_count(); i++)
        if (!strcmp(providers[i].id, id))
            return &providers[i];
    return 0;
}
