/* login -- see login.h. pi references: coding-agent/src/modes/interactive/interactive-mode.ts (vendor/pi-src, pi 1.1.0). */
#include <stdlib.h>
#include <string.h>
#include "login.h"
#include "authstore.h"
#include "claude/util.h"
#include "claude/http.h"
#include "llm/request.h"
#include "llm/creds.h"

long pi_auth_refreshes, pi_auth_logins;

#define AUTH_FILE "auth.json"
#define EXPIRY_WINDOW_S (5L * 60)       /* resolveStoredOAuth: DEFAULT_OAUTH_MINIMUM_VALIDITY_MS */

/* ---- the auth.json text in memory ---- */

/* the text replaces the registry's (and the config's view of it) */
static void adopt(pi_auth *a, char *text, long n)
{
    free(a->m->auth);
    a->m->auth = text;
    a->m->cfg.auth = text;
    a->m->cfg.auth_n = text ? n : 0;
}

/* the file read again: the text in memory is then what is on disk (another amiga-pi may have renewed a token) */
static void reload(pi_auth *a)
{
    long n = 0;
    char *t;
    if (!a->sys || !a->agent_dir || !a->agent_dir[0])
        return;                         /* no file: the text in memory is all there is */
    t = pi_store_read(a->sys, a->agent_dir, AUTH_FILE, &n);
    if (t || a->m->auth)
        adopt(a, t, n);
}

/* the provider's entry in the text in memory: 1 with *e */
static int entry_of(pi_auth *a, const char *provider, jv *e)
{
    jv root;
    if (!a->m->auth || json_parse(a->m->auth, a->m->cfg.auth_n, &root) || json_type(root) != J_OBJ)
        return 0;
    return json_get(root, provider, e);
}

/* auth.json with the provider's entry set (value: its JSON text) or removed (value 0), written, and the text in memory
 * replaced: 0, -1 with err. The write comes first: the memory only changes with the file (unless `memory_only`: a renewed
 * token that could not be saved still serves this run) */
static int store_entry(pi_auth *a, const char *provider, const char *value, int memory_only, char *err, long cap)
{
    jw out;
    char *copy;
    int rc;
    jw_init(&out);
    rc = pi_jobj_set(a->m->auth ? a->m->auth : "", a->m->auth ? a->m->cfg.auth_n : 0, provider, value, &out, err, cap);
    if (rc) {
        jw_free(&out);
        if (rc == -2)
            cl_copy(err, "out of memory", cap);
        return -1;
    }
    if (!memory_only && pi_store_write(a->sys, a->agent_dir, AUTH_FILE, out.p, out.n, err, cap)) {
        jw_free(&out);
        return -1;
    }
    copy = (char *)malloc((size_t)out.n + 1);
    if (!copy) {
        jw_free(&out);
        cl_copy(err, "out of memory", cap);
        return -1;
    }
    memcpy(copy, out.p, (size_t)out.n + 1);
    adopt(a, copy, out.n);
    jw_free(&out);
    return 0;
}

/* ---- the transport of the exchanges ---- */

static llm_ts a_now(void *u)
{
    pi_auth *a = (pi_auth *)u;
    llm_ts t;
    if (a->s && a->s->now)
        return a->s->now(a->s->h.u);
    t.sec = 0;
    t.ms = 0;
    return t;
}

static int a_cancelled(void *u)
{
    pi_auth *a = (pi_auth *)u;
    if (!a->cancel && a->s && a->s->h.poll && a->s->h.poll(a->s->h.u))
        a->cancel = 1;
    return a->cancel;
}

/* waits ms in slices that watch for Ctrl+C: 1 when it was pressed */
static int a_sleep(void *u, long ms)
{
    pi_auth *a = (pi_auth *)u;
    while (ms > 0) {
        long step = ms > 250 ? 250 : ms;
        if (a->s && a->s->h.sleep && a->s->h.sleep(a->s->h.u, step))
            a->cancel = 1;
        if (a_cancelled(u))
            return 1;
        ms -= step;
    }
    return 0;
}

static int a_random(void *u, unsigned char *buf, long n)
{
    pi_auth *a = (pi_auth *)u;
    return a->random ? a->random(a->random_u, buf, n) : -1;
}

static long a_getenv(void *u, const char *name, char *out, long cap)
{
    pi_auth *a = (pi_auth *)u;
    return a->getenv ? a->getenv(name, out, cap) : -1;
}

typedef struct hx {
    pi_auth *a;
    jw *resp;
    char head[2048 + 4096];
    long hn;
} hx;

static int hx_head(void *u, const char **head, long *hn)
{
    hx *h = (hx *)u;
    *head = h->head;
    *hn = h->hn;
    return 0;
}

static int hx_poll(void *u)
{
    return a_cancelled(((hx *)u)->a);
}

static void hx_body(void *u, const char *s, long n)
{
    hx *h = (hx *)u;
    if (h->resp->n < 262144L)
        jw_raw(h->resp, s, n);
}

static unsigned long hx_ms(void *u)
{
    pi_auth *a = ((hx *)u)->a;
    return a->s && a->s->h.ms ? a->s->h.ms(a->s->h.u) : 0;
}

/* one exchange (oauth.h oa_io.http). The URL is https, or under a->url_base (the tests' loopback server) http. The
 * head carries the credential of the exchange; it is never logged (no h.log here) */
static int a_http(void *u, const char *method, const char *url, const char *hdrs, const char *body, long bn, int *status,
                  jw *resp, char *err, long cap)
{
    pi_auth *a = (pi_auth *)u;
    char mapped[700];
    http_url hu;
    llm_call c;
    hx *h;
    jw errbody;
    http_resp hr;
    sse ss;
    char buf[2048];
    int rc;
    if (a->url_base && !strncmp(url, "https://", 8)) {
        cl_copy(mapped, a->url_base, sizeof(mapped));
        cl_cat(mapped, "/", sizeof(mapped));
        cl_cat(mapped, url + 8, sizeof(mapped));
        url = mapped;
    }
    if (http_parse_url(url, &hu)) {
        cl_copy(err, "not an http or https URL", cap);
        return -1;
    }
    if (!hu.tls) {
        unsigned char ip[4];
        /* a credential never goes over plain http; the tests' loopback server is the one exception */
        if (!a->url_base || !llm_ipv4(hu.host, ip) || ip[0] != 127) {
            cl_copy(err, "refusing to send a login over plain http (", cap);
            cl_cat(err, hu.host, cap);
            cl_cat(err, "); use https", cap);
            return -1;
        }
    }
    if (!a->s || !a->s->pool) {
        cl_copy(err, "no network", cap);
        return -1;
    }
    h = (hx *)malloc(sizeof(*h));
    if (!h) {
        cl_copy(err, "out of memory", cap);
        return -1;
    }
    h->a = a;
    h->resp = resp;
    {
        char len[24];
        cl_copy(h->head, method, sizeof(h->head));
        cl_cat(h->head, " ", sizeof(h->head));
        cl_cat(h->head, hu.path[0] ? hu.path : "/", sizeof(h->head));
        cl_cat(h->head, " HTTP/1.1\r\nHost: ", sizeof(h->head));
        cl_cat(h->head, hu.host, sizeof(h->head));
        if (hu.port != (hu.tls ? 443 : 80)) {
            cl_ltoa(hu.port, len);
            cl_cat(h->head, ":", sizeof(h->head));
            cl_cat(h->head, len, sizeof(h->head));
        }
        cl_cat(h->head, "\r\n", sizeof(h->head));
        if (hdrs)
            cl_cat(h->head, hdrs, sizeof(h->head));
        if (body || !strcmp(method, "POST")) {
            cl_ltoa(bn, len);
            cl_cat(h->head, "Content-Length: ", sizeof(h->head));
            cl_cat(h->head, len, sizeof(h->head));
            cl_cat(h->head, "\r\n", sizeof(h->head));
        }
        cl_cat(h->head, "Connection: keep-alive\r\n\r\n", sizeof(h->head));
        h->hn = (long)strlen(h->head);
    }
    memset(&c, 0, sizeof(c));
    jw_init(&errbody);
    c.pool = a->s->pool;
    c.host = hu.host;
    c.port = hu.port;
    c.tls = hu.tls;
    c.body = body ? body : "";
    c.bn = bn;
    c.h.u = h;
    c.h.poll = hx_poll;
    c.h.ms = hx_ms;
    c.h.head = hx_head;
    c.h.body = hx_body;
    c.idle_ms = 30000;
    c.buf = buf;
    c.bufcap = sizeof(buf);
    c.resp = &hr;
    c.sse = &ss;
    c.errbody = &errbody;
    rc = llm_post(&c);
    memset(h->head, 0, sizeof(h->head));
    free(h);
    if (rc == LP_OK) {
        *status = hr.status;
        if (errbody.n && resp->n < 262144L)
            jw_raw(resp, errbody.p, errbody.n);
        jw_free(&errbody);
        return 0;
    }
    jw_free(&errbody);
    if (rc == LP_BREAK) {
        a->cancel = 1;
        cl_copy(err, "Login cancelled", cap);
    } else if (rc == LP_CONNECT)
        cl_copy(err, "cannot connect", cap);
    else if (rc == LP_IDLE)
        cl_copy(err, "no answer within 30 seconds", cap);
    else
        cl_copy(err, "the connection failed", cap);
    if (c.err && *c.err && rc != LP_BREAK) {
        cl_cat(err, ": ", cap);
        cl_cat(err, c.err, cap);
    }
    return -1;
}

/* ---- the user's side of a flow ---- */

static void a_info(void *u, int kind, const char *x, const char *y)
{
    pi_auth *a = (pi_auth *)u;
    char line[700];
    const pi_login_ui *ui = a->ui;
    if (!ui || !ui->note)
        return;
    if (kind == OA_I_AUTH_URL) {
        ui->note(ui->u, PL_ACCENT, x);
        if (y && *y)
            ui->note(ui->u, PL_WARNING, y);
    } else if (kind == OA_I_DEVICE_CODE) {
        ui->note(ui->u, PL_ACCENT, y);
        cl_copy(line, "Enter code: ", sizeof(line));
        cl_cat(line, x, sizeof(line));
        ui->note(ui->u, PL_WARNING, line);
        ui->note(ui->u, PL_DIM, "Waiting for authentication... (Ctrl+C to cancel)");
    } else if (kind == OA_I_INFO)
        ui->note(ui->u, PL_TEXT, x);
    else
        ui->note(ui->u, PL_DIM, x);
}

static int a_prompt(void *u, int kind, const char *message, const char *placeholder, char *out, long cap)
{
    pi_auth *a = (pi_auth *)u;
    const pi_login_ui *ui = a->ui;
    (void)kind;
    if (!ui || !ui->line)
        return -1;
    return ui->line(ui->u, message, placeholder, out, cap);
}

static int a_select(void *u, const char *message, const char *const *labels, int n)
{
    pi_auth *a = (pi_auth *)u;
    const pi_login_ui *ui = a->ui;
    return ui && ui->menu ? ui->menu(ui->u, message, labels, n) : -1;
}

static int a_known(void *u, const char *provider, const char *id)
{
    pi_auth *a = (pi_auth *)u;
    return llm_reg_find(&a->m->reg, provider, id) != 0;
}

void pi_auth_io(pi_auth *a, oa_io *io)
{
    memset(io, 0, sizeof(*io));
    io->u = a;
    io->http = a_http;
    io->now = a_now;
    io->sleep = a_sleep;
    io->cancelled = a_cancelled;
    io->random = a_random;
    io->getenv = a_getenv;
    io->info = a_info;
    io->prompt = a_prompt;
    io->select = a_select;
    io->known_model = a_known;
    io->device_id = a->device_id[0] ? a->device_id : 0;
    io->agent_name = a->agent_name;
}

/* ---- the credential at a request ---- */

void pi_auth_init(pi_auth *a, pi_models *m, pi_session *s, cl_sys *sys, const char *agent_dir,
                  long (*getenv)(const char *, char *, long))
{
    a->m = m;
    a->s = s;
    a->sys = sys;
    a->agent_dir = agent_dir;
    a->getenv = getenv;
    s->auth_hook = pi_auth_prepare;
    s->auth_u = a;
}

int pi_auth_prepare(void *u, pi_session *s, int force, char *err, long cap)
{
    pi_auth *a = (pi_auth *)u;
    const char *prov = s->model.provider;
    const oa_provider *op;
    jv e, t;
    llm_ts now;
    char ebuf[400], *old_access = 0, *copy;
    oa_io io;
    jw ne;
    int rc;
    err[0] = 0;
    if (!llm_oauth_provider(prov) || !(op = oa_find(prov)) || !op->refresh || !s->now)
        return 0;
    if (!entry_of(a, prov, &e) || json_type(e) != J_OBJ || !json_get(e, "type", &t) || !json_streq(t, "oauth"))
        return 0;
    now = s->now(s->h.u);
    if (!force && !oa_expires_soon(e, now, EXPIRY_WINDOW_S))
        return 0;
    {
        jv av;
        if (json_get(e, "access", &av) && json_type(av) == J_STR)
            old_access = json_strdup(av, 0);
    }
    /* pi renews under the store's lock and looks again first: another process may have done it already */
    reload(a);
    if (!entry_of(a, prov, &e) || json_type(e) != J_OBJ || !json_get(e, "type", &t) || !json_streq(t, "oauth")) {
        free(old_access);
        return 0;                       /* logged out meanwhile */
    }
    {
        jv av;
        char *cur = json_get(e, "access", &av) && json_type(av) == J_STR ? json_strdup(av, 0) : 0;
        int changed = old_access && cur && strcmp(old_access, cur) != 0;
        if (old_access)
            memset(old_access, 0, strlen(old_access));
        if (cur)
            memset(cur, 0, strlen(cur));
        free(old_access);
        free(cur);
        if (changed || (!force && !oa_expires_soon(e, now, EXPIRY_WINDOW_S)))
            return 0;                   /* someone else renewed it */
    }
    copy = (char *)malloc((size_t)e.n + 1);
    if (!copy) {
        cl_copy(err, "out of memory", cap);
        return -1;
    }
    memcpy(copy, e.p, (size_t)e.n);
    copy[e.n] = 0;
    {
        jv cv;
        json_parse(copy, e.n, &cv);
        jw_init(&ne);
        pi_auth_io(a, &io);
        a->ui = 0;
        a->cancel = 0;
        pi_auth_refreshes++;
        rc = op->refresh(&io, cv, &ne, ebuf, sizeof(ebuf));
    }
    memset(copy, 0, (size_t)e.n);
    free(copy);
    if (rc) {
        jw_free(&ne);
        cl_copy(err, "OAuth refresh failed for ", cap);
        cl_cat(err, prov, cap);
        cl_cat(err, ": ", cap);
        cl_cat(err, rc == OA_CANCEL ? "Operation aborted" : ebuf, cap);
        return -1;
    }
    if (store_entry(a, prov, ne.p, 0, ebuf, sizeof(ebuf))) {
        /* the token was renewed (and may have rotated) but the file would not take it: this run keeps it */
        char m[300];
        store_entry(a, prov, ne.p, 1, m, sizeof(m));
        if (s->h.notice) {
            cl_copy(m, "could not save ", sizeof(m));
            cl_cat(m, AUTH_FILE, sizeof(m));
            cl_cat(m, ": ", sizeof(m));
            cl_cat(m, ebuf, sizeof(m));
            s->h.notice(s->h.u, m);
        }
    }
    memset(ne.p, 0, (size_t)ne.n);
    jw_free(&ne);
    return 0;
}

/* ---- /login and /logout ---- */

static void say2(const pi_login_ui *ui, int kind, const char *a, const char *b, const char *c)
{
    char line[900];
    cl_copy(line, a, sizeof(line));
    if (b)
        cl_cat(line, b, sizeof(line));
    if (c)
        cl_cat(line, c, sizeof(line));
    ui->note(ui->u, kind, line);
}

static int ieq(const char *a, const char *b)
{
    return cl_strieq(a, b);
}

/* strcmp without regard to case */
static int ci_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return (unsigned char)*a < (unsigned char)*b ? -1 : (unsigned char)*a > (unsigned char)*b ? 1 : 0;
}

/* the stored credential type of the provider: "oauth", "api_key", else 0 */
static const char *stored_type(pi_auth *a, const char *id)
{
    jv e, t;
    if (!entry_of(a, id, &e) || json_type(e) != J_OBJ || !json_get(e, "type", &t))
        return 0;
    if (json_streq(t, "oauth"))
        return "oauth";
    if (json_streq(t, "api_key"))
        return "api_key";
    return 0;
}

/* a row of the provider list: "OpenAI [subscription] - configured" */
static void row(pi_auth *a, const oa_provider *p, int oauth, int show_type, char *out, long cap)
{
    const char *st = stored_type(a, p->id);
    char env[96], nm[96], v[8];
    cl_copy(out, p->name, cap);
    if (show_type)
        cl_cat(out, oauth ? (p->subscription ? " [subscription]" : " [account]") : " [API key]", cap);
    if (st) {
        if (!strcmp(st, oauth ? "oauth" : "api_key"))
            cl_cat(out, " - configured", cap);
        else
            cl_cat(out, !strcmp(st, "oauth") ? " - subscription configured" : " - API key configured", cap);
        return;
    }
    llm_key_env_name(p->id, nm, sizeof(nm));
    if (a->getenv && a->getenv(nm, v, sizeof(v)) > 0 && !oauth) {
        cl_copy(env, " - env: ", sizeof(env));
        cl_cat(env, nm, sizeof(env));
        cl_cat(out, env, cap);
        return;
    }
    cl_cat(out, " - not configured", cap);
}

/* the provider chosen from the providers having the login type (oauth 1 / API key 0); ref: the typed name, shortens the
 * list when it matches. The index into the table, -1 cancelled */
static int pick_provider(pi_auth *a, const pi_login_ui *ui, int oauth, const char *ref, int show_type)
{
    int idx[16], n = 0, i, c;
    const char *opt[16];
    char rows[16][160];
    for (i = 0; i < oa_count() && n < 16; i++) {
        const oa_provider *p = oa_nth(i);
        if (oauth ? p->login != 0 : p->key_name != 0)
            idx[n++] = i;
    }
    if (ref && *ref) {
        /* the search box: only the rows containing the text, when any do */
        int k = 0, keep[16];
        for (i = 0; i < n; i++) {
            const oa_provider *p = oa_nth(idx[i]);
            char lo[160], rf[96];
            long j;
            cl_copy(lo, p->name, sizeof(lo));
            cl_cat(lo, " ", sizeof(lo));
            cl_cat(lo, p->id, sizeof(lo));
            cl_copy(rf, ref, sizeof(rf));
            for (j = 0; lo[j]; j++)
                if (lo[j] >= 'A' && lo[j] <= 'Z')
                    lo[j] = (char)(lo[j] + 32);
            for (j = 0; rf[j]; j++)
                if (rf[j] >= 'A' && rf[j] <= 'Z')
                    rf[j] = (char)(rf[j] + 32);
            if (strstr(lo, rf))
                keep[k++] = idx[i];
        }
        if (k) {
            memcpy(idx, keep, (size_t)k * sizeof(int));
            n = k;
        }
    }
    if (!n) {
        ui->note(ui->u, PL_WARNING, oauth ? "No account providers available." : "No API key providers available.");
        return -1;
    }
    for (i = 0; i < n; i++) {
        row(a, oa_nth(idx[i]), oauth, show_type, rows[i], sizeof(rows[i]));
        opt[i] = rows[i];
    }
    c = ui->menu(ui->u, "Select provider to configure:", opt, n);
    return c < 0 ? -1 : idx[c];
}

/* the default model a login of the provider selects when the session has no usable one (model-resolver.ts
 * defaultModelPerProvider) */
static const char *default_model(const char *provider)
{
    static const char *const t[][2] = {
        { "openai", "gpt-5.5" }, { "openai-codex", "gpt-6.1-sol" }, { "github-copilot", "gpt-5.4" },
        { "xai", "grok-4.7" }, { "kimi-coding", "kimi-for-coding" }, { "meta", "muse-spark-1.3" },
        { "anthropic", "claude-opus-4-8" }, { "azure", "gpt-5.4" }, { "google", "gemini-3.1-pro-preview" }
    };
    unsigned i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(t[i][0], provider))
            return t[i][1];
    return 0;
}

/* settings.json defaultProvider / defaultModel (pi's setDefaultModelAndProvider, written after a login picked a model) */
static void save_default(pi_auth *a, const char *provider, const char *id)
{
    long n = 0;
    char *t = pi_store_read(a->sys, a->agent_dir, "settings.json", &n), err[200];
    jw o1, o2, v;
    if (!a->agent_dir)
        return;
    jw_init(&o1);
    jw_init(&o2);
    jw_init(&v);
    jw_strz(&v, provider);
    if (!pi_jobj_set(t ? t : "", t ? n : 0, "defaultProvider", v.p, &o1, err, sizeof(err))) {
        jw_reset(&v);
        jw_strz(&v, id);
        if (!pi_jobj_set(o1.p, o1.n, "defaultModel", v.p, &o2, err, sizeof(err)))
            pi_store_write(a->sys, a->agent_dir, "settings.json", o2.p, o2.n, err, sizeof(err));
    }
    jw_free(&o1);
    jw_free(&o2);
    jw_free(&v);
    free(t);
}

/* completeProviderAuthentication (interactive-mode.ts :6066): the status line, and the provider's default model when
 * the session's own has no credential (pi: when no model is selected). Returns 1 */
static int finish(pi_auth *a, const pi_login_ui *ui, const oa_provider *p, int oauth)
{
    char path[320], line[500], why[500];
    pi_session *s = a->s;
    const char *selected = 0;
    why[0] = 0;
    cl_copy(path, a->agent_dir ? a->agent_dir : "", sizeof(path));
    if (path[0] && path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, AUTH_FILE, sizeof(path));
    if (s && !pi_model_usable(a->m, &s->model)) {
        const char *want = default_model(p->id);
        const llm_model *mo = want ? llm_reg_find(&a->m->reg, p->id, want) : 0;
        char err[200], label[160];
        cl_copy(label, oauth ? "Logged in to " : "Saved API key for ", sizeof(label));
        cl_cat(label, p->name, sizeof(label));
        if (!want) {
            cl_copy(why, label, sizeof(why));
            cl_cat(why, ", but no default model is configured for provider \"", sizeof(why));
            cl_cat(why, p->id, sizeof(why));
            cl_cat(why, "\". Use /model to select a model.", sizeof(why));
        } else if (!mo) {
            cl_copy(why, label, sizeof(why));
            cl_cat(why, ", but its default model \"", sizeof(why));
            cl_cat(why, want, sizeof(why));
            cl_cat(why, "\" is not available. Use /model to select a model.", sizeof(why));
        } else if (pi_model_switch(a->m, s, mo, err, sizeof(err))) {
            cl_copy(why, label, sizeof(why));
            cl_cat(why, ", but selecting its default model failed: ", sizeof(why));
            cl_cat(why, err, sizeof(why));
            cl_cat(why, ". Use /model to select a model.", sizeof(why));
        } else {
            save_default(a, p->id, want);
            selected = want;
        }
    }
    cl_copy(line, oauth ? "Logged in to " : "Saved API key for ", sizeof(line));
    cl_cat(line, p->name, sizeof(line));
    if (selected) {
        cl_cat(line, ". Selected ", sizeof(line));
        cl_cat(line, selected, sizeof(line));
    }
    cl_cat(line, ". Credentials saved to ", sizeof(line));
    cl_cat(line, path, sizeof(line));
    ui->note(ui->u, PL_OK, line);
    if (why[0])
        ui->note(ui->u, PL_ERROR, why);
    return 1;
}

/* the device id of this installation: settings.json deviceId, made and saved on first use (getOrCreateDeviceId) */
static int device_id(pi_auth *a, char *err, long cap)
{
    long n = 0;
    char *t = pi_store_read(a->sys, a->agent_dir, "settings.json", &n), *id;
    jw out, v;
    unsigned char b[16];
    int rc;
    id = t ? pi_jobj_str(t, n, "deviceId") : 0;
    if (id && strlen(id) == 36) {
        cl_copy(a->device_id, id, sizeof(a->device_id));
        free(id);
        free(t);
        return 0;
    }
    free(id);
    if (!a->random || a->random(a->random_u, b, 16)) {
        free(t);
        cl_copy(err, "no secure random source on this system", cap);
        return -1;
    }
    {
        static const char hx[] = "0123456789abcdef";
        int i, k = 0;
        b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
        b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
        for (i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                a->device_id[k++] = '-';
            a->device_id[k++] = hx[b[i] >> 4];
            a->device_id[k++] = hx[b[i] & 15];
        }
        a->device_id[k] = 0;
    }
    jw_init(&out);
    jw_init(&v);
    jw_strz(&v, a->device_id);
    rc = pi_jobj_set(t ? t : "", t ? n : 0, "deviceId", v.p, &out, err, cap);
    if (!rc && pi_store_write(a->sys, a->agent_dir, "settings.json", out.p, out.n, err, cap))
        rc = -1;
    jw_free(&out);
    jw_free(&v);
    free(t);
    return rc;
}

/* showLoginDialog: the OAuth login of the provider. 1 when credentials were saved */
static int login_oauth(pi_auth *a, const pi_login_ui *ui, const oa_provider *p)
{
    oa_io io;
    jw entry;
    char err[500];
    int rc;
    say2(ui, PL_ACCENT, "Login to ", p->name, 0);
    if (!strcmp(p->id, "openai") && device_id(a, err, sizeof(err))) {
        say2(ui, PL_ERROR, "Failed to login to ", p->name, ": ");
        ui->note(ui->u, PL_ERROR, err);
        return 0;
    }
    jw_init(&entry);
    pi_auth_io(a, &io);
    a->ui = ui;
    a->cancel = 0;
    err[0] = 0;
    rc = p->login(&io, &entry, err, sizeof(err));
    a->ui = 0;
    if (rc == OA_CANCEL) {
        jw_free(&entry);
        ui->note(ui->u, PL_DIM, "Login cancelled");
        return 0;
    }
    if (rc) {
        jw_free(&entry);
        say2(ui, PL_ERROR, "Failed to login to ", p->name, 0);
        ui->note(ui->u, PL_ERROR, err);
        return 0;
    }
    if (store_entry(a, p->id, entry.p, 0, err, sizeof(err))) {
        memset(entry.p, 0, (size_t)entry.n);
        jw_free(&entry);
        say2(ui, PL_ERROR, "Failed to login to ", p->name, ": ");
        ui->note(ui->u, PL_ERROR, err);
        return 0;
    }
    memset(entry.p, 0, (size_t)entry.n);
    jw_free(&entry);
    pi_auth_logins++;
    return finish(a, ui, p, 1);
}

/* showApiKeyLoginDialog: "Enter <name>" for a key kept as {"type":"api_key","key":...} */
static int login_key(pi_auth *a, const pi_login_ui *ui, const oa_provider *p)
{
    char key[1200], msg[160], err[300];
    jw v;
    int rc;
    say2(ui, PL_ACCENT, "Login to ", p->name, 0);
    cl_copy(msg, "Enter ", sizeof(msg));
    cl_cat(msg, p->key_name, sizeof(msg));
    if (ui->line(ui->u, msg, 0, key, sizeof(key))) {
        ui->note(ui->u, PL_DIM, "Login cancelled");
        return 0;
    }
    {
        long s = 0, e = (long)strlen(key);
        while (s < e && (key[s] == ' ' || key[s] == '\t'))
            s++;
        while (e > s && (key[e - 1] == ' ' || key[e - 1] == '\t' || key[e - 1] == '\r' || key[e - 1] == '\n'))
            e--;
        memmove(key, key + s, (size_t)(e - s));
        key[e - s] = 0;
    }
    if (!key[0]) {
        say2(ui, PL_ERROR, "Failed to save API key for ", p->name, ": no key was entered");
        return 0;
    }
    jw_init(&v);
    jw_rawz(&v, "{\"type\":\"api_key\",\"key\":");
    jw_strz(&v, key);
    jw_rawz(&v, "}");
    memset(key, 0, sizeof(key));
    rc = store_entry(a, p->id, v.p, 0, err, sizeof(err));
    memset(v.p, 0, (size_t)v.n);
    jw_free(&v);
    if (rc) {
        say2(ui, PL_ERROR, "Failed to save API key for ", p->name, ": ");
        ui->note(ui->u, PL_ERROR, err);
        return 0;
    }
    return finish(a, ui, p, 0);
}

int pi_login_run(pi_auth *a, const char *arg, const pi_login_ui *ui)
{
    int oauth, pi_idx;
    const oa_provider *p;
    while (arg && (*arg == ' ' || *arg == '\t'))
        arg++;
    reload(a);
    if (arg && *arg) {
        /* findLoginProviderOptions: the typed name is a provider's id or name */
        int i, hits = 0, which = -1, kinds = 0;
        for (i = 0; i < oa_count(); i++) {
            const oa_provider *q = oa_nth(i);
            if (ieq(q->id, arg) || ieq(q->name, arg)) {
                hits++;
                which = i;
                kinds = (q->login != 0) + (q->key_name != 0);
            }
        }
        if (hits == 1 && kinds == 1) {
            p = oa_nth(which);
            return p->login ? login_oauth(a, ui, p) : login_key(a, ui, p);
        }
        if (hits == 1) {
            const oa_provider *q = oa_nth(which);
            char title[160];
            const char *opt[2];
            int c;
            opt[0] = q->login_label ? q->login_label : "Sign in with an account";
            opt[1] = "Sign in with an API key";
            cl_copy(title, "Select authentication method for ", sizeof(title));
            cl_cat(title, q->name, sizeof(title));
            cl_cat(title, ":", sizeof(title));
            c = ui->menu(ui->u, title, opt, 2);
            if (c < 0)
                return 0;
            return c == 0 ? login_oauth(a, ui, q) : login_key(a, ui, q);
        }
    }
    if (arg && *arg) {
        /* no provider of that name: the list, narrowed by what was typed (every kind of login) */
        int c = pick_provider(a, ui, 1, arg, 1);
        if (c < 0)
            return 0;
        p = oa_nth(c);
        return login_oauth(a, ui, p);
    }
    {
        const char *opt[2];
        int c;
        opt[0] = "Sign in with an account";
        opt[1] = "Sign in with an API key";
        c = ui->menu(ui->u, "Select authentication method:", opt, 2);
        if (c < 0)
            return 0;
        oauth = c == 0;
    }
    pi_idx = pick_provider(a, ui, oauth, 0, 0);
    if (pi_idx < 0)
        return 0;
    p = oa_nth(pi_idx);
    return oauth ? login_oauth(a, ui, p) : login_key(a, ui, p);
}

int pi_logout_run(pi_auth *a, const pi_login_ui *ui)
{
    jv root, k, v, t;
    jit it;
    const char *opt[40];
    char rows[40][200], ids[40][64];
    int oauth[40], n = 0, c, i;
    reload(a);
    if (a->m->auth && !json_parse(a->m->auth, a->m->cfg.auth_n, &root) && json_type(root) == J_OBJ) {
        json_iter(root, &it);
        while (json_next(&it, &k, &v) && n < 40) {
            const oa_provider *p;
            if (json_type(v) != J_OBJ || !json_get(v, "type", &t) || json_type(t) != J_STR)
                continue;
            json_str(k, ids[n], sizeof(ids[n]));
            p = oa_find(ids[n]);
            cl_copy(rows[n], p ? p->name : ids[n], sizeof(rows[n]));
            oauth[n] = json_streq(t, "oauth");
            if (oauth[n])
                cl_cat(rows[n], p && !p->subscription ? " [account]" : " [subscription]", sizeof(rows[n]));
            else
                cl_cat(rows[n], " [API key]", sizeof(rows[n]));
            cl_cat(rows[n], " - stored credential", sizeof(rows[n]));
            n++;
        }
    }
    if (!n) {
        ui->note(ui->u, PL_DIM,
                 "No stored credentials to remove. /logout only removes credentials saved by /login; environment variables "
                 "and models.json config are unchanged.");
        return 0;
    }
    /* sorted by name, as pi's list (case-insensitive insertion sort) */
    for (i = 0; i < n; i++)
        opt[i] = rows[i];
    for (i = 1; i < n; i++) {
        int j;
        for (j = i; j > 0 && ci_cmp(rows[j], rows[j - 1]) < 0; j--) {
            char tr[200], ti[64];
            int to;
            memcpy(tr, rows[j], sizeof(tr));
            memcpy(rows[j], rows[j - 1], sizeof(tr));
            memcpy(rows[j - 1], tr, sizeof(tr));
            memcpy(ti, ids[j], sizeof(ti));
            memcpy(ids[j], ids[j - 1], sizeof(ti));
            memcpy(ids[j - 1], ti, sizeof(ti));
            to = oauth[j];
            oauth[j] = oauth[j - 1];
            oauth[j - 1] = to;
        }
    }
    c = ui->menu(ui->u, "Select provider to logout:", opt, n);
    if (c < 0)
        return 0;
    {
        char err[300];
        const oa_provider *p = oa_find(ids[c]);
        const char *name = p ? p->name : ids[c];
        if (store_entry(a, ids[c], 0, 0, err, sizeof(err))) {
            say2(ui, PL_ERROR, "Logout failed: ", err, 0);
            return 0;
        }
        if (oauth[c])
            say2(ui, PL_OK, "Logged out of ", name, 0);
        else
            say2(ui, PL_OK, "Removed stored API key for ", name,
                 ". Environment variables and models.json config are unchanged.");
    }
    return 1;
}
