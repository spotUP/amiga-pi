/* amiga-pi's JSON and RPC modes against pi 1.0.4's (plan H1-H7): pi_app() with `--mode json` and `--mode rpc` over the
 * stub transport, the answers pi was given (tests/pi/rpc/<name>.N.sse|http), the commands of the real run
 * (<name>.in) fed on the console's input, and every line written compared with the line pi wrote (<name>.out). The
 * goldens and the environment of each scenario (<name>.cfg.json) come from tools/pi_oracle/rpc_gen.py.
 *
 * A line is compared after the masks of tests/pi/rpc/masks.json (the one list of this test and of vtcon's rig
 * tools/rig/amigapi_rig.py), each with its reason there and a COMPATIBILITY.md row (group pi-modes), never a way to
 * hide one: keys starred (made per run, or compared where they live), fresh ids starred, pi 1.0.4's message fields
 * dropped (PH-S), and the rules: cost `total`, the text of an HTTP errorMessage, message_start usage, baseUrl, the
 * parse error text, get_available_models for the fixture's provider.
 * A command that arrives while the agent works is held by its `#after <event>` line until pi's run had that event in its
 * output, as the golden was made. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "pi_test.h"
#include "claude/util.h"
#include "pi_stub.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/host_posix.h"
#include "../amiga-pi/mode_json.h"
#include "../amiga-pi/mode_rpc.h"
#include "../amiga-pi/session3.h"
#include "claude/sys_posix.h"

#define DIR_RPC PI_TESTS "/pi/rpc/"
#define GOLD_CWD "/private/tmp/pirpc/project"

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
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = 0;
    }
    if (b)
        b[len] = 0;
    fclose(f);
    if (n)
        *n = len;
    return b;
}

static char *slurp_rpc(const char *name, const char *ext, long *n)
{
    char path[600];
    strcpy(path, DIR_RPC);
    strcat(path, name);
    strcat(path, ext);
    return slurp(path, n);
}

static void put_file(const char *path, const char *text, long n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, (size_t)n, f);
        fclose(f);
    }
}

/* ---- the masks ---- */

static int hexdig(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* an id made per run: a uuid, or an entry id of 8 hex digits (the fixed ids of the session fixtures start d000) */
static int fresh_id(const char *s)
{
    long n = (long)strlen(s), i;
    if (n == 36) {
        for (i = 0; i < 36; i++)
            if (i == 8 || i == 13 || i == 18 || i == 23 ? s[i] != '-' : !hexdig(s[i]))
                return 0;
        return 1;
    }
    if (n == 8 && strncmp(s, "d000", 4)) {
        for (i = 0; i < 8; i++)
            if (!hexdig(s[i]))
                return 0;
        return 1;
    }
    return 0;
}

/* the mask lists, from tests/pi/rpc/masks.json (the one list of this test and of vtcon's rig, with the reasons) */
#define MASK_MAX 24
typedef struct {
    char k[MASK_MAX][32];
    int n;
} key_list;
static key_list star_keys, id_keys, drop_keys;
static char fixture_provider[32];
static int masks_loaded;

static int in_list(const char *k, const key_list *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(k, l->k[i]))
            return 1;
    return 0;
}

static void keys_of(jv v, key_list *l)
{
    jit it;
    jv k, e;
    json_iter(v, &it);
    if (json_type(v) == J_OBJ)
        while (json_next(&it, &k, &e) && l->n < MASK_MAX)
            json_str(k, l->k[l->n++], sizeof(l->k[0]));
    else
        while (json_next(&it, 0, &e) && l->n < MASK_MAX)
            json_str(e, l->k[l->n++], sizeof(l->k[0]));
}

static int load_masks(void)
{
    long n;
    char *t;
    jv v, x, y;
    if (masks_loaded)
        return 1;
    t = slurp_rpc("masks", ".json", &n);
    if (!t || json_parse(t, n, &v) || !json_get(v, "star", &x) || !json_get(v, "drop", &y)) {
        free(t);
        return 0;
    }
    keys_of(x, &star_keys);
    keys_of(y, &drop_keys);
    if (!json_get(v, "ids", &x) || !json_get(x, "keys", &y) || !json_get(v, "fixtureProvider", &x)) {
        free(t);
        return 0;
    }
    keys_of(y, &id_keys);
    json_str(x, fixture_provider, sizeof(fixture_provider));
    free(t);
    masks_loaded = 1;
    return 1;
}

/* a cost number as whole micro-dollars: pi computes in binary floating point (0.0006000000000000001,
 * 0.0036149999999999997), ours in decimal micro-dollars (0.0006, 0.003615); equal to the micro-dollar */
static void micro_dollars(jv v, jw *o)
{
    char t[48], num[24];
    double d;
    long n = v.n < (long)sizeof(t) - 1 ? v.n : (long)sizeof(t) - 1;
    memcpy(t, v.p, (size_t)n);
    t[n] = 0;
    d = strtod(t, 0) * 1e6;
    cl_ltoa((long)(d < 0 ? d - 0.5 : d + 0.5), num);
    jw_rawz(o, "\"u$");
    jw_rawz(o, num);
    jw_rawz(o, "\"");
}

/* an HTTP error's text: pi's `400 {...}` (the SDK's APIError) or ours `HTTP 400: ...` */
static int http_error_text(jv v)
{
    char t[16];
    if (json_type(v) != J_STR)
        return 0;
    json_str(v, t, sizeof(t));
    if (!strncmp(t, "HTTP ", 5))
        memmove(t, t + 5, strlen(t + 5) + 1);
    return t[0] >= '1' && t[0] <= '5' && t[1] >= '0' && t[1] <= '9' && t[2] >= '0' && t[2] <= '9' && (t[3] == ' ' || t[3] == ':');
}

#define F_STARTMSG 1
#define F_COST 2

static void m_val(jv v, int flags, jw *o, const char *cwd);

static void m_str(jv v, jw *o, const char *cwd)
{
    long n, cl = (long)strlen(cwd), i;
    char *z = json_strdup(v, &n);
    jw t;
    if (!z)
        return;
    jw_init(&t);
    for (i = 0; i < n;) {
        if (cl && i + cl <= n && !memcmp(z + i, cwd, (size_t)cl)) {
            jw_rawz(&t, "<cwd>");
            i += cl;
        } else if (i + (long)strlen(GOLD_CWD) <= n && !memcmp(z + i, GOLD_CWD, strlen(GOLD_CWD))) {
            jw_rawz(&t, "<cwd>");
            i += (long)strlen(GOLD_CWD);
        } else
            jw_raw(&t, z + i++, 1);
    }
    jw_str(o, t.p, t.n);
    jw_free(&t);
    free(z);
}

static int str_is(jv obj, const char *key, const char *want)
{
    jv v;
    return json_get(obj, key, &v) && json_type(v) == J_STR && json_streq(v, want);
}

static void m_obj(jv v, int flags, jw *o, const char *cwd)
{
    jit it;
    jv k, val;
    char key[64];
    int first = 1;
    int assistant = str_is(v, "role", "assistant"), retry_start = str_is(v, "type", "auto_retry_start");
    int parse_err = str_is(v, "command", "parse");
    int start_msg = 0;
    jv m;
    if (str_is(v, "type", "message_start") && json_get(v, "message", &m) && str_is(m, "role", "assistant"))
        start_msg = 1;
    jw_rawz(o, "{");
    json_iter(v, &it);
    while (json_next(&it, &k, &val)) {
        jv t;
        json_str(k, key, sizeof(key));
        if (in_list(key, &drop_keys))
            continue;
        if (!first)
            jw_rawz(o, ",");
        first = 0;
        jw_strz(o, key);
        jw_rawz(o, ":");
        if (in_list(key, &star_keys))
            jw_rawz(o, "\"*\"");
        else if ((flags & F_COST) && json_type(val) == J_NUM)
            micro_dollars(val, o);
        else if (!strcmp(key, "errorMessage") && (assistant || retry_start) && http_error_text(val))
            jw_rawz(o, "\"<http error>\"");
        else if (in_list(key, &id_keys) && json_type(val) == J_STR) {
            char id[64];
            json_str(val, id, sizeof(id));
            if (fresh_id(id))
                jw_rawz(o, "\"*\"");
            else
                m_val(val, 0, o, cwd);
        } else if (!strcmp(key, "cost") && json_type(val) == J_OBJ && json_get(val, "total", &t))
            m_obj(val, F_COST, o, cwd);
        else if (!strcmp(key, "baseUrl") && json_type(val) == J_STR)
            jw_rawz(o, "\"<base>\"");
        else if (!strcmp(key, "error") && parse_err)
            jw_rawz(o, "\"*\"");
        else if (!strcmp(key, "usage") && (flags & F_STARTMSG))
            jw_rawz(o, "\"*\"");
        else if (!strcmp(key, "message") && start_msg)
            m_val(val, F_STARTMSG, o, cwd);
        else if (!strcmp(key, "models") && json_type(val) == J_ARR) {
            jit ai;
            jv e;
            int f2 = 1;
            jw_rawz(o, "[");
            json_iter(val, &ai);
            while (json_next(&ai, 0, &e))
                if (json_type(e) == J_OBJ && str_is(e, "provider", fixture_provider)) {
                    if (!f2)
                        jw_rawz(o, ",");
                    f2 = 0;
                    m_val(e, 0, o, cwd);
                }
            jw_rawz(o, "]");
        } else
            m_val(val, 0, o, cwd);
    }
    jw_rawz(o, "}");
}

static void m_val(jv v, int flags, jw *o, const char *cwd)
{
    switch (json_type(v)) {
    case J_OBJ:
        m_obj(v, flags, o, cwd);
        break;
    case J_ARR: {
        jit it;
        jv e;
        int first = 1;
        jw_rawz(o, "[");
        json_iter(v, &it);
        while (json_next(&it, 0, &e)) {
            if (!first)
                jw_rawz(o, ",");
            first = 0;
            m_val(e, 0, o, cwd);
        }
        jw_rawz(o, "]");
        break;
    }
    case J_STR:
        m_str(v, o, cwd);
        break;
    default:
        jw_raw(o, v.p, v.n);
    }
}

/* one record after the masks (cwd: this run's project directory) */
static void mask_line(const char *p, long n, const char *cwd, jw *o)
{
    jv v;
    if (json_parse(p, n, &v))
        jw_raw(o, p, n);
    else
        m_val(v, 0, o, cwd);
}

/* ---- the console: commands in, records out ---- */

#define MAX_LINES 64

static struct {
    char *in;                   /* the .in text, split into lines in place */
    char *line[MAX_LINES];
    int nline, next;
    char gate_type[48];         /* waiting for the Nth record of this type; "" none */
    int gate_need;
    int gates_of[MAX_LINES];
    char cur[8192];             /* the command being handed over */
    long cur_n, cur_at;
    jw out;                     /* every record written, with its line feed */
    int starved;                /* the reader found nothing to do many times: the run cannot finish */
    long polls;
} fd;

static int out_count(const char *type)
{
    char pat[80];
    int n = 0;
    const char *p = fd.out.p ? fd.out.p : "";
    size_t pl;
    strcpy(pat, "{\"type\":\"");
    strcat(pat, type);
    strcat(pat, "\"");
    pl = strlen(pat);
    while ((p = strstr(p, pat)) != 0) {
        if (p == fd.out.p || p[-1] == '\n')
            n++;
        p += pl;
    }
    return n;
}

/* the id of the N-th extension_ui_request of the output */
static int ui_id(int n, char *id, long cap)
{
    const char *p = fd.out.p ? fd.out.p : "", *e;
    int k = 0;
    while ((p = strstr(p, "{\"type\":\"extension_ui_request\",\"id\":\"")) != 0) {
        p += strlen("{\"type\":\"extension_ui_request\",\"id\":\"");
        if (++k == n) {
            e = strchr(p, '"');
            if (!e || e - p >= cap)
                return -1;
            memcpy(id, p, (size_t)(e - p));
            id[e - p] = 0;
            return 0;
        }
    }
    return -1;
}

/* the next line to hand over into fd.cur: 1, 0 not yet (a gate holds it), -1 no more */
static int feed_next(void)
{
    while (fd.next < fd.nline) {
        const char *l = fd.line[fd.next];
        if (!strncmp(l, "#after ", 7)) {
            if (fd.gate_type[0] == 0 || fd.gate_need == 0) {
                int i, need = 1;
                for (i = 0; i < fd.next; i++)
                    if (!strcmp(fd.line[i], l))
                        need++;
                strcpy(fd.gate_type, l + 7);
                fd.gate_need = need;
            }
            if (out_count(fd.gate_type) < fd.gate_need)
                return 0;
            fd.gate_type[0] = 0;
            fd.gate_need = 0;
            fd.next++;
            continue;
        }
        fd.cur_n = 0;
        while (*l) {
            if (!strncmp(l, "$ui:", 4)) {
                char id[64];
                int n = atoi(l + 4);
                if (ui_id(n, id, sizeof(id)))
                    strcpy(id, "missing");
                memcpy(fd.cur + fd.cur_n, id, strlen(id));
                fd.cur_n += (long)strlen(id);
                l += 4;
                while (*l >= '0' && *l <= '9')
                    l++;
            } else
                fd.cur[fd.cur_n++] = *l++;
        }
        fd.cur[fd.cur_n++] = '\n';
        fd.cur_at = 0;
        fd.next++;
        return 1;
    }
    return -1;
}

static long io_read(void *u, char *buf, long cap, long timeout_ms)
{
    long n;
    (void)u;
    fd.polls++;
    if (fd.cur_at >= fd.cur_n) {
        int rc = feed_next();
        if (rc < 0)
            return timeout_ms > 0 ? -1 : 0;     /* the input ends once the agent is idle */
        if (rc == 0) {
            if (timeout_ms > 0 && ++fd.starved > 200) {
                CHECK(0);                       /* a gate whose event never came */
                return -1;
            }
            return 0;
        }
    }
    n = fd.cur_n - fd.cur_at;
    if (n > cap)
        n = cap;
    memcpy(buf, fd.cur + fd.cur_at, (size_t)n);
    fd.cur_at += n;
    return n;
}

static void io_write(void *u, const char *s, long n)
{
    (void)u;
    jw_raw(&fd.out, s, n);
}

static unsigned long io_ms(void *u)
{
    return ps_ms(u);
}

/* ---- a scenario's sandbox ---- */

static char root[400], proj[440], agent[440], data[440];
static sys_posix sp;
static cl_sys sys_;
static long aclock;

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + aclock++;
    t.ms = 7;
    return t;
}

static long no_env(const char *name, char *out, long cap)
{
    (void)name;
    (void)out;
    (void)cap;
    return -1;
}

static void sandbox(void)
{
    const char *base = getenv("TMPDIR");
    char p[500];
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_modes_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    strcpy(proj, root);
    strcat(proj, "/project");
    strcpy(agent, root);
    strcat(agent, "/agent");
    strcpy(data, root);
    strcat(data, "/data");
    mkdir(proj, 0777);
    mkdir(agent, 0777);
    mkdir(data, 0777);
    strcpy(p, proj);
    strcat(p, "/notes.txt");
    put_file(p, "one\ntwo\nthree\n", 14);
    sys_posix_init(&sp, &sys_);
}

static void sandbox_clear(void)
{
    char cmd[600];
    if (strlen(root) < 10)
        return;
    strcpy(cmd, "rm -rf '");
    strcat(cmd, root);
    strcat(cmd, "'");
    if (system(cmd))
        CHECK(0);
    root[0] = 0;
}

/* the scenario's files: models.json, settings.json from its cfg, the session and the extra files */
static const char *extra_settings, *extra_retry;

static int setup_files(jv cfg, char *session_arg, long cap)
{
    char path[600], *models, *txt;
    long n;
    jw st;
    jv v, s, e;
    jit it, fi;
    char model[64], think[16], key[200], doc[80];
    int retry = 0;
    models = slurp_rpc("models", ".json", &n);
    if (!models)
        return -1;
    strcpy(path, agent);
    strcat(path, "/models.json");
    put_file(path, models, n);
    free(models);
    strcpy(model, "fake-model");
    strcpy(think, "off");
    if (json_get(cfg, "model", &v))
        json_str(v, model, sizeof(model));
    if (json_get(cfg, "thinking", &v))
        json_str(v, think, sizeof(think));
    if (json_get(cfg, "retry", &v))
        retry = json_type(v) == J_TRUE;
    jw_init(&st);
    jw_rawz(&st, "{\"defaultProvider\":\"fake\",\"defaultModel\":");
    jw_strz(&st, model);
    jw_rawz(&st, ",\"defaultThinkingLevel\":");
    jw_strz(&st, think);
    jw_rawz(&st, ",\"retry\":{\"enabled\":");
    jw_rawz(&st, retry ? "true" : "false");
    jw_rawz(&st, ",\"maxRetries\":2,\"baseDelayMs\":100");
    if (extra_retry)
        jw_rawz(&st, extra_retry);
    jw_rawz(&st, "},\"quietStartup\":true");
    if (json_get(cfg, "settings", &s) && json_type(s) == J_OBJ) {
        json_iter(s, &it);
        while (json_next(&it, &v, &e)) {
            json_str(v, key, sizeof(key));
            jw_rawz(&st, ",");
            jw_strz(&st, key);
            jw_rawz(&st, ":");
            jw_raw(&st, e.p, e.n);
        }
    }
    if (extra_settings) {
        jw_rawz(&st, ",");
        jw_rawz(&st, extra_settings);
    }
    jw_rawz(&st, "}");
    strcpy(path, agent);
    strcat(path, "/settings.json");
    put_file(path, st.p, st.n);
    jw_free(&st);
    session_arg[0] = 0;
    if (json_get(cfg, "session", &v) && json_type(v) == J_STR) {
        json_str(v, doc, sizeof(doc));
        strcpy(path, proj);
        strcat(path, "/session.jsonl");
        if (strcmp(doc, "new")) {
            char src[600];
            long k, l;
            jw t;
            strcpy(src, PI_TESTS "/pi/sessions/");
            strcat(src, doc);
            strcat(src, ".jsonl");
            txt = slurp(src, &n);
            if (!txt)
                return -1;
            jw_init(&t);
            for (k = 0; k < n;) {
                l = (long)strlen("/path/to/project");
                if (k + l <= n && !memcmp(txt + k, "/path/to/project", (size_t)l)) {
                    jw_rawz(&t, proj);
                    k += l;
                } else
                    jw_raw(&t, txt + k++, 1);
            }
            put_file(path, t.p, t.n);
            jw_free(&t);
            free(txt);
        }
        if ((long)strlen(path) < cap)
            strcpy(session_arg, path);
    }
    if (json_get(cfg, "files", &v) && json_type(v) == J_OBJ) {
        json_iter(v, &fi);
        while (json_next(&fi, &s, &e)) {
            char name[100], src[600];
            jw t;
            long k, l = (long)strlen("/path/to/project");
            json_str(s, name, sizeof(name));
            json_str(e, doc, sizeof(doc));
            strcpy(src, PI_TESTS "/pi/sessions/");
            strcat(src, doc);
            strcat(src, ".jsonl");
            txt = slurp(src, &n);
            if (!txt)
                return -1;
            jw_init(&t);
            for (k = 0; k < n;) {
                if (k + l <= n && !memcmp(txt + k, "/path/to/project", (size_t)l)) {
                    jw_rawz(&t, proj);
                    k += l;
                } else
                    jw_raw(&t, txt + k++, 1);
            }
            strcpy(path, proj);
            strcat(path, "/");
            strcat(path, name);
            put_file(path, t.p, t.n);
            jw_free(&t);
            free(txt);
        }
    }
    return 0;
}

/* ---- running a scenario ---- */

typedef struct run_opts {
    void (*ready)(pi_session *s);   /* the session once made (a test extension) */
    const char *more_args;          /* one more argument */
    const char *inline_in;          /* commands from here instead of the golden's .in */
    long rounds;                    /* out: pi_agent_rounds the run made */
    int rc;
    const char *force_mode;         /* run the scenario's answers in this mode (rpc) whatever its cfg says */
    const char *extra_settings;     /* members added to settings.json ("\"permissionGate\":true") */
    const char *extra_retry;        /* members added to its "retry" object (",\"maxAgentDelayMs\":40") */
    const char *first_http;         /* the whole HTTP answer given instead of the scenario's first, when that one is http */
    int host;                       /* run through the host binary's console (host_posix.c: its stdin reader, stdout, stderr), not
                                     * this test's: in a child process, the commands on a pipe as its stdin */
    long prompts;                   /* out with host: pi_prompt calls the run made */
    int nocmp;                      /* do not compare with the golden: the caller reads the output */
    char *out;                      /* out with nocmp: the records, malloc'ed */
} run_opts;

static void run_diff(const char *name, const char *want, const char *got)
{
    const char *w = want, *g = got;
    int line = 0, shown = 0, bad = 0;
    for (;;) {
        const char *we = strchr(w, '\n'), *ge = strchr(g, '\n');
        long wn = we ? (long)(we - w) : (long)strlen(w), gn = ge ? (long)(ge - g) : (long)strlen(g);
        line++;
        if (!*w && !*g)
            break;
        if (wn != gn || memcmp(w, g, (size_t)wn)) {
            bad++;
            if (shown++ < 2) {
                long k = 0;
                while (k < wn && k < gn && w[k] == g[k])
                    k++;
                printf("  %s line %d differs at %ld:\n  want ...%.*s\n  got  ...%.*s\n", name, line, k, (int)(wn - (k > 60 ? k - 60 : 0) > 200 ? 200 : wn - (k > 60 ? k - 60 : 0)),
                       w + (k > 60 ? k - 60 : 0), (int)(gn - (k > 60 ? k - 60 : 0) > 200 ? 200 : gn - (k > 60 ? k - 60 : 0)), g + (k > 60 ? k - 60 : 0));
            }
        }
        w = we ? we + 1 : w + wn;
        g = ge ? ge + 1 : g + gn;
        if (!we && !ge)
            break;
    }
    if (bad)
        printf("  %s: %d lines differ\n", name, bad);
}

/* the records as masked lines, one per line */
static void masked_text(const char *text, const char *cwd, jw *o)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        long n = e ? (long)(e - p) : (long)strlen(p);
        if (n > 0) {
            mask_line(p, n, cwd, o);
            jw_rawz(o, "\n");
        }
        p += n + (e ? 1 : 0);
    }
}

static char *dup_text(const char *t)
{
    char *c = (char *)malloc(strlen(t) + 1);
    if (c)
        strcpy(c, t);
    return c;
}

/* a run through pi_host_run(), the host binary's own program: the commands are its stdin (a pipe), the records its stdout
 * (a pipe), in a child process that has the stub transport; what the child counted (the exit code of pi_app, the loop's
 * rounds, the pi_prompt calls) comes back on a third pipe. 0 when the child ran. */
static int host_run(int argc, char **argv, const pi_platform *over, const char *input, jw *out, int *rc, long *rounds, long *prompts)
{
    int in_p[2], out_p[2], st_p[2], status;
    pid_t pid;
    char buf[4096], rep[128];
    long n, got = 0;
    if (pipe(in_p) || pipe(out_p) || pipe(st_p))
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int r;
        long r0 = pi_agent_rounds, p0 = pi_agent_prompts;     /* the parent's, copied */
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        close(st_p[0]);
        alarm(30);                      /* a reader that never delivers must fail the test, not hang it */
        if (chdir(proj) || setenv("PI_CODING_AGENT_DIR", agent, 1) || setenv("TMPDIR", root, 1))
            _exit(2);
        r = pi_host_run(argc, argv, over);
        snprintf(rep, sizeof(rep), "%d %ld %ld\n", r, pi_agent_rounds - r0, pi_agent_prompts - p0);
        if (write(st_p[1], rep, strlen(rep)) < 0)
            _exit(3);
        _exit(0);
    }
    close(in_p[0]);
    close(out_p[1]);
    close(st_p[1]);
    if (write(in_p[1], input, strlen(input)) < 0)
        CHECK(0);
    close(in_p[1]);                     /* the end of the input: the run goes on to its end */
    while ((n = (long)read(out_p[0], buf, sizeof(buf))) > 0)
        jw_raw(out, buf, n);
    close(out_p[0]);
    while ((n = (long)read(st_p[0], rep + got, sizeof(rep) - 1 - (size_t)got)) > 0)
        got += n;
    close(st_p[0]);
    rep[got] = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) || sscanf(rep, "%d %ld %ld", rc, rounds, prompts) != 3)
        return -1;
    return 0;
}

/* the scenario `name` run and compared with its golden; ro->inline_in replaces the golden's commands */
static int scenario(const char *name, run_opts *ro)
{
    char *cfg_t, *in_t = 0, *want_t;
    long cn, in_n = 0, wn;
    jv cfg, v;
    char session_arg[500], mode[16], p[600], tool[100];
    char *argv[24];
    int argc = 0, i, nans = 0, ok;
    pi_platform pl;
    cl_io io;
    jw wmask, gmask;
    long r0 = pi_agent_rounds;
    char arg_store[8][300], real_cwd[PATH_MAX];
    const char *mcwd = proj;
    long host_rounds = 0;
    int nstore = 0;
    jit it;
    jv a;

    cfg_t = slurp_rpc(name, ".cfg.json", &cn);
    want_t = slurp_rpc(name, ".out", &wn);
    if (!cfg_t || !want_t || json_parse(cfg_t, cn, &cfg)) {
        printf("  %s: golden missing\n", name);
        CHECK(0);
        free(cfg_t);
        free(want_t);
        return 0;
    }
    sandbox();
    extra_settings = ro ? ro->extra_settings : 0;
    extra_retry = ro ? ro->extra_retry : 0;
    if (setup_files(cfg, session_arg, sizeof(session_arg))) {
        printf("  %s: sandbox failed\n", name);
        CHECK(0);
        free(cfg_t);
        free(want_t);
        sandbox_clear();
        return 0;
    }
    strcpy(mode, "rpc");
    if (json_get(cfg, "mode", &v))
        json_str(v, mode, sizeof(mode));
    if (ro && ro->force_mode)
        strcpy(mode, ro->force_mode);
    memset(&fd, 0, sizeof(fd));
    jw_init(&fd.out);
    if (ro && ro->inline_in)
        in_t = dup_text(ro->inline_in);
    else if (!strcmp(mode, "rpc"))
        in_t = slurp_rpc(name, ".in", &in_n);
    if (in_t) {
        char *l = in_t;
        while (*l && fd.nline < MAX_LINES) {
            char *e = strchr(l, '\n');
            fd.line[fd.nline++] = l;
            if (!e)
                break;
            *e = 0;
            l = e + 1;
        }
    }
    (void)in_n;
    argv[argc++] = (char *)"amiga-pi";
    argv[argc++] = (char *)"--mode";
    argv[argc++] = mode;
    argv[argc++] = (char *)"--model";
    strcpy(arg_store[nstore], "fake/");
    if (json_get(cfg, "model", &v))
        json_str(v, arg_store[nstore] + 5, 200);
    else
        strcat(arg_store[nstore], "fake-model");
    argv[argc++] = arg_store[nstore++];
    if (session_arg[0]) {
        argv[argc++] = (char *)"--session";
        argv[argc++] = session_arg;
    } else
        argv[argc++] = (char *)"--no-session";
    if (json_get(cfg, "args", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &a)) {
            if (nstore < 8 && argc < 22) {
                json_str(a, arg_store[nstore], sizeof(arg_store[0]));
                argv[argc++] = arg_store[nstore++];
            }
        }
    }
    if (ro && ro->more_args)
        argv[argc++] = (char *)ro->more_args;
    argv[argc] = 0;
    (void)tool;

    ps_reset();
    ps_pool_up(LLM_POOL);
    if (json_get(cfg, "answers", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &a)) {
            char kind[16], ext[24], num[8];
            char *b;
            long bn;
            json_str(a, kind, sizeof(kind));
            cl_ltoa(++nans, num);
            strcpy(ext, ".");
            strcat(ext, num);
            strcat(ext, !strcmp(kind, "http") ? ".http" : ".sse");
            b = slurp_rpc(name, ext, &bn);
            if (!b) {
                printf("  %s: answer %s missing\n", name, ext);
                CHECK(0);
                continue;
            }
            if (!strcmp(kind, "http") && ro && ro->first_http && nans == 1)
                ps_http_text(ro->first_http, (long)strlen(ro->first_http));
            else if (!strcmp(kind, "http"))
                ps_http_text(b, bn);
            else
                ps_sse_text(b, bn, 0);
            free(b);
        }
    }
    memset(&io, 0, sizeof(io));
    io.write = io_write;
    io.ms = io_ms;
    if (!strcmp(mode, "rpc"))
        io.read = io_read;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = proj;
    pl.sys = &sys_;
    pl.tmp = root;
    pl.shell = PI_SH_HOST;
    pl.utc = 1;
    pl.agent_dir = agent;
    pl.data_dir = data;
    pl.ready = ro ? ro->ready : 0;
    if (ro && ro->host) {
        char input[4096];
        long k, rounds = 0, prompts = 0;
        input[0] = 0;
        for (k = 0; k < fd.nline; k++)
            if (strncmp(fd.line[k], "#after ", 7) && strlen(input) + strlen(fd.line[k]) + 2 < sizeof(input)) {
                strcat(input, fd.line[k]);
                strcat(input, "\n");
            }
        i = -1;
        if (host_run(argc, argv, &pl, input, &fd.out, &i, &rounds, &prompts)) {
            printf("  %s: the host run failed\n", name);
            CHECK(0);
        }
        ro->prompts = prompts;
        host_rounds = rounds;
        if (!realpath(proj, real_cwd))      /* the child's getcwd() is the real path */
            strcpy(real_cwd, proj);
        mcwd = real_cwd;
    } else
        i = pi_app(argc, argv, &pl);
    if (ro)
        ro->rc = i;
    (void)p;

    jw_init(&wmask);
    jw_init(&gmask);
    masked_text(want_t, mcwd, &wmask);
    masked_text(fd.out.p ? fd.out.p : "", mcwd, &gmask);
    ok = wmask.p && gmask.p && !strcmp(wmask.p, gmask.p);
    if (ro && ro->nocmp) {
        ro->out = dup_text(fd.out.p ? fd.out.p : "");
        ok = 1;
    } else
        CHECK(ok);
    if (!ok)
        run_diff(name, wmask.p ? wmask.p : "", gmask.p ? gmask.p : "");
    if (ro)
        ro->rounds = ro->host ? host_rounds : pi_agent_rounds - r0;
    jw_free(&wmask);
    jw_free(&gmask);
    jw_free(&fd.out);
    free(in_t);
    free(cfg_t);
    free(want_t);
    sandbox_clear();
    return ok;
}

/* ---- the groups ---- */

static void group(const char *const *names)
{
    int i;
    for (i = 0; names[i]; i++)
        if (!getenv("PI_MODES_ONLY") || !strcmp(getenv("PI_MODES_ONLY"), names[i]))
            scenario(names[i], 0);
}

/* H1: `--mode json` */
static void test_json(void)
{
    static const char *const names[] = { "json_text", "json_tool", "json_error", "json_think", "json_retry", 0 };
    group(names);
}

/* H2: framing, an unknown command and a line that is not JSON answered, the loop going on */
static void test_framing(void)
{
    static const char *const names[] = { "errors", 0 };
    group(names);
}

/* H3a: prompting, state and messages */
static void test_core(void)
{
    static const char *const names[] = { "prompt", "prompt_busy", "steer", "follow_up", "abort", "clear_queue", "queue_idle",
                                         "get_state", "get_messages", 0 };
    group(names);
}

/* H3b: model and thinking level */
static void test_models(void)
{
    static const char *const names[] = { "set_model", "cycle_model", "get_available_models", "set_thinking_level",
                                         "cycle_thinking_level", "cycle_thinking_level_off", "get_available_thinking_levels",
                                         "get_available_thinking_levels_off", 0 };
    group(names);
}

/* H3c: queue modes, compaction, retry */
static void test_modes(void)
{
    static const char *const names[] = { "set_steering_mode", "set_follow_up_mode", "compact", "compact_empty",
                                         "set_auto_compaction", "set_auto_retry", "abort_retry", 0 };
    group(names);
}

/* H3d: bash, stats, export, last text, commands */
static void test_misc(void)
{
    static const char *const names[] = { "bash", "abort_bash", "get_session_stats", "export_html", "get_last_assistant_text",
                                         "get_commands", 0 };
    group(names);
}

/* H3e: sessions */
static void test_sessions(void)
{
    static const char *const names[] = { "new_session", "switch_session", "fork", "clone", "get_fork_messages", "get_entries",
                                         "get_tree", "set_session_name", 0 };
    group(names);
}

/* the 33 commands of pi 1.0.4's rpc-mode.js (its `case` labels): each is in the commands of some golden */
static void test_command_set(void)
{
    static const char *const cmds[] = {
        "prompt", "steer", "follow_up", "abort", "clear_queue", "new_session", "get_state", "set_model", "cycle_model",
        "get_available_models", "set_thinking_level", "cycle_thinking_level", "get_available_thinking_levels",
        "set_steering_mode", "set_follow_up_mode", "compact", "set_auto_compaction", "set_auto_retry", "abort_retry", "bash",
        "abort_bash", "get_session_stats", "export_html", "switch_session", "fork", "clone", "get_fork_messages",
        "get_entries", "get_tree", "get_last_assistant_text", "set_session_name", "get_messages", "get_commands", 0
    };
    static const char *const scen[] = { "prompt", "prompt_busy", "steer", "follow_up", "abort", "clear_queue", "queue_idle",
                                        "get_state", "get_messages", "set_model", "cycle_model", "get_available_models",
                                        "set_thinking_level", "cycle_thinking_level", "get_available_thinking_levels",
                                        "set_steering_mode", "set_follow_up_mode", "compact", "set_auto_compaction",
                                        "set_auto_retry", "abort_retry", "bash", "abort_bash", "get_session_stats", "export_html",
                                        "get_last_assistant_text", "get_commands", "new_session", "switch_session", "fork",
                                        "clone", "get_fork_messages", "get_entries", "get_tree", "set_session_name", 0 };
    int i, j, hit = 0;
    for (i = 0; cmds[i]; i++) {
        int found = 0;
        char pat[80];
        strcpy(pat, "\"type\":\"");
        strcat(pat, cmds[i]);
        strcat(pat, "\"");
        for (j = 0; scen[j] && !found; j++) {
            char *t = slurp_rpc(scen[j], ".in", 0);
            if (t && strstr(t, pat))
                found = 1;
            free(t);
        }
        if (!found)
            printf("  command %s has no golden exchange\n", cmds[i]);
        CHECK(found);
        hit += found;
    }
    CHECK_INT(i, 33);
    CHECK_INT(hit, 33);
}

/* H5: the extension UI over RPC. The test extension asks the four questions pi's TS extension of the golden asks (select,
 * confirm, input, notify) on a tool call; pi_rpc_ui() is the call an extension makes */
static int ext_tool_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    pi_rpc *r = pi_rpc_ui();
    static const char *const opt[] = { "Allow", "Block" };
    char choice[40], why[60], msg[200];
    int has_choice, has_why, yes;
    (void)u;
    (void)id;
    (void)name;
    (void)args;
    if (!r)
        return 0;
    has_choice = pi_rpc_select(r, "Run it?", opt, 2, 0, choice, sizeof(choice));
    yes = pi_rpc_confirm(r, "Sure?", "It runs a command.", 0);
    has_why = pi_rpc_input(r, "Why?", "a reason", 0, why, sizeof(why));
    strcpy(msg, "decided ");
    strcat(msg, has_choice ? choice : "undefined");
    strcat(msg, yes ? " true " : " false ");
    strcat(msg, has_why ? why : "undefined");
    pi_rpc_notify(r, msg, "info");
    if (!has_choice || strcmp(choice, "Allow")) {
        cl_copy(reason, "Blocked by the user", cap);
        return 1;
    }
    return 0;
}

static pi_ext ui_ext = { "ui-test", 0, 0, ext_tool_call, 0 };
static long ui_ready_calls;

static void ui_ready(pi_session *s)
{
    ui_ready_calls++;
    pi_ext_add(&s->ext, &ui_ext);
}

static void test_ui(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.ready = ui_ready;
    CHECK(scenario("ui_dialogs", &ro));
    memset(&ro, 0, sizeof(ro));
    ro.ready = ui_ready;
    CHECK(scenario("ui_cancel", &ro));
    CHECK(ui_ready_calls >= 2);
}

static int has(const char *text, const char *needle)
{
    return text && strstr(text, needle) != 0;
}

/* abort_retry while the agent waits to retry: the retry ends cancelled, the run settles, the second answer is not asked for.
 * (The golden of abort_retry is the command with nothing to abort.) */
static void test_retry_cancel(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\"}\n#after auto_retry_start\n"
                   "{\"id\":\"2\",\"type\":\"abort_retry\"}\n";
    scenario("json_retry", &ro);
    CHECK(has(ro.out, "\"type\":\"auto_retry_start\",\"attempt\":1"));
    CHECK(has(ro.out, "\"type\":\"auto_retry_end\",\"success\":false,\"attempt\":1,\"finalError\":\"Retry cancelled\""));
    CHECK(has(ro.out, "\"command\":\"abort_retry\",\"success\":true"));
    CHECK(has(ro.out, "\"type\":\"agent_settled\""));
    CHECK(!has(ro.out, "Recovered."));
    CHECK_INT(ps.nreq, 1);
    free(ro.out);
}

/* HF2: settings retry.maxAgentDelayMs caps the wait before a retry (pi-ai retryDelayMs: min(base * 2^(attempt - 1), the cap)) */
static void test_retry_cap(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.extra_retry = ",\"maxAgentDelayMs\":40";
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\"}\n#after agent_settled\n";
    scenario("json_retry", &ro);
    CHECK(has(ro.out, "\"type\":\"auto_retry_start\",\"attempt\":1,\"maxAttempts\":2,\"delayMs\":40,"));
    CHECK(has(ro.out, "Recovered."));
    free(ro.out);
    /* not set: the base delay of the settings stands (the golden json_retry) */
    memset(&ro, 0, sizeof(ro));
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\"}\n#after agent_settled\n";
    scenario("json_retry", &ro);
    CHECK(has(ro.out, "\"type\":\"auto_retry_start\",\"attempt\":1,\"maxAttempts\":2,\"delayMs\":100,"));
    free(ro.out);
}

/* HF2: an answer that failed because the request outgrew the window (HTTP 500 from llama.cpp's server, whose text 500
 * would have it retried) is not retried: one request, no auto_retry events, the run settles (the compaction check takes it
 * as pi does); the same status with another text is retried */
static void test_retry_overflow(void)
{
    static const char body[] = "{\"error\":{\"message\":\"the request exceeds the available context size, try increasing it\"}}";
    char http[400], len[16];
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    cl_ltoa((long)strlen(body), len);
    strcpy(http, "HTTP/1.1 500 X\r\ncontent-type: application/json\r\ncontent-length: ");
    strcat(http, len);
    strcat(http, "\r\n\r\n");
    strcat(http, body);
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.first_http = http;
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\"}\n#after agent_settled\n";
    scenario("json_retry", &ro);
    CHECK(has(ro.out, "exceeds the available context size"));
    CHECK(!has(ro.out, "auto_retry_start"));
    CHECK(has(ro.out, "\"type\":\"agent_settled\""));
    CHECK_INT(ps.nreq, 1);
    free(ro.out);
}

/* the permission gate over RPC: its question is a select; No blocks the call, Yes runs it */
static void test_gate(void)
{
    static const char *const answer[] = { "No", "Yes" };
    int i;
    for (i = 0; i < 2; i++) {
        run_opts ro;
        char in[600];
        memset(&ro, 0, sizeof(ro));
        ro.nocmp = 1;
        ro.more_args = "--gate";
        ro.extra_settings = "\"permissions\":{\"ask\":[\"Read\"]}";
        strcpy(in, "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"read the file\"}\n#after extension_ui_request\n"
                   "{\"type\":\"extension_ui_response\",\"id\":\"$ui:1\",\"value\":\"");
        strcat(in, answer[i]);
        strcat(in, "\"}\n");
        ro.inline_in = in;
        scenario("prompt_tool", &ro);
        CHECK(has(ro.out, "\"method\":\"select\",\"title\":\"Allow read"));
        CHECK(has(ro.out, "\"options\":[\"Yes\",\"Yes, for this session\""));
        CHECK(has(ro.out, "\"type\":\"agent_settled\""));
        CHECK(i ? has(ro.out, "one\\ntwo\\nthree") : !has(ro.out, "one\\ntwo\\nthree"));
        free(ro.out);
    }
}

/* H7: the reachability test, through the host binary's real stdin path (HF3): an RPC `prompt` line on the stdin of
 * pi_host_run() -- main_posix.c's program, its reader, its stdout -- drives pi_prompt() (called once, the sentinel
 * pi_agent_prompts; two rounds of the loop: the read call, then the answer), and the stream written equals the golden's */
static void test_reach(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.host = 1;
    CHECK(scenario("prompt_tool", &ro));
    CHECK_INT(ro.rc, 0);
    CHECK_INT(ro.prompts, 1);
    CHECK_INT(ro.rounds, 2);
}

/* H6: the C SDK. tests/pi/sdk_example.c is a program on piapi.h; here it runs over the stub: two prompts, a subscriber, a
 * steering message queued from it */
int sdk_example(const pi_platform *pl, int argc, char **argv, char *answer, long cap, long counts[3]);

static void test_sdk(void)
{
    static const char *const files[] = { "prompt_tool.1.sse", "prompt_tool.2.sse", "prompt.1.sse", 0 };
    char *argv[6], answer[200], cfgt[] = "{\"model\":\"fake-model\",\"thinking\":\"off\",\"retry\":false,\"settings\":{},\"files\":{}}";
    char session_arg[8];
    long counts[3], r0 = pi_agent_rounds;
    pi_platform pl;
    cl_io io;
    jv cfg;
    int i, rc;
    sandbox();
    CHECK(!json_parse(cfgt, (long)strlen(cfgt), &cfg));
    CHECK_INT(setup_files(cfg, session_arg, sizeof(session_arg)), 0);
    ps_reset();
    ps_pool_up(LLM_POOL);
    for (i = 0; files[i]; i++) {
        long bn;
        char *b = slurp_rpc(files[i], "", &bn);
        CHECK(b != 0);
        if (b)
            ps_sse_text(b, bn, 0);
        free(b);
    }
    memset(&fd, 0, sizeof(fd));
    jw_init(&fd.out);
    memset(&io, 0, sizeof(io));
    io.write = io_write;
    io.ms = io_ms;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = proj;
    pl.sys = &sys_;
    pl.tmp = root;
    pl.shell = PI_SH_HOST;
    pl.utc = 1;
    pl.agent_dir = agent;
    pl.data_dir = data;
    argv[0] = (char *)"sdk";
    argv[1] = (char *)"--model";
    argv[2] = (char *)"fake/fake-model";
    argv[3] = (char *)"--no-session";
    argv[4] = 0;
    rc = sdk_example(&pl, 4, argv, answer, sizeof(answer), counts);
    CHECK_INT(rc, 0);
    CHECK(counts[0] > 20);
    CHECK_INT(counts[2], 1);
    CHECK(counts[1] >= 6);
    CHECK_STR(answer, "Hello from the fixture.");
    CHECK_INT(pi_agent_rounds - r0, 3);
    CHECK(ps.nreq > 1 && strstr(ps.body[1], "and keep it short") != 0);
    CHECK(ps.nreq > 0 && strstr(ps.body[0], "and keep it short") == 0);
    CHECK(fd.out.n == 0);       /* a library prints nothing of its own */
    jw_free(&fd.out);
    sandbox_clear();
}

/* the retry classifier (pi-ai isRetryableAssistantError) */
static void test_retryable(void)
{
    CHECK(pi_retryable("529 {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}"));
    CHECK(pi_retryable("Connection error."));
    CHECK(pi_retryable("request timed out"));
    CHECK(pi_retryable("Rate limit reached"));
    CHECK(pi_retryable("terminated"));
    CHECK(pi_retryable("fetch failed"));
    CHECK(!pi_retryable("400 {\"error\":\"bad request\"}"));
    CHECK(!pi_retryable("429 insufficient_quota: you exceeded your current quota, billing"));
    CHECK(!pi_retryable("quota exceeded"));
    CHECK(!pi_retryable(""));
    CHECK(!pi_retryable(0));
}

/* HF2: pi-ai's isContextOverflow on error texts; a throttling text that mentions tokens is not one */
static void test_overflow(void)
{
    CHECK(pi_context_overflow("prompt is too long: 213462 tokens > 200000 maximum"));
    CHECK(pi_context_overflow("413 {\"error\":{\"type\":\"request_too_large\"}}"));
    CHECK(pi_context_overflow("Your input exceeds the context window of this model"));
    CHECK(pi_context_overflow("Requested token count exceeds the model's maximum context length of 131072 tokens"));
    CHECK(pi_context_overflow("Input length (265330) exceeds model's maximum context length (262144)."));
    CHECK(pi_context_overflow("The input token count (1196265) exceeds the maximum number of tokens allowed (1048575)"));
    CHECK(pi_context_overflow("This model's maximum prompt length is 131072 but the request contains 537812 tokens"));
    CHECK(pi_context_overflow("This endpoint's maximum context length is 8192 tokens. However, you requested about 9000 tokens"));
    CHECK(pi_context_overflow("The input (9,000 tokens) is longer than the model's context length (8192 tokens)."));
    CHECK(pi_context_overflow("the request exceeds the available context size, try increasing it"));
    CHECK(pi_context_overflow("Prompt has 9000 tokens, but the configured context size is 8192 tokens"));
    CHECK(pi_context_overflow("400 context_length_exceeded"));
    CHECK(!pi_context_overflow("Throttling error: Too many tokens, please wait before trying again."));
    CHECK(!pi_context_overflow("429 rate limit: too many tokens per minute"));
    CHECK(!pi_context_overflow("529 overloaded_error"));
    CHECK(!pi_context_overflow("Connection error."));
    CHECK(!pi_context_overflow(""));
    CHECK(!pi_context_overflow(0));
}

void suite_pi_modes(void)
{
    CHECK(load_masks());
    test_retryable();
    test_overflow();
    test_command_set();
    test_json();
    test_framing();
    test_core();
    test_models();
    test_modes();
    test_misc();
    test_sessions();
    test_ui();
    test_reach();
    test_sdk();
    test_retry_cancel();
    test_retry_cap();
    test_retry_overflow();
    test_gate();
}
