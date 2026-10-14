/* amiga-pi's JSON and RPC modes against pi 1.0.4's (plan H1-H7): pi_app() with `--mode json` and `--mode rpc` over the
 * stub transport, the answers pi was given (tests/pi/rpc/<name>.N.sse|http), the commands of the real run
 * (<name>.in) fed on the console's input, and every line written compared with the line pi wrote (<name>.out). The
 * goldens and the environment of each scenario (<name>.cfg.json) come from tools/pi_oracle/rpc_gen.py.
 *
 * A line is compared after the masks of tests/pi/rpc/masks.json (the one list of this test and of vtcon's rig
 * tools/rig/amigapi_rig.py), each with its reason there and a COMPATIBILITY.md row (group pi-modes), never a way to
 * hide one: keys starred (made per run, or compared where they live), fresh ids starred, and the rules: cost `total`, the text of an HTTP errorMessage, message_start usage, baseUrl, the
 * parse error text, get_available_models for the fixture's provider.
 * A command that arrives while the agent works is held by its `#after <event>` line until pi's run had that event in its
 * output, as the golden was made. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "pi_test.h"
#include "claude/util.h"
#include "pi_stub.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/ext_hooks.h"
#include "../amiga-pi/ext_lua.h"
#include "../amiga-pi/host_posix.h"
#include "../amiga-pi/mode_json.h"
#include "../amiga-pi/mode_rpc.h"
#include "../amiga-pi/session3.h"
#include "claude/sys_posix.h"

#define DIR_RPC PI_TESTS "/pi/rpc/"
#define GOLD_CWD "/private/tmp/pirpc/project"
#define GOLD_AGENT "/private/tmp/pirpc/agent"
static const char *mask_agent = "";   /* this run's agent directory (a path in a record: get_commands' sourceInfo) */

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

static void mkdirs(const char *path)
{
    char b[600];
    char *p;
    cl_copy(b, path, sizeof(b));
    for (p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0777);
            *p = '/';
        }
    mkdir(b, 0777);
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
static key_list star_keys, id_keys;
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
    if (!t || json_parse(t, n, &v) || !json_get(v, "star", &x)) {
        free(t);
        return 0;
    }
    keys_of(x, &star_keys);
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
#define F_OVERFLOW 4            /* the result of an overflow compaction_end */

static void m_val(jv v, int flags, jw *o, const char *cwd);

static int str_is(jv obj, const char *key, const char *want);

/* a get_commands row of pi's built-in extensions (mcp, llama: sourceInfo.source "builtin"): amiga-pi has none, the rows are left out
 * on both sides (masks.json, rules) */
static int builtin_command_row(jv e)
{
    jv si;
    return json_type(e) == J_OBJ && str_is(e, "source", "extension") && json_get(e, "sourceInfo", &si) &&
           json_type(si) == J_OBJ && str_is(si, "source", "builtin");
}

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
        } else if (mask_agent[0] && i + (long)strlen(mask_agent) <= n && !memcmp(z + i, mask_agent, strlen(mask_agent))) {
            jw_rawz(&t, "<agent>");
            i += (long)strlen(mask_agent);
        } else if (i + (long)strlen(GOLD_AGENT) <= n && !memcmp(z + i, GOLD_AGENT, strlen(GOLD_AGENT))) {
            jw_rawz(&t, "<agent>");
            i += (long)strlen(GOLD_AGENT);
        } else
            jw_raw(&t, z + i++, 1);
    }
    /* an extension is a .ts file for pi and a .lua file here, in the same place (the file's path in a sourceInfo) */
    if (t.p && strstr(t.p, "/extensions/") && t.n > 3 && !strcmp(t.p + t.n - 3, ".ts")) {
        t.n -= 3;
        jw_rawz(&t, ".<ext>");
    } else if (t.p && strstr(t.p, "/extensions/") && t.n > 4 && !strcmp(t.p + t.n - 4, ".lua")) {
        t.n -= 4;
        jw_rawz(&t, ".<ext>");
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
    jv wr;
    int first = 1;
    int assistant = str_is(v, "role", "assistant"), retry_start = str_is(v, "type", "auto_retry_start");
    /* an overflow compaction that asks again followed the omission of the failed answer (a context_edit): pi counts it
     * by estimate alone (estimateProjectedContextTokens), its system message included; a silent overflow is usage-based */
    int overflow_end = str_is(v, "type", "compaction_end") && str_is(v, "reason", "overflow") &&
                       json_get(v, "willRetry", &wr) && json_type(wr) == J_TRUE;
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
        if ((flags & F_STARTMSG) && (!strcmp(key, "responseId") || !strcmp(key, "responseModel")))
            continue;           /* set by message_start: in pi's shared partial or not, as its consumer runs */
        if (!first)
            jw_rawz(o, ",");
        first = 0;
        jw_strz(o, key);
        jw_rawz(o, ":");
        if (in_list(key, &star_keys) || ((flags & F_OVERFLOW) && !strcmp(key, "tokensBefore")) ||
            (!strcmp(key, "payload") && str_is(v, "type", "before_provider_request")) ||
            (!strcmp(key, "headers") && str_is(v, "type", "after_provider_response")))
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
        } else if (in_list(key, &id_keys) && json_type(val) == J_ARR) {
            /* a list of ids (turn_end's toolResultEntryIds) */
            jit ai;
            jv e;
            int f2 = 1;
            jw_rawz(o, "[");
            json_iter(val, &ai);
            while (json_next(&ai, 0, &e)) {
                char id[64];
                if (!f2)
                    jw_rawz(o, ",");
                f2 = 0;
                id[0] = 0;
                if (json_type(e) == J_STR)
                    json_str(e, id, sizeof(id));
                if (id[0] && fresh_id(id))
                    jw_rawz(o, "\"*\"");
                else
                    m_val(e, 0, o, cwd);
            }
            jw_rawz(o, "]");
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
        else if (!strcmp(key, "result") && overflow_end)
            m_val(val, F_OVERFLOW, o, cwd);
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
            if (builtin_command_row(e))
                continue;
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
            if (timeout_ms > 0)
                ps.clock += (unsigned long)timeout_ms;      /* a read that waited and got nothing took its time: a dialog's timeout runs out (L5) */
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
    if (json_get(cfg, "provider_headers", &v) && json_type(v) == J_OBJ) {
        /* the provider's "headers" (models.json): put in front of its baseUrl */
        char *at = strstr(models, "\"baseUrl\"");
        if (at) {
            jw nm;
            jw_init(&nm);
            jw_raw(&nm, models, (long)(at - models));
            jw_rawz(&nm, "\"headers\":");
            jw_raw(&nm, v.p, v.n);
            jw_rawz(&nm, ",");
            jw_raw(&nm, at, n - (long)(at - models));
            put_file(path, nm.p, nm.n);
            jw_free(&nm);
        } else
            put_file(path, models, n);
    } else
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
    {
        jv rs, bd;
        long base = 100;            /* a scenario's settings.retry.baseDelayMs (a wait long enough to abort in) replaces it */
        if (json_get(cfg, "settings", &s) && json_get(s, "retry", &rs) && json_get(rs, "baseDelayMs", &bd))
            base = json_long(bd, 100);
        jw_rawz(&st, ",\"maxRetries\":2,\"baseDelayMs\":");
        jw_long(&st, base);
    }
    if (extra_retry)
        jw_rawz(&st, extra_retry);
    jw_rawz(&st, "},\"quietStartup\":true");
    if (json_get(cfg, "settings", &s) && json_type(s) == J_OBJ) {
        json_iter(s, &it);
        while (json_next(&it, &v, &e)) {
            json_str(v, key, sizeof(key));
            if (!strcmp(key, "retry"))      /* the scenario's own retry object (a long delay to abort in) replaces the default */
                continue;
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
    if (json_get(cfg, "text_files", &v) && json_type(v) == J_OBJ) {
        /* J: prompt templates; the name is under agent/ or project/, the value the file's text */
        json_iter(v, &fi);
        while (json_next(&fi, &s, &e)) {
            char name[100], *body;
            long bn;
            json_str(s, name, sizeof(name));
            body = json_strdup(e, &bn);
            if (!body)
                return -1;
            strcpy(path, !strncmp(name, "agent/", 6) ? agent : proj);
            strcat(path, "/");
            strcat(path, strchr(name, '/') + 1);
            {
                char dir[600], *sl;
                strcpy(dir, path);
                sl = strrchr(dir, '/');
                *sl = 0;
                mkdirs(dir);
            }
            put_file(path, body, bn);
            free(body);
        }
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
    const char *more_argv[4];       /* further arguments after more_args, 0 ends the list */
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
    int bad_models;                 /* models.json is not JSON: the warning of the start, a diagnostic */
    int diag;                       /* capture the diagnostic stream (pl.diag) in diag_log: one entry per call, '|' between */
    int nocmp;                      /* do not compare with the golden: the caller reads the output */
    int refuse;                     /* the first this many connects fail (nothing listening), the answers after them */
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

/* the diagnostic stream of a run with ro->diag: the lines pl.diag was given, '|' between them */
static char diag_log[1024];

static void diag_capture(const char *line)
{
    if (strlen(diag_log) + strlen(line) + 2 < sizeof(diag_log)) {
        if (diag_log[0])
            strcat(diag_log, "|");
        strcat(diag_log, line);
    }
}

/* the scenario `name` run and compared with its golden; ro->inline_in replaces the golden's commands */
/* L3: a Lua extension records the events it is handed with print (a notice), through json.encode, whose objects have
 * sorted keys; pi's dump has the keys in its own order. A scenario with "canonical" compares the two after both are
 * rewritten with sorted keys; "lua_dump" names the file the notices go to (in the project directory). */
typedef struct ckey {
    char name[64];
    jv v;
} ckey;

static int ckey_cmp(const void *a, const void *b)
{
    return strcmp(((const ckey *)a)->name, ((const ckey *)b)->name);
}

static void canon_val(jv v, jw *o)
{
    if (json_type(v) == J_OBJ) {
        ckey *ks = (ckey *)calloc((size_t)json_count(v) + 1, sizeof(ckey));
        jit it;
        jv k, e;
        long n = 0, i;
        json_iter(v, &it);
        while (ks && json_next(&it, &k, &e)) {
            if (json_type(e) == J_NULL)
                continue;       /* a Lua table has no null member (json.decode reads it as nil): both sides leave it out */
            json_str(k, ks[n].name, sizeof(ks[n].name));
            ks[n++].v = e;
        }
        if (ks)
            qsort(ks, (size_t)n, sizeof(ckey), ckey_cmp);
        jw_rawz(o, "{");
        for (i = 0; i < n; i++) {
            if (i)
                jw_rawz(o, ",");
            jw_strz(o, ks[i].name);
            jw_rawz(o, ":");
            canon_val(ks[i].v, o);
        }
        jw_rawz(o, "}");
        free(ks);
    } else if (json_type(v) == J_ARR) {
        jit it;
        jv e;
        int first = 1;
        jw_rawz(o, "[");
        json_iter(v, &it);
        while (json_next(&it, 0, &e)) {
            if (!first)
                jw_rawz(o, ",");
            first = 0;
            canon_val(e, o);
        }
        jw_rawz(o, "]");
    } else
        jw_raw(o, v.p, v.n);
}

static char *canon_text(const char *text)
{
    jw o;
    const char *p = text;
    jw_init(&o);
    while (*p) {
        const char *e = strchr(p, '\n');
        long n = e ? (long)(e - p) : (long)strlen(p);
        jv v;
        if (n > 0) {
            if (json_parse(p, n, &v))
                jw_raw(&o, p, n);
            else
                canon_val(v, &o);
            jw_rawz(&o, "\n");
        }
        p += n + (e ? 1 : 0);
    }
    return o.p;
}

/* L4: the tools and the messages of request N that amiga-pi sent, against pi's (<name>.req.N.json), both with sorted keys:
 * a schema written as a Lua table has no key order. A request without tools has no "tools" in ours; pi's file has []. */
/* the headers an extension set or took out (x-test, x-gone): what pi's server saw in each request (.xtest) and what ours was sent */
static const char *head_value(const char *head, const char *name)
{
    static char v[80];
    const char *p = head;
    size_t nl = strlen(name);
    while (p && *p) {
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            size_t k = 0;
            p += nl + 1;
            while (*p == ' ')
                p++;
            while (*p && *p != '\r' && *p != '\n' && k < sizeof(v) - 1)
                v[k++] = *p++;
            v[k] = 0;
            return v;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return "-";
}

static int compare_xtest(const char *name)
{
    char *want;
    long wn;
    int k = 0, ok = 1;
    const char *p;
    want = slurp_rpc(name, ".xtest", &wn);
    if (!want)
        return 1;
    for (p = want; *p; k++) {
        char line[160], t[80], g[80];
        const char *e = strchr(p, '\n');
        long ln = e ? (long)(e - p) : (long)strlen(p);
        if (ln >= (long)sizeof(line))
            ln = (long)sizeof(line) - 1;
        memcpy(line, p, (size_t)ln);
        line[ln] = 0;
        if (k >= ps.nreq) {
            ok = 0;
            CHECK(0);
            break;
        }
        cl_copy(t, head_value(ps.head[k], "x-test"), sizeof(t));
        cl_copy(g, head_value(ps.head[k], "x-gone"), sizeof(g));
        {
            char got[200];
            cl_copy(got, "t=", sizeof(got));
            cl_cat(got, t, sizeof(got));
            cl_cat(got, " g=", sizeof(got));
            cl_cat(got, g, sizeof(got));
            CHECK_STR(got, line);
            if (strcmp(got, line))
                ok = 0;
        }
        p += ln + 1;
        if (!e)
            break;
    }
    free(want);
    return ok;
}

static int compare_requests(const char *name)
{
    int k, ok = 1;
    for (k = 1;; k++) {
        char ext[40], num[8];
        char *want;
        long wn;
        jv wv, gv, w, g;
        int f;
        cl_ltoa(k, num);
        strcpy(ext, ".req.");
        strcat(ext, num);
        strcat(ext, ".json");
        want = slurp_rpc(name, ext, &wn);
        if (!want)
            break;
        if (k > ps.nreq || json_parse(want, wn, &wv) || json_parse(ps.body[k - 1], (long)strlen(ps.body[k - 1]), &gv)) {
            printf("  %s: request %d missing or not JSON\n", name, k);
            CHECK(0);
            ok = 0;
            free(want);
            break;
        }
        for (f = 0; f < 2; f++) {
            const char *key = f ? "messages" : "tools";
            jw cw, cg;
            int same;
            jw_init(&cw);
            jw_init(&cg);
            if (json_get(wv, key, &w))
                canon_val(w, &cw);
            if (json_get(gv, key, &g))
                canon_val(g, &cg);
            else
                jw_rawz(&cg, "[]");
            same = cw.p && cg.p && !strcmp(cw.p, cg.p);
            CHECK(same);
            if (!same) {
                printf("  %s: request %d, %s differ\n   pi:  %.600s\n   ours: %.600s\n", name, k, key, cw.p ? cw.p : "", cg.p ? cg.p : "");
                ok = 0;
            }
            jw_free(&cw);
            jw_free(&cg);
        }
        free(want);
    }
    return ok;
}

static char lua_dump_file[80];

static void lua_dump_notice(void *u, const char *text)
{
    char path[600];
    FILE *f;
    (void)u;
    strcpy(path, proj);
    strcat(path, "/");
    strcat(path, lua_dump_file);
    f = fopen(path, "ab");
    if (f) {
        fputs(text, f);
        fputc('\n', f);
        fclose(f);
    }
}

static void lua_dump_ready(pi_session *s)
{
    s->h.notice = lua_dump_notice;
}

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
    char arg_store[8][300], real_cwd[PATH_MAX], gold_name[80];
    const char *mcwd = proj;
    long host_rounds = 0;
    int nstore = 0;
    jit it;
    jv a;

    cfg_t = slurp_rpc(name, ".cfg.json", &cn);
    if (cfg_t && !json_parse(cfg_t, cn, &cfg) && json_get(cfg, "golden", &v) && json_type(v) == J_STR) {
        /* L3: a scenario that runs another one's golden (pi's records of the same behaviour) with its own extension kind */
        json_str(v, gold_name, sizeof(gold_name));
        name = gold_name;
    }
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
    if (ro && ro->bad_models) {
        char mpath[600];
        strcpy(mpath, agent);
        strcat(mpath, "/models.json");
        put_file(mpath, "{not json", 9);
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
    for (i = 0; ro && i < 4 && ro->more_argv[i]; i++)
        argv[argc++] = (char *)ro->more_argv[i];
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
    if (ro)
        ps.refuse = ro->refuse;
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
    lua_dump_file[0] = 0;
    if (json_get(cfg, "lua_dump", &v) && json_type(v) == J_STR) {
        json_str(v, lua_dump_file, sizeof(lua_dump_file));
        pl.ready = lua_dump_ready;
    }
    if (ro && ro->diag) {
        diag_log[0] = 0;
        pl.diag = diag_capture;
    }
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
    mask_agent = agent;
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
    if (json_get(cfg, "requests", &v) && json_type(v) == J_TRUE && !compare_requests(name))
        ok = 0;
    if (json_get(cfg, "xtest", &v) && json_type(v) == J_TRUE && !compare_xtest(name))
        ok = 0;
    /* the files the scenario's extension wrote in the project directory (pi) and its hook commands wrote (amiga-pi): the
     * same masks on both, line by line (L2) */
    if (json_get(cfg, "collect", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &a)) {
            char file[80], gp[600], suffix[100];
            char *want_f, *got_f;
            long wfn, gfn;
            json_str(a, file, sizeof(file));
            strcpy(suffix, ".");
            strcat(suffix, file);
            want_f = slurp_rpc(name, suffix, &wfn);
            strcpy(gp, proj);
            strcat(gp, "/");
            strcat(gp, file);
            got_f = slurp(gp, &gfn);
            if (!want_f || !got_f) {
                printf("  %s: %s missing (%s)\n", name, file, want_f ? "run" : "golden");
                CHECK(0);
            } else {
                jw wm, gm;
                jw_init(&wm);
                jw_init(&gm);
                if (json_get(cfg, "canonical", &v) && json_type(v) == J_TRUE) {
                    char *cw = canon_text(want_f), *cg = canon_text(got_f);
                    masked_text(cw ? cw : "", mcwd, &wm);
                    masked_text(cg ? cg : "", mcwd, &gm);
                    free(cw);
                    free(cg);
                } else {
                    masked_text(want_f, mcwd, &wm);
                    masked_text(got_f, mcwd, &gm);
                }
                if (!wm.p || !gm.p || strcmp(wm.p, gm.p)) {
                    CHECK(0);
                    run_diff(name, wm.p ? wm.p : "", gm.p ? gm.p : "");
                    ok = 0;
                } else
                    CHECK(1);
                jw_free(&wm);
                jw_free(&gm);
            }
            free(want_f);
            free(got_f);
        }
    }
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
                                         "set_auto_compaction", "set_auto_retry", "abort_retry", "abort_in_retry", "abort_retry_wait",
                                         "abort_in_compaction", 0 };
    group(names);
}

/* H3d: bash, stats, export, last text, commands */
static void test_misc(void)
{
    static const char *const names[] = { "bash", "abort_bash", "get_session_stats", "export_html", "get_last_assistant_text",
                                         "get_commands", "bash_no_tools", 0 };
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

/* H5, L5: the extension UI over RPC. A Lua extension asks the questions pi's TS extension of the golden asks (select, confirm,
 * input, notify; ui_dialogs and ui_cancel answer them, ext_ui_timeout lets two run out and answers one too late, ext_ui_notify
 * sends the three types and none, ext_ui_print has no client at all) and ctx.ui reaches the client's extension_ui_request /
 * extension_ui_response (mode_rpc.c's pi_ext_ui) */
static void test_ui(void)
{
    static const char *const names[] = { "ext_lua_ui_dialogs", "ext_lua_ui_cancel", "ext_lua_ui_timeout", "ext_lua_ui_notify",
                                         "ext_lua_ui_print", 0 };
    group(names);
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

/* P11-I5: the session entries carry durationMs (pi 1.1.0) on the assistant and the toolResult message, as the records of
 * the run do: the run's answer and its tool call, then the entries read back */
static void test_duration_entries(void)
{
    run_opts ro;
    const char *e;
    memset(&ro, 0, sizeof(ro));
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"read the file\"}\n#after agent_settled\n"
                   "{\"id\":\"2\",\"type\":\"get_entries\"}\n";
    scenario("json_tool", &ro);
    e = strstr(ro.out, "\"command\":\"get_entries\"");
    CHECK(e != 0);
    if (e) {
        const char *tr = strstr(e, "\"role\":\"toolResult\"");
        const char *as = strstr(e, "\"role\":\"assistant\"");
        CHECK(tr && strstr(tr, "\"isError\":false,\"durationMs\":"));
        CHECK(as && strstr(as, "\"rawStopReason\":\"tool_use\",\"durationMs\":"));
    }
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

/* N1: nothing listening on the base URL (pi 1.1.0, http://127.0.0.1:9/v1: "Connection error.", retried 2, 4, 8 s, then
 * the error; agent-session.ts _handleRetryableError): every attempt refused, the retries run out (maxRetries 2) and the run
 * settles on the error. Giving up resets the retry count, so the run must not look at it again: it started the retries
 * over without end (amiga-pi hung > 10 min on the Amiga). The scenario's answers come after three refusals, so the old
 * loop's fourth attempt reached them instead of hanging this test. */
static void test_retry_refused(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.force_mode = "rpc";
    ro.nocmp = 1;
    ro.refuse = 3;
    ro.inline_in = "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\"}\n#after agent_settled\n";
    scenario("json_retry", &ro);
    CHECK(has(ro.out, "\"type\":\"auto_retry_start\",\"attempt\":1,\"maxAttempts\":2,\"delayMs\":100,\"errorMessage\":\"Connection error.\""));
    CHECK(has(ro.out, "\"type\":\"auto_retry_start\",\"attempt\":2,\"maxAttempts\":2,\"delayMs\":200,"));
    CHECK(has(ro.out, "\"type\":\"auto_retry_end\",\"success\":false,\"attempt\":2,\"finalError\":\"Connection error.\""));
    CHECK(has(ro.out, "\"type\":\"agent_settled\""));
    CHECK(!has(ro.out, "Recovered."));
    CHECK_INT(ps.refused, 3);
    CHECK_INT(ps.nreq, 0);
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

/* HF6: context-overflow recovery, pi's _checkCompaction through pi_prompt (the goldens overflow and overflow_twice are pi's
 * records of the same scripted answers: the third prompt's request outgrew the window). pi omits the failed answer
 * (entry_appended), compacts (compaction_start / compaction_end reason overflow, willRetry true) and asks again, once; a
 * second overflow ends the run with compaction_end's error and no third request. overflow_silent: an answer that succeeded
 * with more input than the window is compacted and kept, not asked again; overflow_length: a length stop below the output
 * limit is omitted, compacted and asked again. overflow_tool: the retried answer calls a tool, which gives the allowance
 * back (pi clears _overflowRecoveryAttempted at a clean answer), so the run's next overflow recovers again;
 * overflow_twice_next: the check before the next prompt tells the failure again, before the prompt's response */
static void test_overflow_recovery(void)
{
    static const char *const names[] = { "overflow", "overflow_twice", "overflow_silent", "overflow_length", "overflow_tool",
                                         "overflow_twice_next", 0 };
    long r0;
    group(names);
    if (getenv("PI_MODES_ONLY"))
        return;
    /* five requests each: the two answers, the overflowing one, the summary, the retry -- not a sixth */
    r0 = pi_agent_rounds;
    {
        run_opts ro;
        memset(&ro, 0, sizeof(ro));
        ro.nocmp = 1;
        scenario("overflow_twice", &ro);
        CHECK_INT(ps.nreq, 5);
        CHECK(has(ro.out, "\"reason\":\"overflow\",\"aborted\":false,\"willRetry\":false,\"errorMessage\":\"Context overflow recovery failed after one"));
        free(ro.out);
    }
    CHECK_INT(pi_agent_rounds - r0, 4);
}

/* Q6b, R1: the agent's bash call emits pi's tool_execution_update records (bash.ts onUpdate): an empty one when the
 * command starts, then the output so far for each chunk (details {}); the stream equals the oracle's golden (pi 1.1.0).
 * prompt_bash: one chunk; prompt_bash_chunks: two chunks 0.6 s apart, three records (the old end-only update gave two) */
static void test_tool_update(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    CHECK(scenario("prompt_bash", &ro));
    CHECK(scenario("prompt_bash_chunks", &ro));
}

/* J3: prompt templates through RPC mode against pi 1.1.0's records: get_commands lists them (the project's first, its
 * name wins, descriptions from the frontmatter or the first line cut at 60, sourceInfo user/project), "/name args"
 * becomes the template's body in the user message (the request carries the same), an unknown /name goes as typed;
 * prompt_template_path: --no-prompt-templates --prompt-template extra (a directory read whole, source cli) */
static void test_templates_rpc(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    CHECK(scenario("prompt_templates", &ro));
    CHECK(scenario("prompt_template_path", &ro));
}

/* Q16: pi writes a diagnostic as one line; amiga-pi's say() wrote "amiga-pi: " and the message in two writes, which json
 * and rpc mode send to the diagnostic stream as two lines. A
 * models.json that is not JSON is the diagnostic. (pi 1.1.0 prints nothing for an unknown --tools name: test_select) */
static void test_diag_one_line(void)
{
    run_opts ro;
    memset(&ro, 0, sizeof(ro));
    ro.nocmp = 1;
    ro.diag = 1;
    ro.bad_models = 1;
    scenario("json_text", &ro);
    CHECK(strstr(diag_log, "[WARN] ") == diag_log);
    CHECK(strstr(diag_log, "(models.json is ignored)") != 0);
    CHECK(strstr(diag_log, "[WARN] |") == 0);       /* not "[WARN] " and the message as two lines */
    free(ro.out);
}

/* pi prints an argument error as "Error: Unknown option: --bogus" on stderr in every mode and exits 1
 * (oracle run, pi 1.1.0; main.ts process.exit(1) on an args error): the diagnostic stream, never the JSONL or RPC stdout */
static void test_arg_error_on_diag(void)
{
    static const char *const mode[] = { "json_text", "prompt_tool" };
    int i;
    for (i = 0; i < 2; i++) {
        run_opts ro;
        memset(&ro, 0, sizeof(ro));
        ro.nocmp = 1;
        ro.diag = 1;
        ro.more_args = "--bogus";
        scenario(mode[i], &ro);
        CHECK(!strcmp(diag_log, "Error: Unknown option: --bogus"));
        CHECK(!ro.out || ro.out[0] == 0);
        CHECK_INT(ro.rc, 1);
        free(ro.out);
    }
}

/* --thinking with a level pi does not know is a WARNING: pi prints "Warning: Invalid thinking level "bad". Valid values:
 * off, minimal, low, medium, high, xhigh, max" on stderr (cli/args.ts:169-176, main.ts:621-625) and goes on; the level
 * stays what it was (oracle run, pi 1.1.0: `--thinking bad -v` -> that line on stderr, "1.1.0" on stdout, rc 0;
 * `--thinking bad --bogus --thinking worse -v` -> one Warning line per bad level, version, rc 0) */
static void test_thinking_warning(void)
{
    static const char w1[] = "Warning: Invalid thinking level \"bad\". Valid values: off, minimal, low, medium, high, xhigh, max";
    run_opts ro;
    char *a[] = { "x", "--thinking", "high", "--thinking", "bad", "-v" };
    char *b[] = { "x", "--thinking", "bad", "--thinking", "worse", "-v" };
    char *c[] = { "x", "--thinking", "bad", "--bogus" };
    pi_args pa;
    CHECK_INT(pi_args_parse(&pa, 6, a), 0);
    CHECK_STR(pa.warn, w1);
    CHECK_INT(pa.thinking, LT_HIGH);                /* a bad level does not reset the earlier good one */
    CHECK_INT(pi_args_parse(&pa, 6, b), 0);
    CHECK(strstr(pa.warn, "\nWarning: Invalid thinking level \"worse\"") != 0);
    CHECK_INT(pi_args_parse(&pa, 4, c), -1);        /* the error stays an error, the warning before it is kept */
    CHECK_STR(pa.warn, w1);
    memset(&ro, 0, sizeof(ro));
    ro.nocmp = 1;
    ro.diag = 1;
    ro.more_args = "--thinking";
    ro.more_argv[0] = "bad";
    ro.more_argv[1] = "-v";
    scenario("json_text", &ro);
    CHECK(ro.out && !strcmp(ro.out, "1.2.0\n"));
    CHECK_STR(diag_log, w1);
    CHECK_INT(ro.rc, 0);
    free(ro.out);
}

/* --version / -v prints exactly what pi prints, the pi version amiga-pi matches, and exits 0, in every mode and over an
 * unknown option or -p without a prompt (oracle run, pi 1.1.0: `pi -v` -> "1.1.0\n" rc 0; `-v --bogus`, `--bogus -v`,
 * `-p -v`, `--mode json -v` the same; `--mode xx -v` is still the mode error, rc 1) */
static void test_version_flag(void)
{
    static const char *const flag[] = { "--version", "-v" };
    int i;
    for (i = 0; i < 2; i++) {
        run_opts ro;
        memset(&ro, 0, sizeof(ro));
        ro.nocmp = 1;
        ro.diag = 1;
        ro.more_args = flag[i];
        scenario("json_text", &ro);
        CHECK(ro.out && !strcmp(ro.out, "1.2.0\n"));
        CHECK_INT(ro.rc, 0);
        CHECK(diag_log[0] == 0);
        free(ro.out);
    }
    {
        char *one[] = { "x", "-v", "--bogus" }, *two[] = { "x", "--bogus", "-v" }, *three[] = { "x", "-p", "-v" };
        char *bad[] = { "x", "--mode", "xx", "-v" };
        pi_args a;
        CHECK_INT(pi_args_parse(&a, 3, one), 0);
        CHECK_INT(a.version, 1);
        CHECK_INT(pi_args_parse(&a, 3, two), 0);
        CHECK_INT(a.version, 1);
        CHECK_INT(pi_args_parse(&a, 3, three), 0);
        CHECK_INT(a.version, 1);
        CHECK_INT(pi_args_parse(&a, 4, bad), -1);
        CHECK_STR(a.err, "Invalid mode \"xx\". Valid values: text, json, rpc");
    }
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

/* L1, L2: hook commands on pi's extension events (ext_hooks.h). Each scenario is a TypeScript extension run by pi 1.1.0 next
 * to the same behaviour as hook commands in amiga-pi's settings (tools/pi_oracle/rpc_gen.py, ext_*): the JSON records pi
 * prints are compared, and for ext_events / ext_rpc_prompt / ext_rpc_new also the payload of every event pi handed its
 * handler, against the event files the hook commands received (dump.jsonl, the masks of both sides the same) */
static void test_hooks_pi(void)
{
    static const char *const names[] = { "ext_events", "ext_block_plain", "ext_mutate", "ext_matcher", "ext_result",
                                         "ext_input_transform", "ext_input_handled", "ext_rpc_prompt", "ext_rpc_handled",
                                         "ext_rpc_new", "ext_ev_model", "ext_ev_fork", "ext_ev_run", "ext_ev_compact", "ext_ev_bash", 0 };
    group(names);
}

/* L7: what handlers of the other events decide (context rewrites, a custom message from before_agent_start, the before events
 * cancelling, an extension's own compaction, user_bash's result, a boundary that continues, turn_end's entries, resources
 * the extension names): hook commands and Lua extensions against pi 1.1.0's records, requests and session files */
static void test_decisions(void)
{
    static const char *const names[] = { "ext_ctx_rewrite", "ext_bas_message", "ext_cancel_session", "ext_cancel_compact",
                                         "ext_bash_result", "ext_turn_entries", "ext_input_images", "ext_lua_input_images",
                                         "ext_lua_ctx_rewrite", "ext_lua_ctx_inplace", "ext_lua_bas_message",
                                         "ext_lua_cancel_session", "ext_lua_cancel_compact", "ext_lua_compact_own",
                                         "ext_lua_bash_result", "ext_lua_settle_continue", "ext_lua_turn_entries",
                                         "ext_lua_resources", "ext_lua_ev_ui", "ext_term_block", "ext_lua_term_block",
                                         "ext_result_usage", "ext_lua_result_usage", "ext_lua_tool_usage", "ext_lua_msg_end",
                                         "ext_lua_error_rpc", "ext_lua_ui_misc", "ext_prov_events", "ext_lua_prov_events", "ext_lua_prov_payload",
                                         "ext_lua_prov_headers", 0 };
    group(names);
}

/* a diagnostic line with this run's directories and pi's fixed ones written as <cwd> and <agent>, an extension's suffix as .<ext> */
static void mask_diag(const char *text, jw *o)
{
    const char *from[4], *to[4];
    int i, k;
    from[0] = GOLD_CWD;
    from[1] = GOLD_AGENT;
    from[2] = proj;
    from[3] = mask_agent;
    to[0] = to[2] = "<cwd>";
    to[1] = to[3] = "<agent>";
    for (i = 0; text[i];) {
        for (k = 0; k < 4; k++)
            if (from[k][0] && !strncmp(text + i, from[k], strlen(from[k]))) {
                jw_rawz(o, to[k]);
                i += (int)strlen(from[k]);
                break;
            }
        if (k < 4)
            continue;
        if (!strncmp(text + i, ".lua)", 5) || !strncmp(text + i, ".ts)", 4)) {
            jw_rawz(o, ".<ext>)");
            i += text[i + 1] == 'l' ? 5 : 4;
            continue;
        }
        jw_raw(o, text + i++, 1);
    }
}

/* L16: in print and json mode pi tells an extension's error on stderr, `Extension error (<path>): <message>` (print-mode.ts:101); the
 * golden is pi's stderr (.err) with the path masked as the records' are */
static void test_ext_error_stderr(void)
{
    static const char *const names[] = { "ext_lua_error_json", 0 };
    int i;
    for (i = 0; names[i]; i++) {
        run_opts ro;
        char *want;
        long wn;
        jw wm, gm;
        if (getenv("PI_MODES_ONLY") && strcmp(getenv("PI_MODES_ONLY"), names[i]))
            continue;
        memset(&ro, 0, sizeof(ro));
        ro.diag = 1;
        CHECK(scenario(names[i], &ro));
        want = slurp_rpc("ext_error_json", ".err", &wn);
        CHECK(want != 0);
        if (want) {
            jw_init(&wm);
            jw_init(&gm);
            while (wn && (want[wn - 1] == '\n'))
                want[--wn] = 0;
            mask_diag(want, &wm);
            mask_diag(diag_log, &gm);
            CHECK(wm.p && gm.p && !strcmp(wm.p, gm.p));
            if (!wm.p || !gm.p || strcmp(wm.p, gm.p))
                printf("  stderr want %s\n  stderr got  %s\n", wm.p ? wm.p : "", gm.p ? gm.p : "");
            jw_free(&wm);
            jw_free(&gm);
            free(want);
        }
    }
}

/* L1 reachability: pi_app -> the agent loop -> the hook dispatcher -> a hook command that blocks bash: the command ran
 * once (the sentinel) and the toolResult is pi's error result with the hook's reason (the golden has pi's own text) */
static void test_hooks_reach(void)
{
    long r0 = pi_hook_runs;
    if (getenv("PI_MODES_ONLY") && strcmp(getenv("PI_MODES_ONLY"), "ext_block"))
        return;
    CHECK(scenario("ext_block", 0));
    CHECK_INT(pi_hook_runs - r0, 1);
}

/* L3: the same behaviours as Lua extensions (tools/pi_oracle/gen_lua_scenarios.py makes each ext_lua_* cfg from its hook-command
 * twin; the golden is pi 1.1.0's, shared): the records pi prints, and for the event recorders the payload of every event
 * the handler was handed, with keys sorted on both sides (json.encode sorts) */
static void test_lua_pi(void)
{
    static const char *const names[] = { "ext_lua_events", "ext_lua_block_plain", "ext_lua_mutate", "ext_lua_matcher",
                                         "ext_lua_result", "ext_lua_input_transform", "ext_lua_input_handled",
                                         "ext_lua_rpc_prompt", "ext_lua_rpc_handled", "ext_lua_rpc_new", "ext_lua_ev_model",
                                         "ext_lua_ev_fork", "ext_lua_ev_run", "ext_lua_ev_compact", "ext_lua_ev_bash", 0 };
    group(names);
}

/* L3 reachability: pi_app discovers <agent dir>/extensions/t.lua, the agent loop's tool_call reaches the Lua handler (the
 * sentinel counts its one call) and the bash call becomes pi's error toolResult with the extension's reason (the golden is
 * pi's own run with a TypeScript extension that blocks) */
static void test_lua_reach(void)
{
    long c0 = pi_lua_calls;
    if (getenv("PI_MODES_ONLY") && strcmp(getenv("PI_MODES_ONLY"), "ext_lua_block"))
        return;
    CHECK(scenario("ext_lua_block", 0));
    CHECK_INT(pi_lua_calls - c0, 1);
}

/* L8: the gate is the last entry of the one extension list (ext.h ranks), so a Lua tool_call extension that blocks bash decides
 * before it and the gate asks nothing; the same run with -ne (no extension) shows the gate does ask for bash with --gate */
static void test_gate_in_list(void)
{
    static const char *const flag[] = { 0, "--no-extensions" };
    int i;
    for (i = 0; i < 2; i++) {
        run_opts ro;
        char in[600];
        memset(&ro, 0, sizeof(ro));
        ro.nocmp = 1;
        ro.force_mode = "rpc";
        ro.more_args = "--gate";
        ro.more_argv[0] = flag[i];
        strcpy(in, "{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"run it\"}\n");
        if (i)
            strcat(in, "#after extension_ui_request\n{\"type\":\"extension_ui_response\",\"id\":\"$ui:1\",\"value\":\"No\"}\n");
        ro.inline_in = in;
        scenario("ext_lua_block", &ro);
        CHECK(has(ro.out, "\"type\":\"agent_settled\""));
        if (i) {
            CHECK(has(ro.out, "\"method\":\"select\",\"title\":\"Allow bash"));
        } else {
            CHECK(!has(ro.out, "extension_ui_request"));
            CHECK(has(ro.out, "no shell today"));
        }
        free(ro.out);
    }
}

/* L4, L6: tools, commands and entries a Lua extension registers, against pi 1.1.0 running the TypeScript twin
 * (tools/pi_oracle/rpc_gen.py ext_tool*, ext_cmd*, ext_entry; the Lua extensions are in gen_lua_scenarios.py): the records pi
 * prints, the tools and messages of each request, what execute and the handlers were handed, the session file */
static void test_lua_register(void)
{
    static const char *const names[] = { "ext_lua_tool", "ext_lua_tool_results", "ext_lua_tool_allow", "ext_lua_tool_named",
                                         "ext_lua_tool_only", "ext_lua_tool_none", "ext_lua_tool_minus", "ext_lua_tool_plus_none",
                                         "ext_lua_tool_pattern", "ext_lua_tool_default", "ext_lua_cmd", "ext_lua_cmd_dup",
                                         "ext_lua_cmd_scope", "ext_lua_entry", 0 };
    group(names);
}

/* L4 reachability: pi_app discovers t.lua, which registers the tool "greet"; the fake model calls it, so the agent loop runs its
 * execute function (the sentinel counts that one call) and its result is the toolResult of the session file (the golden is pi's
 * run of the TypeScript twin: same records, same requests, same file) */
static void test_lua_tool_reach(void)
{
    long c0 = pi_lua_tool_calls;
    if (getenv("PI_MODES_ONLY") && strcmp(getenv("PI_MODES_ONLY"), "ext_lua_tool"))
        return;
    CHECK(scenario("ext_lua_tool", 0));
    CHECK_INT(pi_lua_tool_calls - c0, 1);
}

/* L6 reachability: the same path with the tool "note", whose pi.appendEntry calls leave custom entries in the session file and
 * none in the second request */
static void test_lua_entry_reach(void)
{
    long c0 = pi_lua_tool_calls;
    if (getenv("PI_MODES_ONLY") && strcmp(getenv("PI_MODES_ONLY"), "ext_lua_entry"))
        return;
    CHECK(scenario("ext_lua_entry", 0));
    CHECK_INT(pi_lua_tool_calls - c0, 1);
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
    test_overflow_recovery();
    test_misc();
    test_sessions();
    test_ui();
    test_reach();
    test_sdk();
    test_retry_cancel();
    test_duration_entries();
    test_retry_cap();
    test_retry_overflow();
    test_retry_refused();
    test_gate();
    test_tool_update();
    test_templates_rpc();
    test_diag_one_line();
    test_arg_error_on_diag();
    test_version_flag();
    test_thinking_warning();
    test_hooks_pi();
    test_hooks_reach();
    test_decisions();
    test_ext_error_stderr();
    test_lua_pi();
    test_lua_reach();
    test_lua_register();
    test_lua_tool_reach();
    test_lua_entry_reach();
    test_gate_in_list();
}
