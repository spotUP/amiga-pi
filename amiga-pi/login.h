/* login -- /login and /logout, and the credential that goes with a request (plan phase O2, O3).
 *
 * pi 1.1.0's interactive mode (modes/interactive/interactive-mode.ts handleLoginCommand, showLoginDialog,
 * loginProvider, showOAuthSelector) over the flows of oauth.c, as numbered menus and lines instead of pi's full-screen
 * selectors (COMPATIBILITY.md). The credentials are pi's auth.json in the agent directory, ENVARC:amiga-pi/auth.json:
 * {"<provider>": {"type":"oauth","access":...,"refresh":...,"expires":...} | {"type":"api_key","key":...}}; entries this
 * version does not touch come back as they were (authstore.h).
 *
 * At a request (pi_req_auth, agent.c) pi_auth_prepare renews an OAuth credential that expires within five minutes
 * (ai/src/auth/resolve.ts resolveStoredOAuth) and saves the answer; when the API answers 401 the request asks
 * once more (force) and goes again once. A refresh token a provider rotates is saved at once. Tokens are never logged:
 * not in the debug log (llm_request blanks the Authorization header), not in a message, not in a trace; and they only
 * travel over https -- oauth http refuses a plain http URL, except under `url_base`, the tests' loopback server.
 * Portable C89, host-tested (tests/test_pi_login.c). */
#ifndef PI_LOGIN_H
#define PI_LOGIN_H

#include "agent.h"
#include "models.h"
#include "oauth.h"
#include "claude/sys.h"

/* a row of the provider selector (interactive-mode.ts AuthSelectorProvider; oauth-selector.ts formatAuthSelectorProviderType and
 * formatAuthSelectorProviderStatus): the status is "not configured" (PL_ST_NONE), configured with the other kind of login (PL_ST_OTHER;
 * status_text "API key configured") or configured (PL_ST_OK; status_text "" for a stored credential, else the source: "env: NAME") */
enum { PL_ST_NONE, PL_ST_OTHER, PL_ST_OK };
typedef struct pi_login_row {
    const char *name;           /* the provider's name */
    const char *type;           /* "subscription", "account" or "API key" */
    const char *search;         /* what the search box matches: the name, the id, the type, the login method's name */
    int status;
    const char *status_text;
} pi_login_row;

/* the steps of a sign-in dialog (login-dialog.ts), pi's look */
enum { PD_OPEN, PD_CLOSE, PD_AUTH, PD_DEVICE, PD_INFO, PD_PROGRESS };

/* how the screen asks and tells (ui_pi.c gives it the pi look, or plain lines) */
typedef struct pi_login_ui {
    void *u;
    /* a titled list: the option chosen (0..n-1), -1 cancelled */
    int (*menu)(void *u, const char *title, const char *const *opt, int n);
    /* a line: 0 with the text, -1 cancelled; message above, placeholder shown as "e.g., ..." (0 none) */
    int (*line)(void *u, const char *message, const char *placeholder, char *out, long cap);
    /* a note in the conversation: PL_* kind */
    void (*note)(void *u, int kind, const char *text);
    /* pi's look only (0 in the line mode and in the tests that script it by lines): the searchable provider selector
     * (oauth-selector.ts): the row chosen, -1 cancelled; show_type: the rows say which login they are; initial: the search
     * box's text; empty: the row for no match. */
    int (*pick)(void *u, const char *title, const pi_login_row *rows, int n, int show_type, const char *initial,
                const char *empty);
    /* ... the sign-in dialog that replaces the editor: op PD_*; a, b by op (PD_OPEN: title; PD_AUTH: url, instructions;
     * PD_DEVICE: user code, verification URL; PD_INFO, PD_PROGRESS: the text) */
    void (*dialog)(void *u, int op, const char *a, const char *b);
    /* ... a line asked inside the dialog: manual: the redirect URL or code pasted back. 0 with the text, -1 cancelled */
    int (*ask)(void *u, int manual, const char *message, const char *placeholder, char *out, long cap);
    /* ... Esc or Ctrl+C was typed while the program waits for the provider */
    int (*cancelled)(void *u);
} pi_login_ui;

enum { PL_TEXT, PL_DIM, PL_WARNING, PL_ERROR, PL_ACCENT, PL_OK };

typedef struct pi_auth {
    pi_models *m;
    pi_session *s;
    cl_sys *sys;
    const char *agent_dir;
    long (*getenv)(const char *name, char *out, long cap);
    /* secure random bytes for PKCE and the state: 0, -1 none (a login then fails with a message) */
    int (*random)(void *u, unsigned char *buf, long n);
    void *random_u;
    const char *agent_name;     /* what the app is called to a provider; 0: pi's own ("Pi" to ChatGPT, "pi" as Codex's originator) */
    /* tests only: every https://host/path an OAuth exchange names is fetched from <url_base>/host/path, and plain http is
     * then allowed (to the loopback address only). 0 in the program */
    const char *url_base;
    const pi_login_ui *ui;      /* while a login runs */
    int cancel;                 /* Ctrl+C was pressed during the login */
    char device_id[40];
} pi_auth;

/* the reachability tests' sentinels: OAuth refreshes started (a request renewing its credential, or after a 401), logins
 * completed (credentials saved) */
extern long pi_auth_refreshes, pi_auth_logins;

/* the tests' and the app's one set-up: a (zeroed by the caller's static) bound to the models and session; s->auth_hook
 * and s->auth_u are set */
void pi_auth_init(pi_auth *a, pi_models *m, pi_session *s, cl_sys *sys, const char *agent_dir,
                  long (*getenv)(const char *, char *, long));

/* the exchange hooks of a flow bound to this auth (the network through the session's pool, its clock and sleeping, the
 * random source); the caller sets info, prompt and select. For oauth.h's flows when they are driven without the dialog */
void pi_auth_io(pi_auth *a, oa_io *io);

/* pi_session.auth_hook: the credential of s->model's provider ready for a request. force: renew it whatever its expiry
 * (the API refused it). 0, or -1 with pi's message in err ("OAuth refresh failed for meta: Meta session expired ...") */
int pi_auth_prepare(void *u, pi_session *s, int force, char *err, long cap);

/* /login [provider] and /logout: the whole dialog through ui. 1 when something was changed (the screen redraws its footer) */
int pi_login_run(pi_auth *a, const char *arg, const pi_login_ui *ui);
int pi_logout_run(pi_auth *a, const pi_login_ui *ui);

#endif
