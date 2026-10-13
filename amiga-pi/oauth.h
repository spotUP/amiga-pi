/* oauth -- the subscription logins of pi 1.1.0 (plan phase O2/O3), flow by flow as pi's ai/src/auth/oauth/<provider>.ts:
 *   openai          Sign in with ChatGPT: authorization code + PKCE, the client registered dynamically (the issued
 *                   client id comes back in the redirect), the redirect URL pasted back (openai-chatgpt.ts)
 *   openai-codex    device code, or authorization code + PKCE with the code pasted back (openai-codex.ts)
 *   github-copilot  GitHub's device flow, then the Copilot token and the account's model list (github-copilot.ts)
 *   kimi-coding     device flow (kimi-coding.ts)
 *   xai             device flow (xai.ts)
 *   meta            device flow, then an API key minted from the identity token (meta.ts)
 * Never Anthropic: pi has a Claude Pro/Max login (oauth/anthropic.ts), this port deliberately has none (owner decision
 * 2026-10-07; llm_oauth_provider in vtcon llm/creds.c is the list).
 *
 * This file is the protocol and nothing else: every network exchange, the clock, the sleeping, the random bytes and
 * every question to the user go through oa_io, so the flows run unchanged in the host tests (against
 * tools/claude_fixture.py) and on the Amiga. A credential is pi's auth.json entry as JSON text
 * {"type":"oauth","access":...,"refresh":...,"expires":<epoch ms>, ...the provider's extra fields}. Tokens are never
 * written to any log or message made here, and every URL is https (login.c refuses anything else).
 * Portable C89. */
#ifndef PI_OAUTH_H
#define PI_OAUTH_H

#include "claude/json.h"
#include "llm/llm.h"

enum { OA_OK = 0, OA_FAIL = -1, OA_CANCEL = 1 };

/* what the user is shown (pi's AuthEvent) */
enum {
    OA_I_AUTH_URL,              /* a = the URL, b = the instructions */
    OA_I_DEVICE_CODE,           /* a = the user code, b = the verification URI */
    OA_I_INFO,                  /* a = a message */
    OA_I_PROGRESS               /* a = a message */
};
/* what the user is asked (pi's AuthPrompt) */
enum {
    OA_P_TEXT,                  /* a line of text: message, placeholder */
    OA_P_MANUAL                 /* the redirect URL / code pasted back: message, placeholder */
};

typedef struct oa_io {
    void *u;
    /* one HTTP exchange: method "GET" or "POST", url (https), hdrs "Name: value\r\n" lines, body (0: none). 0 with the
     * status and the answer's body appended to resp; -1 with the transport's words in err */
    int (*http)(void *u, const char *method, const char *url, const char *hdrs, const char *body, long bn, int *status,
                jw *resp, char *err, long cap);
    llm_ts (*now)(void *u);                 /* the wall clock */
    int (*sleep)(void *u, long ms);         /* waits; 1 when the user cancelled meanwhile */
    int (*cancelled)(void *u);              /* the user cancelled */
    int (*random)(void *u, unsigned char *buf, long n);     /* secure random bytes: 0, -1 none */
    long (*getenv)(void *u, const char *name, char *out, long cap);    /* -1 unset; may be 0 */
    void (*info)(void *u, int kind, const char *a, const char *b);
    /* a line the user typed: 0, -1 cancelled */
    int (*prompt)(void *u, int kind, const char *message, const char *placeholder, char *out, long cap);
    /* the option chosen (0..n-1), -1 cancelled */
    int (*select)(void *u, const char *message, const char *const *labels, int n);
    /* copilot: does the catalog know this model of the provider (GITHUB_COPILOT_MODELS has it) */
    int (*known_model)(void *u, const char *provider, const char *id);
    const char *device_id;                  /* this installation's UUID (Sign in with ChatGPT needs one), 0 none */
    const char *agent_name;                 /* what the app is called to the provider ("pi"), 0: pi */
} oa_io;

typedef struct oa_provider {
    const char *id;
    const char *name;           /* the provider's name: "OpenAI Codex (legacy)" */
    const char *oauth_name;     /* the OAuth method's name: "OpenAI (ChatGPT Plus/Pro)", 0: no OAuth login */
    const char *login_label;    /* "Sign in with ChatGPT", 0: pi's "Sign in with an account" */
    const char *key_name;       /* the API key method's name: "OpenAI API key", 0: no API key login */
    int subscription;           /* isSubscription */
    /* login: 0 with the credential's entry JSON in *entry, OA_CANCEL, or OA_FAIL with pi's message in err */
    int (*login)(oa_io *io, jw *entry, char *err, long cap);
    /* refresh: the credential (the stored entry) renewed. 0 with the new entry, else OA_FAIL with pi's message */
    int (*refresh)(oa_io *io, jv cred, jw *entry, char *err, long cap);
} oa_provider;

/* the providers /login knows, sorted by name as pi's selector; those with no OAuth login have login 0 */
int oa_count(void);
const oa_provider *oa_nth(int i);
const oa_provider *oa_find(const char *id);

/* is the credential's `expires` within min_s seconds of now (pi: Date.now() + minValidity >= expires; five minutes by
 * default). A credential with no readable expires is not */
int oa_expires_soon(jv cred, llm_ts now, long min_s);

/* the pieces the tests and the request path share */
/* "key=value&..." the way JavaScript's URLSearchParams writes it */
void oa_form_add(jw *w, const char *name, const char *value);
/* 32-bit-safe decimal of epoch milliseconds: sec * 1000 + ms into w */
void oa_write_ms(jw *w, long sec, int ms);

#endif
