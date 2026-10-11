/* session3 -- pi's session file, JSONL version 3 (plan E1, E2, E6, E8;
 * pi f10993bc packages/coding-agent/src/core/session-manager.ts, docs/session-format.md).
 *
 * One file per session: the header line
 *   {"type":"session","version":3,"id":<uuidv7>,"timestamp","cwd","parentSession"?}
 * then one line per entry {type, id (8 hex), parentId, timestamp, ...} in
 * pi's key order. Entries form a tree through parentId; the leaf is where
 * the next entry goes (the last entry of the file when a session is opened).
 * Append-only: an entry is one appended line, except that a new session's
 * file is made only once it holds a user or assistant message (pi's
 * _persist; until then the entries wait in RAM).
 *
 * In RAM: an index of every entry (id, parent, type, role, target, the
 * line's place in the file) and the text of the active branch's lines only;
 * an off-branch line is read again when something asks for it (E2). The
 * file is read in pieces through cl_sys.pread, never whole.
 *
 * Names (E8): sessions/<dir>/<file>, the directory pi's "--<cwd>--" cut to
 * 30 characters by C:Claude's rule (sess_fit), the file
 * "<YYYYMMDDTHHMMSS>_<8 hex>.jsonl" (30): pi's "<ISO time>_<uuid>.jsonl"
 * does not fit FFS's 30.
 * Timestamps (E9): ISO 8601 with milliseconds; with "Z" when the clock is
 * UTC (the host), without an offset when it is the Amiga's local clock.
 * Portable C89 over claude/sys.h and claude/json.h, host-tested
 * (tests/test_pi_session.c). */
#ifndef PI_SESSION3_H
#define PI_SESSION3_H

#include "claude/sys.h"
#include "llm/llm.h"

/* entry types (pi's SessionEntry union), S3_UNKNOWN kept verbatim */
enum { S3_MESSAGE, S3_THINKING, S3_MODEL, S3_USAGE, S3_COMPACTION, S3_CONTEXT_EDIT, S3_BRANCH_SUMMARY, S3_CUSTOM,
       S3_CUSTOM_MESSAGE, S3_LABEL, S3_SESSION_INFO, S3_UNKNOWN };
/* a message entry's role */
enum { S3R_NONE, S3R_USER, S3R_ASSISTANT, S3R_TOOLRESULT, S3R_SYSTEM, S3R_CUSTOM, S3R_BASH, S3R_OTHER };

typedef struct s3_ent {
    long id;                    /* the id's place in the pool */
    long pid;                   /* the parentId's place in the pool, -1 null */
    int parent;                 /* its index, -1 a root (or a parent not in the file) */
    int target;                 /* label, context_edit: the target's index, -1 */
    long label;                 /* the entry's label in the pool (labels resolved), -1 none */
    long off, len;              /* the line in the file; off -1: not written yet */
    char *text;                 /* the line (no newline), 0 when not in RAM */
    signed char type, role;
} s3_ent;

typedef struct pi_s3 {
    cl_sys *sys;
    int persist;                /* 0: --no-session, nothing is written */
    int flushed;                /* the file exists */
    long fsize;                 /* its length: where the next line goes */
    char dir[256];              /* the session directory ("" none) */
    char file[320];
    char id[48];                /* the header's id */
    char cwd[256];
    char parent[320];           /* parentSession, "" none */
    char *head;                 /* the header line */
    s3_ent *e;
    int n, cap;
    char *pool;
    long pn, pcap;
    int leaf;                   /* -1: before the first entry */
    llm_ts (*now)(void *u);
    void *u;
    int utc;                    /* the clock is UTC: timestamps end in Z */
    unsigned long seed;         /* the id generator (cl_uuid) */
    int oom;
    long appends;               /* lines appended to a file (the tests' sentinel) */
    long reads;                 /* lines read back for an off-branch entry */
    char err[200];
} pi_s3;

void s3_init(pi_s3 *st, cl_sys *sys, llm_ts (*now)(void *u), void *u, int utc, unsigned long seed);
void s3_free(pi_s3 *st);
/* a new session in dir (made when the file is), cwd its header's; parent the
 * parentSession ("" none); id 0: a new uuidv7. persist 0: nothing written */
int s3_new(pi_s3 *st, const char *dir, const char *cwd, const char *parent, const char *id, int persist);
/* an existing file opened (the leaf its last entry); migrated to version 3
 * and rewritten when older; an empty file gets a header. 0, -1 with err
 * (not a session: the file is left as it was) */
int s3_open(pi_s3 *st, const char *path);

/* ---- appends: each a child of the leaf, then the leaf. Its index, -1 ---- */
/* the generic one: {"type":type,<pre>,"id","parentId","timestamp",<post>}; pre/post
 * comma-joined members without braces, 0 none */
int s3_append(pi_s3 *st, const char *type, const char *pre, const char *post);
int s3_message(pi_s3 *st, const char *msg, long n);
int s3_thinking(pi_s3 *st, const char *level);
int s3_model(pi_s3 *st, const char *provider, const char *model);
int s3_usage(pi_s3 *st, const char *kind, const char *provider, const char *model, const llm_usage *u,
             const char *note);
/* first_kept -1: this entry's own id (retain none); details, usage, system raw JSON or 0; from_hook -1 absent */
int s3_compaction(pi_s3 *st, const char *summary, int first_kept, long tokens_before, const char *details,
                  const char *usage, int from_hook, const char *system);
/* pi's branchWithSummary: the leaf moved to from (-1 the root) and the summary appended there */
int s3_branch_summary(pi_s3 *st, int from, const char *summary, const char *details, const char *usage, int from_hook);
int s3_custom(pi_s3 *st, const char *custom_type, const char *data);
/* content: a JSON string or array */
int s3_custom_message(pi_s3 *st, const char *custom_type, const char *content, int display, const char *details);
/* replacement: {"content":...} JSON or 0 (omit the target); pi's checks: -1 with err */
int s3_context_edit(pi_s3 *st, int target, const char *replacement);
/* label 0 or "": cleared */
int s3_label(pi_s3 *st, int target, const char *label);
int s3_session_info(pi_s3 *st, const char *name);

/* ---- reading ---- */
/* the entry's line (read back when not in RAM; 0 out of memory or unreadable) */
const char *s3_text(pi_s3 *st, int i, long *n);
const char *s3_id(const pi_s3 *st, int i);
int s3_find(const pi_s3 *st, const char *id);         /* index, -1 */
const char *s3_label_of(const pi_s3 *st, int i);    /* 0 none */
/* the path from the root to i (pi's getBranch), into a malloc'ed array: its length, -1 */
int s3_path(const pi_s3 *st, int i, int **out);
/* the latest session_info name, 0 none */
int s3_name(pi_s3 *st, char *out, long cap);

/* ---- the tree ---- */
/* the leaf moved (pi's branch / resetLeaf with -1); lines off the new branch leave RAM */
void s3_set_leaf(pi_s3 *st, int i);
/* pi's createBranchedSession: a new file in the same directory holding the
 * path to leaf (labels re-chained after it), parentSession the old file;
 * st becomes that session. 0, -1 */
int s3_branched(pi_s3 *st, int leaf);

/* ---- names and finding sessions ---- */
/* <data dir>/sessions/<pi's --<cwd>-- cut to 30> into out: 0, -1 */
int s3_dir(const char *data_dir, const char *cwd, char *out, long cap);
/* the newest .jsonl in dir (by mtime) whose header is a session: 0 with out, -1 none */
int s3_recent(cl_sys *sys, const char *dir, char *out, long cap);
/* the session in dir whose header id is id or starts with it: 0 with out, -1 none */
int s3_by_id(cl_sys *sys, const char *dir, const char *id, char *out, long cap);
/* every session in dir, newest first: calls fn for each (path, id, name or first
 * message); at most max. Their number */
int s3_list(cl_sys *sys, const char *dir, int max, void (*fn)(void *c, const char *path, const char *id,
                                                                const char *title), void *c);

/* ISO 8601 of t with milliseconds, Z when utc: out at least 32 bytes */
void s3_iso(llm_ts t, int utc, char *out);
/* an ISO 8601 time back to t (the zone ignored: the clock's frame): 0, -1 */
int s3_iso_read(const char *s, llm_ts *t);
/* the entry's type from its name (S3_UNKNOWN) */
int s3_type_of(const char *name);

#endif
