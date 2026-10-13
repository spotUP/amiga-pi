/* amiga-pi's tools and the gate on the host (plan phase D): pi's tools on
 * C:Claude's (claude/tools.c over sys_posix) on a temporary tree, the
 * shared truncation, --tools, the permission gate as an extension, and
 * the reachability test through pi_app: a recording that calls edit then
 * bash changes the file on disk. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "claude/datadir.h"
#include "claude/config.h"
#include "pi_stub.h"
#include "../amiga-pi/tools_pi.h"
#include "../amiga-pi/truncate.h"
#include "../amiga-pi/image.h"
#include "../amiga-pi/ext_gate.h"
#include "../amiga-pi/args.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/ansi.h"
#include "../amiga-pi/store.h"

static char dir[400], tmpd[410];
static sys_posix sp;
static cl_sys sys, csys;
static long writes;
static pi_tools pt;
static pi_session ses;

/* sys->write counted (an edit writes its file once) */
static int count_write(void *u, const char *p, const char *s, long n)
{
    writes++;
    return sys.write(u, p, s, n);
}

static void full(const char *name, char *out)
{
    strcpy(out, dir);
    strcat(out, "/");
    strcat(out, name);
}

static void put(const char *name, const char *s, long n)
{
    char p[600];
    FILE *f;
    full(name, p);
    f = fopen(p, "wb");
    if (!f)
        return;
    fwrite(s, 1, (size_t)(n < 0 ? (long)strlen(s) : n), f);
    fclose(f);
}

/* the file's bytes (malloc'ed, terminated), 0 none */
static char *get(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long k;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    k = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)k + 1);
    if (b)
        k = (long)fread(b, 1, (size_t)k, f);
    fclose(f);
    if (b)
        b[k] = 0;
    if (n)
        *n = k;
    return b;
}

static char *get_rel(const char *name)
{
    char p[600];
    full(name, p);
    return get(p, 0);
}

static void tools_up(void)
{
    sys_posix_init(&sp, &sys);
    csys = sys;
    csys.write = count_write;
    strcpy(tmpd, dir);
    strcat(tmpd, "/t");
    mkdir(tmpd, 0755);          /* the kept output apart from the tree the tools list */
    strcat(tmpd, "/");
    CHECK_INT(pi_tools_init(&pt, &csys, dir, tmpd, PI_SH_HOST), 0);
    memset(&ses, 0, sizeof(ses));
    ses.call_id = "call_t1";
}

/* one call of a tool: its first text block into text (terminated), is_error */
static int call(int kind, const char *args, char **text)
{
    jw c;
    jv v, b, t;
    jit it;
    int err = -1;
    jw_init(&c);
    pt.tool[kind].run(pt.tool[kind].u, &ses, args, (long)strlen(args), &c, &err);
    *text = 0;
    if (c.p && json_parse(c.p, c.n, &v) == 0) {
        json_iter(v, &it);
        if (json_next(&it, 0, &b) && json_get(b, "text", &t))
            *text = json_strdup(t, 0);
    }
    if (!*text) {
        *text = (char *)malloc(1);
        **text = 0;
    }
    jw_free(&c);
    return err;
}

static int has(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != 0;
}

/* PH-S: the structuredContent of a result (pi 1.0.4 bash.js / read.js), what tool_execution_end carries; "" none */
static void structured_is(const char *want)
{
    CHECK_STR(ses.structured.p ? ses.structured.p : "", want);
}

/* ---- D1: read and write through tools.c ---- */

static void test_read_write(void)
{
    char *t, *f;
    long r0 = pi_tool_runs[PT_READ], w0 = pi_tool_runs[PT_WRITE];
    /* write: a new file, then over it without a read (pi), then into new directories */
    CHECK_INT(call(PT_WRITE, "{\"path\":\"a.txt\",\"content\":\"one\\ntwo\\nthree\\n\"}", &t), 0);
    CHECK(has(t, "File created successfully at: "));
    free(t);
    f = get_rel("a.txt");
    CHECK_STR(f ? f : "", "one\ntwo\nthree\n");
    free(f);
    CHECK_INT(call(PT_WRITE, "{\"path\":\"a.txt\",\"content\":\"one\\ntwo\\nthree\\nfour\\n\"}", &t), 0);
    CHECK(has(t, "has been updated"));
    free(t);
    CHECK_INT(call(PT_WRITE, "{\"path\":\"sub/deep/b.txt\",\"content\":\"made\"}", &t), 0);
    free(t);
    f = get_rel("sub/deep/b.txt");
    CHECK_STR(f ? f : "", "made");
    free(f);
    CHECK_INT(pi_tool_runs[PT_WRITE] - w0, 3);
    /* read: whole, offset and limit, the rest named (pi's notice) */
    CHECK_INT(call(PT_READ, "{\"path\":\"a.txt\"}", &t), 0);
    CHECK_STR(t, "one\ntwo\nthree\nfour\n");
    structured_is("\"one\\ntwo\\nthree\\nfour\\n\"");        /* read: the text is the structured content */
    free(t);
    CHECK_INT(call(PT_READ, "{\"path\":\"a.txt\",\"offset\":2,\"limit\":1}", &t), 0);
    CHECK_STR(t, "two\n\n[3 more lines in file. Use offset=3 to continue.]");
    free(t);
    /* pi decodes any file as text: a NUL is text too; a missing one is node's ENOENT */
    put("bin.dat", "ab\0cd", 5);
    CHECK_INT(call(PT_READ, "{\"path\":\"bin.dat\"}", &t), 0);
    free(t);
    CHECK_INT(call(PT_READ, "{\"path\":\"nope.txt\"}", &t), 1);
    CHECK(has(t, "ENOENT: no such file or directory, access '") && has(t, "nope.txt'"));
    structured_is("");          /* a thrown error: none */
    free(t);
    /* pi's schema checks the arguments before anything runs */
    CHECK_INT(call(PT_READ, "{\"offset\":1}", &t), 1);
    CHECK(has(t, "Validation failed for tool \"read\""));
    free(t);
    CHECK_INT(pi_tool_runs[PT_READ] - r0, 4);
}

/* ---- D2: edit against the original ---- */

/* an edit that must fail: the error has want, the file is byte-identical and was not written */
static void edit_fails(const char *args, const char *want)
{
    char *t, *before = get_rel("e.txt"), *after;
    long w0 = writes;
    CHECK_INT(call(PT_EDIT, args, &t), 1);
    if (!has(t, want))
        printf("  edit error was: %s\n", t);
    CHECK(has(t, want));
    free(t);
    after = get_rel("e.txt");
    CHECK(before && after && !strcmp(before, after));
    CHECK_INT(writes - w0, 0);
    free(before);
    free(after);
}

static void test_edit(void)
{
    char *t, *f;
    long w0;
    put("e.txt", "one two three four five\nbeta\nbeta2\n", -1);
    /* a unique match */
    w0 = writes;
    CHECK_INT(call(PT_EDIT, "{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"five\",\"newText\":\"FIVE\"}]}", &t), 0);
    CHECK_STR(t, "Successfully replaced 1 block(s) in e.txt.");
    free(t);
    CHECK_INT(writes - w0, 1);
    f = get_rel("e.txt");
    CHECK_STR(f ? f : "", "one two three four FIVE\nbeta\nbeta2\n");
    free(f);
    /* the errors: nothing written, the file as it was */
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"one\",\"newText\":\"1\"},"
               "{\"oldText\":\"missing\",\"newText\":\"x\"}]}",
               "Could not find edits[1] in e.txt. The oldText must match exactly");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"missing\",\"newText\":\"x\"}]}",
               "Could not find the exact text in e.txt.");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"beta\",\"newText\":\"b\"}]}",
               "Found 2 occurrences of the text in e.txt. The text must be unique.");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"one two\",\"newText\":\"x\"},"
               "{\"oldText\":\"two three\",\"newText\":\"y\"}]}",
               "edits[0] and edits[1] overlap in e.txt. Merge them into one edit or target disjoint regions.");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"one\",\"newText\":\"1\"},"
               "{\"oldText\":\"\",\"newText\":\"y\"}]}",
               "edits[1].oldText must not be empty in e.txt.");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"four\",\"newText\":\"four\"}]}",
               "No changes made to e.txt.");
    edit_fails("{\"path\":\"e.txt\",\"edits\":[]}", "edits must contain at least one replacement");
    CHECK_INT(call(PT_EDIT, "{\"path\":\"none.txt\",\"edits\":[{\"oldText\":\"a\",\"newText\":\"b\"}]}", &t), 1);
    CHECK_STR(t, "Could not edit file: none.txt. Error code: ENOENT.");
    free(t);
    /* three edits matched against the original, applied in one write
     * (one after the other, "a1" -> "b1" would make b1 occur twice) */
    put("e.txt", "a1 b1 c1\n", -1);
    w0 = writes;
    CHECK_INT(call(PT_EDIT, "{\"path\":\"e.txt\",\"edits\":[{\"oldText\":\"c1\",\"newText\":\"C\"},"
                            "{\"oldText\":\"a1\",\"newText\":\"b1\"},{\"oldText\":\"b1\",\"newText\":\"B\"}]}",
                   &t),
              0);
    CHECK_STR(t, "Successfully replaced 3 block(s) in e.txt.");
    free(t);
    CHECK_INT(writes - w0, 1);
    f = get_rel("e.txt");
    CHECK_STR(f ? f : "", "b1 B C\n");
    free(f);
    /* pi's legacy forms: oldText/newText beside path, edits as a JSON string */
    CHECK_INT(call(PT_EDIT, "{\"path\":\"e.txt\",\"oldText\":\"C\",\"newText\":\"c\"}", &t), 0);
    free(t);
    CHECK_INT(call(PT_EDIT, "{\"path\":\"e.txt\",\"edits\":\"[{\\\"oldText\\\":\\\"B\\\",\\\"newText\\\":\\\"b\\\"}]\"}",
                   &t),
              0);
    free(t);
    f = get_rel("e.txt");
    CHECK_STR(f ? f : "", "b1 b c\n");
    free(f);
    /* a Latin-1 file stays Latin-1 */
    put("l.txt", "Gr\xfc\xdf" "e\n", -1);
    CHECK_INT(call(PT_EDIT, "{\"path\":\"l.txt\",\"edits\":[{\"oldText\":\"Gr\xc3\xbc\xc3\x9f" "e\",\"newText\":"
                            "\"Hej \xc3\xa5\"}]}",
                   &t),
              0);
    free(t);
    f = get_rel("l.txt");
    CHECK_STR(f ? f : "", "Hej \xe5\n");
    free(f);
}

/* pi's own edit cases (test/tools.test.ts "edit tool fuzzy matching", "edit tool CRLF handling"), generated */
static const struct { const char *name, *file, *args, *want; int fails; } pi_edit_cases[] = {
    { "trailing white space stripped",
      "line one   \nline two  \nline three\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"line one\\nline two\\n\", \"newText\": \"replaced\\n\"}]}",
      "replaced\nline three\n", 0 },
    { "fullwidth punctuation in Chinese text",
      "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c\n\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x88\xe4\xb8\x96\xe7\x95\x8c\xef\xbc\x89\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"\\u4f60\\u597d,\\u4e16\\u754c\\n\\u4f60\\u597d(\\u4e16\\u754c)\\n\", \"newText\": \"\\u4f60\\u597d\\uff0cpi\\n\\u4f60\\u597d(pi)\\n\"}]}",
      "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8cpi\n\xe4\xbd\xa0\xe5\xa5\xbd(pi)\n", 0 },
    { "compatibility-equivalent Unicode forms",
      "\xef\xbc\xa1\xef\xbc\xa2\xef\xbc\xa3\xef\xbc\x91\xef\xbc\x92\xef\xbc\x93\ncafe\xcc\x81\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"ABC123\\ncaf\\u00e9\\n\", \"newText\": \"XYZ789\\ncoffee\\n\"}]}",
      "XYZ789\ncoffee\n", 0 },
    { "smart single quotes",
      "console.log(\xe2\x80\x98hello\xe2\x80\x99);\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"console.log('hello');\", \"newText\": \"console.log('world');\"}]}",
      "console.log('world');\n", 0 },
    { "smart double quotes",
      "const msg = \xe2\x80\x9cHello World\xe2\x80\x9d;\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"const msg = \\\"Hello World\\\";\", \"newText\": \"const msg = \\\"Goodbye\\\";\"}]}",
      "const msg = \"Goodbye\";\n", 0 },
    { "Unicode dashes",
      "range: 1\xe2\x80\x93" "5\nbreak\xe2\x80\x94here\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"range: 1-5\\nbreak-here\", \"newText\": \"range: 10-50\\nbreak--here\"}]}",
      "range: 10-50\nbreak--here\n", 0 },
    { "non-breaking space",
      "hello\xc2\xa0world\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"hello world\", \"newText\": \"hello universe\"}]}",
      "hello universe\n", 0 },
    { "exact preferred over fuzzy",
      "const x = 'exact';\nconst y = 'other';\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"const x = 'exact';\", \"newText\": \"const x = 'changed';\"}]}",
      "const x = 'changed';\nconst y = 'other';\n", 0 },
    { "fuzzy in multi-edit mode",
      "console.log(\xe2\x80\x98hello\xe2\x80\x99);\nhello\xc2\xa0world\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"console.log('hello');\\n\", \"newText\": \"console.log('world');\\n\"}, {\"oldText\": \"hello world\\n\", \"newText\": \"hello universe\\n\"}]}",
      "console.log('world');\nhello universe\n", 0 },
    { "the right occurrence when the replacement equals a nearby line",
      "replace me   \nafter   \n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"replace me\\n\", \"newText\": \"after\\n\"}]}",
      "after\nafter   \n", 0 },
    { "untouched lines kept in fuzzy multi-edits",
      "keep before  \nfirst target  \nfirst after\nkeep middle   \nsecond target  \nsecond after\nkeep after  \n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"first target\\nfirst after\", \"newText\": \"FIRST\\nFIRST2\"}, {\"oldText\": \"second target\\nsecond after\", \"newText\": \"SECOND\\nSECOND2\"}]}",
      "keep before  \nFIRST\nFIRST2\nkeep middle   \nSECOND\nSECOND2\nkeep after  \n", 0 },
    { "LF oldText against CRLF content",
      "line one\r\nline two\r\nline three\r\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"line two\\n\", \"newText\": \"replaced line\\n\"}]}",
      "line one\r\nreplaced line\r\nline three\r\n", 0 },
    { "CRLF kept",
      "first\r\nsecond\r\nthird\r\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"second\\n\", \"newText\": \"REPLACED\\n\"}]}",
      "first\r\nREPLACED\r\nthird\r\n", 0 },
    { "LF kept",
      "first\nsecond\nthird\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"second\\n\", \"newText\": \"REPLACED\\n\"}]}",
      "first\nREPLACED\nthird\n", 0 },
    { "BOM kept",
      "\xef\xbb\xbf" "first\r\nsecond\r\nthird\r\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"second\\n\", \"newText\": \"REPLACED\\n\"}]}",
      "\xef\xbb\xbf" "first\r\nREPLACED\r\nthird\r\n", 0 },
    { "CRLF and BOM kept in multi-edit mode",
      "\xef\xbb\xbf" "first\r\nsecond\r\nthird\r\nfourth\r\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"second\\n\", \"newText\": \"SECOND\\n\"}, {\"oldText\": \"fourth\\n\", \"newText\": \"FOURTH\\n\"}]}",
      "\xef\xbb\xbf" "first\r\nSECOND\r\nthird\r\nFOURTH\r\n", 0 },
    { "not found even with fuzzy matching",
      "completely different content\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"this does not exist\", \"newText\": \"replacement\"}]}",
      "Could not find the exact text", 1 },
    { "duplicates after fuzzy normalisation",
      "hello world   \nhello world\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"hello world\", \"newText\": \"replaced\"}]}",
      "Found 2 occurrences", 1 },
    { "duplicates across CRLF/LF variants",
      "hello\r\nworld\r\n---\r\nhello\nworld\n",
      "{\"path\": \"f.txt\", \"edits\": [{\"oldText\": \"hello\\nworld\\n\", \"newText\": \"replaced\\n\"}]}",
      "Found 2 occurrences", 1 },
};

/* pi's edit: exact first, then the fuzzy normalisation (NFKC, line ends'
 * blanks, quotes, dashes, spaces) with the untouched lines kept, CRLF matched
 * as LF and restored, the BOM kept */
static void test_edit_pi(void)
{
    int i;
    char *t, *f;
    long n = 0;
    for (i = 0; i < (int)(sizeof(pi_edit_cases) / sizeof(pi_edit_cases[0])); i++) {
        int err;
        put("f.txt", pi_edit_cases[i].file, -1);
        err = call(PT_EDIT, pi_edit_cases[i].args, &t);
        f = get_rel("f.txt");
        if (pi_edit_cases[i].fails) {
            if (!err || !has(t, pi_edit_cases[i].want) || strcmp(f ? f : "", pi_edit_cases[i].file))
                printf("  pi edit case \"%s\": %s\n", pi_edit_cases[i].name, t);
            CHECK_INT(err, 1);
            CHECK(has(t, pi_edit_cases[i].want));
            CHECK_STR(f ? f : "", pi_edit_cases[i].file);
        } else {
            if (err || strcmp(f ? f : "", pi_edit_cases[i].want))
                printf("  pi edit case \"%s\": %s\n", pi_edit_cases[i].name, t);
            CHECK_INT(err, 0);
            CHECK(has(t, "Successfully replaced"));
            CHECK_STR(f ? f : "", pi_edit_cases[i].want);
        }
        free(t);
        free(f);
    }
    /* a NUL is text to pi: the file is edited, its other bytes as they were */
    put("f.txt", "a\0b\nc\n", 6);
    CHECK_INT(call(PT_EDIT, "{\"path\":\"f.txt\",\"edits\":[{\"oldText\":\"c\",\"newText\":\"d\"}]}", &t), 0);
    free(t);
    {
        char p[600];
        full("f.txt", p);
        f = get(p, &n);
    }
    CHECK_INT(n, 6);
    CHECK(f && !memcmp(f, "a\0b\nd\n", 6));
    free(f);
}

/* ---- D3: bash through sys->run ---- */

/* R1: bash's tool_execution_update records, on a clock the test moves (no sleeping for the throttle) */
static char upd[8][64];
static int nupd;
static unsigned long fake_ms, fake_step;

static unsigned long fake_clock(void *u)
{
    (void)u;
    fake_ms += fake_step;
    return fake_ms;
}

static void on_update(void *u, const pi_event *e)
{
    (void)u;
    if (e->type != PE_TOOL_UPDATE || nupd >= 8)
        return;
    upd[nupd][0] = 0;
    if (e->result_n < (long)sizeof(upd[0]))
        sprintf(upd[nupd], "%.*s", (int)e->result_n, e->result ? e->result : "");
    nupd++;
}

static void test_bash(void)
{
    char *t, *f, path[600];
    time_t t0;
    jw cmd;
    CHECK_INT(call(PT_BASH, "{\"command\":\"echo hi\"}", &t), 0);
    CHECK_STR(t, "hi\n");
    structured_is("{\"output\":\"hi\\n\",\"truncated\":false,\"exit_code\":0,\"wall_time_seconds\":0}");
    free(t);
    CHECK_INT(call(PT_BASH, "{\"command\":\"true\"}", &t), 0);
    CHECK_STR(t, "(no output)");
    structured_is("{\"output\":\"\",\"truncated\":false,\"exit_code\":0,\"wall_time_seconds\":0}");
    free(t);
    CHECK_INT(call(PT_BASH, "{\"command\":\"echo out; echo err 1>&2; exit 3\"}", &t), 1);
    CHECK(has(t, "out\n") && has(t, "err\n"));
    CHECK(has(t, "\n\nCommand exited with code 3"));
    CHECK(has(ses.structured.p, "\"exit_code\":3,") && has(ses.structured.p, "{\"output\":\"") &&
          !has(ses.structured.p, "Command exited"));          /* the output without the status */
    free(t);
    CHECK_INT(call(PT_BASH, "{\"command\":\"exit 4\"}", &t), 1);
    CHECK_STR(t, "(no output)\n\nCommand exited with code 4");
    structured_is("{\"output\":\"\",\"truncated\":false,\"exit_code\":4,\"wall_time_seconds\":0}");
    free(t);
    /* the time limit: stopped, not moved to the background */
    t0 = time(0);
    CHECK_INT(call(PT_BASH, "{\"command\":\"sleep 20\",\"timeout\":1}", &t), 1);
    CHECK_STR(t, "Command timed out after 1 seconds");      /* pi: no "(no output)" before a time-out */
    structured_is("");          /* pi throws on a time-out: an error result has no structured content */
    CHECK(time(0) - t0 < 10);
    free(t);
    /* Ctrl+C: the command stopped, the prompt too (run answers 1) */
    {
        jw c;
        int err = 0;
        static const char a[] = "{\"command\":\"sleep 20\"}";
        jw_init(&c);
        sp.brk = 1;
        t0 = time(0);
        CHECK_INT(pt.tool[PT_BASH].run(pt.tool[PT_BASH].u, &ses, a, (long)strlen(a), &c, &err), 1);
        CHECK_INT(err, 1);
        CHECK(has(c.p, "Command aborted"));
        CHECK(time(0) - t0 < 10);
        CHECK_INT(pt.t.stop, 0);
        jw_free(&c);
    }
    /* R1: an update per chunk, held back to 100 ms (renderers/bash.ts:19; bash.ts scheduleOutputUpdate), the one held
     * going out at the end (finishOutput): three chunks 0.4 s apart. The clock frozen: the first at once, the other two
     * held and sent as one; the clock 500 ms per look: every chunk sent. */
    {
        static const char a3[] = "{\"command\":\"echo a; sleep 0.4; echo b; sleep 0.4; echo c\"}";
        int step;
        ses.h.ms = fake_clock;
        pi_subscribe(&ses.em, on_update, 0);
        for (step = 0; step < 2; step++) {
            jw c;
            int err = 0;
            jw_init(&c);
            nupd = 0;
            fake_ms = 1000;
            fake_step = step ? 500 : 0;
            CHECK_INT(pt.tool[PT_BASH].run(pt.tool[PT_BASH].u, &ses, a3, (long)strlen(a3), &c, &err), 0);
            CHECK_INT(nupd, step ? 4 : 3);
            CHECK(has(upd[0], "[]"));
            CHECK(has(upd[1], "\"a\\n\"") && !has(upd[1], "b"));
            if (step) {
                CHECK(has(upd[2], "a\\nb\\n") && !has(upd[2], "c"));
                CHECK(has(upd[3], "a\\nb\\nc\\n"));
            } else
                CHECK(has(upd[2], "a\\nb\\nc\\n"));
            jw_free(&c);
        }
        pi_unsubscribe(&ses.em, on_update, 0);
        ses.h.ms = 0;
        CHECK(!pi_update_due(1, 1000, 1099));       /* pi: 100 ms */
        CHECK(pi_update_due(1, 1000, 1100));
        CHECK(pi_update_due(0, 1000, 1000));        /* the first update: lastUpdateAt 0 */
    }
    /* over 50 KB: the tail, the whole output in the kept file */
    jw_init(&cmd);
    jw_rawz(&cmd, "{\"command\":\"i=0; while [ $i -lt 3000 ]; do echo line$i-xxxxxxxxxxxxxxxxxxxxxx; i=$((i+1)); "
                  "done\"}");
    ses.call_id = "call_big|1";
    CHECK_INT(call(PT_BASH, cmd.p, &t), 0);
    ses.call_id = "call_t1";
    pi_cut_path(tmpd, "call_big|1", path, sizeof(path));
    CHECK(has(path, "amiga-pi-call_big_1.txt"));
    /* E8: a tool call id as long as Anthropic's still gives a 30-character file name, and different ids differ */
    {
        char p1[300], p2[300];
        pi_cut_path("T:", "toolu_01A09q90qw90lq917835lq9a", p1, sizeof(p1));
        pi_cut_path("T:", "toolu_01A09q90qw90lq917835lq9b", p2, sizeof(p2));
        CHECK_INT((int)strlen(p1) - 2, 30);
        CHECK(strcmp(p1, p2) != 0);
        pi_cut_path("T:", "call_big_1", p1, sizeof(p1));
        CHECK_STR(p1, "T:amiga-pi-call_big_1.txt");
    }
    CHECK(has(t, "line2999-") && !has(t, "line0-"));
    CHECK(has(t, "(50.0KB limit). Full output: "));
    CHECK(has(t, path));
    CHECK((long)strlen(t) < PI_MAX_BYTES + 300);
    free(t);
    /* pi: the notice belongs to the output, the status comes after it */
    {
        static const char c2[] = "{\"command\":\"i=0; while [ $i -lt 2500 ]; do echo n$i; i=$((i+1)); done; exit 2\"}";
        CHECK_INT(call(PT_BASH, c2, &t), 1);
        CHECK(has(t, "n2499\n\n[Showing lines 501-2500 of 2500. Full output: "));
        CHECK(has(t, ".txt]\n\nCommand exited with code 2"));
        free(t);
    }
    f = get(path, 0);
    CHECK(has(f, "line0-") && has(f, "line2999-"));
    free(f);
    jw_free(&cmd);
}

/* ---- D4: grep, find, ls ---- */

static void test_search(void)
{
    char *t;
    put("s1.txt", "alpha a.b\nAXB\n", -1);
    put("s2.c", "int axb;\n", -1);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"a.b\"}", &t), 0);
    CHECK(has(t, "s1.txt") && has(t, "s2.c"));
    free(t);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"a.b\",\"literal\":true}", &t), 0);
    CHECK_STR(t, "s1.txt:1: alpha a.b");
    free(t);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"axb\",\"ignoreCase\":true,\"glob\":\"*.txt\"}", &t), 0);
    CHECK(has(t, "AXB") && !has(t, "s2.c"));
    free(t);
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"*.c\"}", &t), 0);
    CHECK(has(t, "s2.c") && !has(t, "s1.txt"));
    free(t);
    CHECK_INT(call(PT_LS, "{}", &t), 0);
    CHECK(has(t, "a.txt\nbin.dat\n") && has(t, "sub/"));
    free(t);
    CHECK_INT(call(PT_LS, "{\"path\":\"sub\"}", &t), 0);
    CHECK_STR(t, "deep/");
    free(t);
    CHECK_INT(call(PT_LS, "{\"limit\":2}", &t), 0);
    CHECK(has(t, "[2 entries limit reached. Use limit=4 for more]"));
    free(t);
    CHECK_INT(call(PT_LS, "{\"path\":\"a.txt\"}", &t), 1);
    CHECK(has(t, "Not a directory: ") && has(t, "/a.txt"));
    free(t);
}

/* pi's own read, grep, find and ls cases (test/tools.test.ts), in "pc/" */
static void lines_file(const char *name, long n, const char *fmt_pad)
{
    jw b;
    long i;
    char num[16];
    jw_init(&b);
    for (i = 1; i <= n; i++) {
        if (i > 1)
            jw_raw(&b, "\n", 1);
        jw_rawz(&b, "Line ");
        cl_ltoa(i, num);
        jw_rawz(&b, num);
        if (fmt_pad) {
            jw_rawz(&b, ": ");
            jw_rawz(&b, fmt_pad);
        }
    }
    put(name, b.p ? b.p : "", b.n);
    jw_free(&b);
}

/* U1b: the `details` of a result (read.ts, edit.ts, ls.ts, find.ts, grep.ts, bash.ts), what the TUI's warnings and the edit box draw from. The
 * expected JSON is what pi 1.1.0's own tools return on the same tree (tools/pi_oracle/gen_tool_details.mjs); a truncation's text is compared
 * from the member after "content" (its start is the text cut) */
static void details_is(const char *want)
{
    CHECK_STR(ses.details.p ? ses.details.p : "", want);
}

static void details_tail_is(const char *start, const char *tail)
{
    const char *d = ses.details.p ? ses.details.p : "";
    const char *at = strstr(d, "\",\"truncated\":");
    CHECK(strncmp(d, start, strlen(start)) == 0);
    CHECK(at != 0);
    if (at)
        CHECK_STR(at + 2, tail);
}

static void test_details(void)
{
    char *t, pad[201];
    memset(pad, 'x', 200);
    pad[200] = 0;
    {
        char d[600];
        full("dt", d);
        mkdir(d, 0755);
        full("dt/files", d);
        mkdir(d, 0755);
    }
    lines_file("dt/big.txt", 2500, 0);
    jw_reset(&ses.details);
    CHECK_INT(call(PT_READ, "{\"path\":\"dt/big.txt\"}", &t), 0);
    free(t);
    details_tail_is("{\"truncation\":{\"content\":\"Line 1\\nLine 2\\nLine 3",
                    "\"truncated\":true,\"truncatedBy\":\"lines\",\"totalLines\":2500,\"totalBytes\":23892,\"outputLines\":2000,\"outputBytes\":18892,"
                    "\"lastLinePartial\":false,\"firstLineExceedsLimit\":false,\"maxLines\":2000,\"maxBytes\":51200}}");
    lines_file("dt/wide.txt", 500, pad);
    CHECK_INT(call(PT_READ, "{\"path\":\"dt/wide.txt\"}", &t), 0);
    free(t);
    details_tail_is("{\"truncation\":{\"content\":\"Line 1: xxxx",
                    "\"truncated\":true,\"truncatedBy\":\"bytes\",\"totalLines\":500,\"totalBytes\":105391,\"outputLines\":243,\"outputBytes\":51164,"
                    "\"lastLinePartial\":false,\"firstLineExceedsLimit\":false,\"maxLines\":2000,\"maxBytes\":51200}}");
    put("dt/small.txt", "one\ntwo\nthree\n", -1);
    CHECK_INT(call(PT_READ, "{\"path\":\"dt/small.txt\"}", &t), 0);
    free(t);
    details_is("");             /* pi: details undefined */
    CHECK_INT(call(PT_READ, "{\"path\":\"dt/big.txt\",\"offset\":10,\"limit\":5}", &t), 0);
    free(t);
    details_is("");
    put("dt/edit.txt", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n", -1);
    CHECK_INT(call(PT_EDIT, "{\"path\":\"dt/edit.txt\",\"edits\":[{\"oldText\":\"c\\n\",\"newText\":\"C\\nC2\\n\"},{\"oldText\":\"k\\n\",\"newText\":\"K\\n\"}]}", &t), 0);
    free(t);
    details_is("{\"diff\":\"  1 a\\n  2 b\\n- 3 c\\n+ 3 C\\n+ 4 C2\\n  4 d\\n  5 e\\n  6 f\\n  7 g\\n  8 h\\n  9 i\\n 10 j\\n-11 k\\n+12 K\\n 12 l\","
               "\"patch\":\"--- dt/edit.txt\\n+++ dt/edit.txt\\n@@ -1,12 +1,13 @@\\n a\\n b\\n-c\\n+C\\n+C2\\n d\\n e\\n f\\n g\\n h\\n i\\n j\\n-k\\n+K\\n l\\n\","
               "\"firstChangedLine\":3}");
    CHECK_INT(call(PT_EDIT, "{\"path\":\"dt/edit.txt\",\"edits\":[{\"oldText\":\"no such text\",\"newText\":\"x\"}]}", &t), 1);
    free(t);
    details_is("");             /* an error has none */
    {
        int i;
        for (i = 1; i <= 8; i++) {
            char n[] = "dt/files/fX.txt";
            n[10] = (char)('0' + i);
            put(n, "x\n", -1);
        }
    }
    CHECK_INT(call(PT_LS, "{\"path\":\"dt/files\",\"limit\":3}", &t), 0);
    free(t);
    details_is("{\"entryLimitReached\":3}");
    CHECK_INT(call(PT_LS, "{\"path\":\"dt/files\"}", &t), 0);
    free(t);
    details_is("");
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"*.txt\",\"path\":\"dt/files\",\"limit\":2}", &t), 0);
    free(t);
    details_is("{\"resultLimitReached\":2}");
    put("dt/g.txt", "match 0\nmatch 1\nmatch 2\nmatch 3\nmatch 4\nmatch 5\n", -1);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"match\",\"path\":\"dt/g.txt\",\"limit\":4}", &t), 0);
    free(t);
    details_is("{\"matchLimitReached\":4}");
    {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "match ");
        {
            int i;
            for (i = 0; i < 600; i++)
                jw_raw(&w, "y", 1);
        }
        jw_rawz(&w, "\n");
        put("dt/gl.txt", w.p, w.n);
        jw_free(&w);
    }
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"match\",\"path\":\"dt/gl.txt\"}", &t), 0);
    free(t);
    details_is("{\"linesTruncated\":true}");
    CHECK_INT(call(PT_BASH, "{\"command\":\"seq 1 3000\"}", &t), 0);
    free(t);
    {
        const char *d = ses.details.p ? ses.details.p : "";
        if (strncmp(d, "{\"truncation\":{\"content\":\"1001\\n1002\\n1003", 36))
            printf("  bash details: %.200s\n", d);
        CHECK(strncmp(d, "{\"truncation\":{\"content\":\"1001\\n1002\\n1003", 36) == 0);
        CHECK(strstr(d, "\"truncated\":true,\"truncatedBy\":\"lines\",\"totalLines\":3000,\"totalBytes\":13893,\"outputLines\":2000,\"outputBytes\":9999,"
                        "\"lastLinePartial\":false,\"firstLineExceedsLimit\":false,\"maxLines\":2000,\"maxBytes\":51200},\"fullOutputPath\":\"") != 0);
    }
    CHECK_INT(call(PT_BASH, "{\"command\":\"echo hi\"}", &t), 0);
    free(t);
    details_is("");
}


static void test_pi_cases(void)
{
    char *t, pad[201], p[600];
    memset(pad, 'x', 200);
    pad[200] = 0;
    full("pc", p);
    mkdir(p, 0755);
    /* read */
    put("pc/test.txt", "Hello, world!\nLine 2\nLine 3", -1);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/test.txt\"}", &t), 0);
    CHECK_STR(t, "Hello, world!\nLine 2\nLine 3");
    free(t);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/nonexistent.txt\"}", &t), 1);
    CHECK(has(t, "ENOENT"));
    free(t);
    lines_file("pc/large.txt", 2500, 0);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/large.txt\"}", &t), 0);
    CHECK(has(t, "Line 1\n") && has(t, "Line 2000") && !has(t, "Line 2001"));
    CHECK(has(t, "[Showing lines 1-2000 of 2500. Use offset=2001 to continue.]"));
    free(t);
    lines_file("pc/large-bytes.txt", 500, pad);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/large-bytes.txt\"}", &t), 0);
    CHECK(has(t, "Line 1:") && has(t, "[Showing lines 1-243 of 500 (50.0KB limit). Use offset=244 to continue.]"));
    free(t);
    lines_file("pc/offset-test.txt", 100, 0);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/offset-test.txt\",\"offset\":51}", &t), 0);
    CHECK(!has(t, "Line 50") && has(t, "Line 51") && has(t, "Line 100") && !has(t, "Use offset="));
    free(t);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/offset-test.txt\",\"limit\":10}", &t), 0);
    CHECK(has(t, "Line 1\n") && has(t, "Line 10") && !has(t, "Line 11"));
    CHECK(has(t, "[90 more lines in file. Use offset=11 to continue.]"));
    free(t);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/offset-test.txt\",\"offset\":41,\"limit\":20}", &t), 0);
    CHECK(!has(t, "Line 40") && has(t, "Line 41") && has(t, "Line 60") && !has(t, "Line 61"));
    CHECK(has(t, "[40 more lines in file. Use offset=61 to continue.]"));
    free(t);
    put("pc/short.txt", "Line 1\nLine 2\nLine 3", -1);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/short.txt\",\"offset\":100}", &t), 1);
    CHECK_STR(t, "Offset 100 is beyond end of file (3 lines total)");
    free(t);
    put("pc/not-an-image.png", "definitely not a png", -1);
    CHECK_INT(call(PT_READ, "{\"path\":\"pc/not-an-image.png\"}", &t), 0);
    CHECK_STR(t, "definitely not a png");
    free(t);
    /* grep */
    put("pc/example.txt", "first line\nmatch line\nlast line", -1);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"match\",\"path\":\"pc/example.txt\"}", &t), 0);
    CHECK_STR(t, "example.txt:2: match line");
    free(t);
    put("pc/context.txt", "before\nmatch one\nafter\nmiddle\nmatch two\nafter two", -1);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"match\",\"path\":\"pc/context.txt\",\"limit\":1,\"context\":1}", &t), 0);
    CHECK_STR(t, "context.txt-1- before\ncontext.txt:2: match one\ncontext.txt-3- after\n\n"
                 "[1 matches limit reached. Use limit=2 for more, or refine pattern]");
    free(t);
    put("pc/target.txt", "target\n", -1);
    CHECK_INT(call(PT_GREP, "{\"pattern\":\"--pre=payload.sh\",\"path\":\"pc\"}", &t), 0);
    CHECK_STR(t, "No matches found");
    free(t);
    /* find */
    full("pc/.secret", p);
    mkdir(p, 0755);
    put("pc/.secret/hidden.txt", "hidden", -1);
    put("pc/visible.txt", "visible", -1);
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"**/*.txt\",\"path\":\"pc\"}", &t), 0);
    CHECK(has(t, ".secret/hidden.txt\n") && has(t, "\nvisible.txt"));
    free(t);
    put("pc/.gitignore", "ignored.txt\n", -1);
    put("pc/ignored.txt", "ignored", -1);
    put("pc/kept.txt", "kept", -1);
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"**/*.txt\",\"path\":\"pc\"}", &t), 0);
    CHECK(has(t, "kept.txt") && !has(t, "ignored.txt"));
    free(t);
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"[\",\"path\":\"pc\"}", &t), 1);
    CHECK(has(t, "error parsing glob"));
    free(t);
    CHECK_INT(call(PT_FIND, "{\"pattern\":\"--help\",\"path\":\"pc\"}", &t), 0);
    CHECK_STR(t, "No files found matching pattern");
    free(t);
    /* ls */
    put("pc/.hidden-file", "secret", -1);
    full("pc/.hidden-dir", p);
    mkdir(p, 0755);
    CHECK_INT(call(PT_LS, "{\"path\":\"pc\"}", &t), 0);
    CHECK(has(t, ".hidden-file") && has(t, ".hidden-dir/"));
    free(t);
}

/* ls sorts as pi's localeCompare (ICU, English), checked with node 24:
 * names.sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase(), "en")) */
static void test_ls_order(void)
{
    static const char *const names[] = { "_a", ".hidden", "~x", "10", "1a", "9", "a_c", "a-b", "a.c", "ab", "aeon",
                                         "\xc3\xa6on", "apfel", "\xc3\x84pfel", "B", "eclair", "\xc3\xa9" "clair",
                                         "zeta", "Zulu", 0 };
    static const char *const shuffled[] = { "Zulu", "9", "\xc3\xa9" "clair", "a.c", "_a", "B", "aeon", "~x", "ab",
                                            "\xc3\x84pfel", "10", "a-b", ".hidden", "zeta", "1a", "apfel",
                                            "\xc3\xa6on", "a_c", "eclair", 0 };
    char p[600], *t;
    jw want;
    int i;
    full("lso", p);
    mkdir(p, 0755);
    for (i = 0; shuffled[i]; i++) {
        char n[100];
        strcpy(n, "lso/");
        strcat(n, shuffled[i]);
        put(n, "x", -1);
    }
    jw_init(&want);
    for (i = 0; names[i]; i++) {
        if (i)
            jw_raw(&want, "\n", 1);
        jw_rawz(&want, names[i]);
    }
    jw_raw(&want, "", 0);
    CHECK_INT(call(PT_LS, "{\"path\":\"lso\"}", &t), 0);
    CHECK_STR(t, want.p);
    free(t);
    jw_free(&want);
}

/* ---- read's images (pi's processImage) ---- */


/* a read's content: the text, and the one image's bytes and type (0 none) */
typedef struct rd_img {
    char *text, *mime;
    unsigned char *data;
    long n;
    int images;
} rd_img;

static long b64_val(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
           : c == '+' ? 62 : c == '/' ? 63 : -1;
}

static int read_img(const char *args, rd_img *r)
{
    jw c;
    jv v, b, x;
    jit it;
    int err = -1;
    memset(r, 0, sizeof(*r));
    jw_init(&c);
    pt.tool[PT_READ].run(pt.tool[PT_READ].u, &ses, args, (long)strlen(args), &c, &err);
    if (c.p && json_parse(c.p, c.n, &v) == 0) {
        json_iter(v, &it);
        while (json_next(&it, 0, &b)) {
            if (json_get(b, "type", &x) && json_streq(x, "text") && json_get(b, "text", &x))
                r->text = json_strdup(x, 0);
            else if (json_get(b, "type", &x) && json_streq(x, "image")) {
                char *d = 0;
                long dn = 0, i, acc = 0, bits = 0;
                r->images++;
                if (json_get(b, "mimeType", &x))
                    r->mime = json_strdup(x, 0);
                if (json_get(b, "data", &x) && (d = json_strdup(x, &dn)) != 0) {
                    r->data = (unsigned char *)malloc((size_t)dn + 1);
                    for (i = 0; i < dn && r->data; i++) {
                        long k = b64_val((unsigned char)d[i]);
                        if (k < 0)
                            continue;
                        acc = ((acc << 6) | k) & 0xffffL;
                        bits += 6;
                        if (bits >= 8) {
                            bits -= 8;
                            r->data[r->n++] = (unsigned char)((acc >> bits) & 0xff);
                        }
                    }
                }
                free(d);
            }
        }
    }
    if (!r->text) {
        r->text = (char *)malloc(1);
        r->text[0] = 0;
    }
    jw_free(&c);
    return err;
}

static void rd_free(rd_img *r)
{
    free(r->text);
    free(r->mime);
    free(r->data);
}

/* a 24-bit BMP of w x h: red where red(x, y), else blue */
static void put_bmp(const char *name, long w, long h)
{
    long row = (w * 3 + 3) & ~3L, n = 54 + row * h, x, y;
    unsigned char *b = (unsigned char *)calloc(1, (size_t)n);
    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    hdr[2] = (unsigned char)n;
    hdr[3] = (unsigned char)(n >> 8);
    hdr[4] = (unsigned char)(n >> 16);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)w;
    hdr[19] = (unsigned char)(w >> 8);
    hdr[22] = (unsigned char)h;
    hdr[23] = (unsigned char)(h >> 8);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(b, hdr, 54);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char *q = b + 54 + y * row + x * 3;
            q[0] = 255;         /* blue (BGR) */
        }
    put(name, (const char *)b, n);
    free(b);
}

static void test_images(void)
{
    /* pi's 1x1 PNG (tools.test.ts) under a .txt name: the type is the file's magic */
    static const unsigned char png1[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8, 0x0f, 0x00, 0x01, 0x04, 0x01, 0x00,
        0x5f, 0xe5, 0xc3, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };
    /* pi's createTinyBmp1x1Red24bpp */
    unsigned char bmp1[58];
    /* a 1x1 lossless WebP, and a WebP header (VP8X) of 3000x3000 */
    static const unsigned char webp1[] = { 'R', 'I', 'F', 'F', 0x1a, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L',
                                           0x0d, 0, 0, 0, 0x2f, 0, 0, 0, 0x10, 0x07, 0x10, 0x11, 0x11, 0x88, 0x88,
                                           0xfe, 0x07, 0 };
    static const unsigned char webpbig[] = { 'R', 'I', 'F', 'F', 0x16, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X',
                                             0x0a, 0, 0, 0, 0, 0, 0, 0, 0xb7, 0x0b, 0, 0xb7, 0x0b, 0 };
    char p[600], *f;
    long fn = 0;
    rd_img r;
    int w = 0, h = 0;
    unsigned char *px;
    full("im", p);
    mkdir(p, 0755);
    put("im/image.txt", (const char *)png1, (long)sizeof(png1));
    CHECK_INT(read_img("{\"path\":\"im/image.txt\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/png]");
    CHECK_INT(r.images, 1);
    CHECK_STR(r.mime ? r.mime : "", "image/png");
    CHECK(r.n == (long)sizeof(png1) && !memcmp(r.data, png1, sizeof(png1)));
    rd_free(&r);
    memset(bmp1, 0, sizeof(bmp1));
    bmp1[0] = 'B';
    bmp1[1] = 'M';
    bmp1[2] = 58;
    bmp1[10] = 54;
    bmp1[14] = 40;
    bmp1[18] = 1;
    bmp1[22] = 1;
    bmp1[26] = 1;
    bmp1[28] = 24;
    bmp1[34] = 4;
    bmp1[56] = 0xff;
    put("im/image.bmp", (const char *)bmp1, 58);
    CHECK_INT(read_img("{\"path\":\"im/image.bmp\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/png]\n[Image converted from image/bmp to image/png.]");
    CHECK_STR(r.mime ? r.mime : "", "image/png");
    CHECK(r.n > 8 && r.data[0] == 0x89);
    px = r.data ? pi_stb_load(r.data, (int)r.n, &w, &h) : 0;
    CHECK(px && w == 1 && h == 1 && px[0] == 255 && px[1] == 0 && px[2] == 0);
    pi_stb_free(px);
    rd_free(&r);
    /* wider than 2000: scaled (Lanczos3), sent as PNG, pi's notes */
    put_bmp("im/wide.bmp", 2400, 10);
    CHECK_INT(read_img("{\"path\":\"im/wide.bmp\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/png]\n[Image converted from image/bmp to image/png.]\n"
                      "[Image: original 2400x10, displayed at 2000x8. Multiply coordinates by 1.20 to map to "
                      "original image.]");
    px = r.data ? pi_stb_load(r.data, (int)r.n, &w, &h) : 0;
    CHECK(px && w == 2000 && h == 8 && px[2] == 255 && px[0] == 0);
    pi_stb_free(px);
    rd_free(&r);
    /* a JPEG with EXIF orientation 6: turned first (1200x2400), then scaled to 1000x2000;
     * the red top-left quarter ends up at the top right */
    strcpy(p, PI_TESTS "/img/rot6.jpg");
    f = get(p, &fn);
    CHECK(f != 0);
    put("im/rot6.jpg", f ? f : "", fn);
    free(f);
    CHECK_INT(read_img("{\"path\":\"im/rot6.jpg\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/png]\n[Image: original 1200x2400, displayed at 1000x2000. Multiply "
                      "coordinates by 1.20 to map to original image.]");
    CHECK_STR(r.mime ? r.mime : "", "image/png");
    px = r.data ? pi_stb_load(r.data, (int)r.n, &w, &h) : 0;
    CHECK(px && w == 1000 && h == 2000);
    if (px && w == 1000 && h == 2000) {
        const unsigned char *tr = px + (100 * 1000 + 900) * 4, *tl = px + (100 * 1000 + 100) * 4,
                            *br = px + (1900 * 1000 + 900) * 4;
        CHECK(tr[0] > 200 && tr[2] < 60);
        CHECK(tl[2] > 200 && tl[0] < 60);
        CHECK(br[2] > 200 && br[0] < 60);
    }
    pi_stb_free(px);
    rd_free(&r);
    /* a GIF that fits: its own bytes */
    strcpy(p, PI_TESTS "/img/small.gif");
    f = get(p, &fn);
    put("im/small.gif", f ? f : "", fn);
    CHECK_INT(read_img("{\"path\":\"im/small.gif\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/gif]");
    CHECK(f && r.n == fn && !memcmp(r.data, f, (size_t)fn));
    free(f);
    rd_free(&r);
    /* WebP: passed through when it fits; no WebP decoder, so a big one is omitted */
    put("im/one.webp", (const char *)webp1, (long)sizeof(webp1));
    CHECK_INT(read_img("{\"path\":\"im/one.webp\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/webp]");
    CHECK_INT(r.images, 1);
    rd_free(&r);
    put("im/big.webp", (const char *)webpbig, (long)sizeof(webpbig));
    CHECK_INT(read_img("{\"path\":\"im/big.webp\"}", &r), 0);
    CHECK_STR(r.text, "Read image file [image/webp]\n[Image omitted: could not be resized below the inline image "
                      "size limit.]");
    CHECK_INT(r.images, 0);
    rd_free(&r);
    /* an animated PNG is not an image to pi: text */
    {
        unsigned char apng[60];
        memcpy(apng, png1, 33);
        memcpy(apng + 33, "\0\0\0\x08" "acTL\0\0\0\x01\0\0\0\0\0\0\0\0", 20);
        put("im/anim.png", (const char *)apng, 53);
        CHECK_INT(read_img("{\"path\":\"im/anim.png\"}", &r), 0);
        CHECK_INT(r.images, 0);
        CHECK(!has(r.text, "Read image file"));
        rd_free(&r);
    }
    CHECK_INT(pi_image_orientation((const unsigned char *)"\xff\xd8\xff\xd9", 4), 1);
}

/* ---- D4: which tools a session gets ---- */

static void names_of(char *out)
{
    int i;
    out[0] = 0;
    for (i = 0; i < ses.ntools; i++) {
        if (i)
            strcat(out, ",");
        strcat(out, ses.tools[i]->name);
    }
}

static void test_select(void)
{
    char err[100], got[100];
    pi_args a;
    char *argv[] = { "amiga-pi", "--tools", "read,grep", "--no-gate", "x", 0 };
    char *argv2[] = { "amiga-pi", "--no-tools", "--gate", "x", 0 };
    char *argv3[] = { "amiga-pi", "-t", "read,-write", "x", 0 };
    char *argv4[] = { "amiga-pi", "--tools", "-write,+ls", "x", 0 };
    char *argv5[] = { "amiga-pi", "-nbt", "x", 0 };
    char *argv6[] = { "amiga-pi", "-xt", "bash, mcp__*", "x", 0 };
    char *argv7[] = { "amiga-pi", "--exclude-tools", 0 };
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit,write");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "read,grep,find,ls", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,grep,find,ls");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "+ls", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit,write,ls");
    memset(&ses, 0, sizeof(ses));
    pi_tools_defaults(&pt, 0, 1);                    /* --no-tools */
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    CHECK_INT(ses.ntools, 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "+ls,+read", err, sizeof(err)), 0);       /* modifiers on the empty defaults */
    names_of(got);
    CHECK_STR(got, "read,ls");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "grep", err, sizeof(err)), 0);            /* an allowlist beats --no-tools */
    names_of(got);
    CHECK_STR(got, "grep");
    pi_tools_defaults(&pt, 0, 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "*", err, sizeof(err)), 0);
    CHECK_INT(ses.ntools, PT_COUNT);
    memset(&ses, 0, sizeof(ses));
    /* Q16: pi 1.1.0 (sdk.ts:289-294) drops a name it has no tool for without a word: no error, no stream */
    CHECK_INT(pi_tools_add(&pt, &ses, "read,rm", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "+rm", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit,write");
    /* Q15: user bash (!cmd, RPC bash) runs the backend whatever the list: pi's executeBash (agent-session.ts:3841-3858)
     * uses createLocalBashOperations, not the active tool */
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "grep", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "grep");
    {
        jw out;
        int code = -1, stopped = -1;
        jw_init(&out);
        CHECK_INT(pi_bash_exec(&ses, "echo hi", &out, &code, &stopped), 0);
        CHECK_STR(out.p ? out.p : "", "hi\n");
        CHECK_INT(code, 0);
        jw_free(&out);
    }
    /* pi 1.1.0 (P11-I7): only +name/-name entries change the default selection; "-write" is read, bash, edit */
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "-write", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "-bash,+grep,-nothere", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,edit,write,grep");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, " +ls , -read ,", err, sizeof(err)), 0);        /* entries trimmed, empty ones dropped */
    names_of(got);
    CHECK_STR(got, "bash,edit,write,ls");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "read,+ls", err, sizeof(err)), -1);
    CHECK_STR(err, "tool names cannot be mixed with +name or -name entries");
    CHECK_INT(pi_tools_add(&pt, &ses, "+l*", err, sizeof(err)), -1);
    CHECK_STR(err, "+name and -name entries take exact tool names, not patterns: +l*");
    /* defaultTools (settings.json): the same modifiers on the defaults; plain names replace them; [] is none; --tools
     * modifiers apply on top of the setting */
    pi_tools_defaults(&pt, "-write,+ls", 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit,ls");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "-ls,+grep", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit,grep");
    pi_tools_defaults(&pt, "read,grep", 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,grep");
    pi_tools_defaults(&pt, "", 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    CHECK_INT(ses.ntools, 0);
    pi_tools_defaults(&pt, 0, 0);
    /* L4: --tools patterns select built-in tools as well (createToolNameMatcher: "gre*" is grep) */
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "read,gre*", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,grep");
    CHECK(pi_tool_glob("mcp__*", "mcp__fake__echo") && !pi_tool_glob("mcp__*", "mcp_x") && pi_tool_glob("a*b*c", "aXXbYc") &&
          !pi_tool_glob("read", "reads") && pi_tool_glob("*", ""));
    /* L4: whether a tool an extension registers may join (sdk.ts:282-294 allowedToolNames, agent-session.ts _isAllowedTool):
     * no flags yes; --no-tools none; a list of names and patterns only what it names (--no-tools or not); only +name / -name
     * entries every name, or with --no-tools the ones they add */
    CHECK(pi_tools_ext_allowed(&pt, "", "greet") && pi_tools_ext_allowed(&pt, 0, "greet"));
    CHECK(!pi_tools_ext_allowed(&pt, "read", "greet") && pi_tools_ext_allowed(&pt, "read,greet", "greet"));
    CHECK(pi_tools_ext_allowed(&pt, "read,gre*", "greet") && !pi_tools_ext_allowed(&pt, "read,gre*", "other"));
    CHECK(pi_tools_ext_allowed(&pt, "-greet", "greet") && pi_tools_ext_allowed(&pt, "+ls", "greet"));
    pi_tools_defaults(&pt, 0, 1);
    CHECK(!pi_tools_ext_allowed(&pt, 0, "greet") && !pi_tools_ext_allowed(&pt, "", "greet"));
    CHECK(pi_tools_ext_allowed(&pt, "+greet", "greet") && !pi_tools_ext_allowed(&pt, "+read", "greet"));
    CHECK(!pi_tools_ext_allowed(&pt, "+greet,-greet", "greet") && pi_tools_ext_allowed(&pt, "greet", "greet"));
    pi_tools_defaults(&pt, 0, 0);
    /* L15: --no-builtin-tools empties the defaults and keeps extension and MCP tools (noTools "builtin", sdk.ts:282-291);
     * --exclude-tools removes from every source after all other selection (sdk.ts:292-294, _isAllowedTool) */
    pi_tools_defaults(&pt, 0, PI_NO_TOOLS_BUILTIN);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    CHECK_INT(ses.ntools, 0);
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "+ls", err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "ls");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "grep", err, sizeof(err)), 0);                /* an allowlist still selects */
    names_of(got);
    CHECK_STR(got, "grep");
    CHECK(pi_tools_ext_allowed(&pt, 0, "greet") && pi_tools_ext_allowed(&pt, "+ls", "greet"));
    CHECK(pi_tools_mcp_ok(&pt, 0, "mcp__s__t", 1));
    pi_tools_defaults(&pt, 0, PI_NO_TOOLS_NONE);
    pi_tools_exclude(&pt, " write , gre* ,");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, 0, err, sizeof(err)), 0);
    names_of(got);
    CHECK_STR(got, "read,bash,edit");
    memset(&ses, 0, sizeof(ses));
    CHECK_INT(pi_tools_add(&pt, &ses, "*", err, sizeof(err)), 0);                   /* after the allowlist: still excluded */
    names_of(got);
    CHECK_STR(got, "read,bash,edit,find,ls");
    CHECK(!pi_tools_ext_allowed(&pt, 0, "greet") && !pi_tools_ext_allowed(&pt, "greet", "grep2") && pi_tools_ext_allowed(&pt, 0, "other"));
    CHECK(!pi_tools_ext_allowed(&pt, "+greet", "greet"));
    pi_tools_exclude(&pt, "mcp__*,tool_search");
    CHECK(!pi_tools_mcp_ok(&pt, 0, "mcp__fake__echo", 1) && !pi_tools_mcp_ok(&pt, "mcp__fake__echo", "mcp__fake__echo", 1));
    CHECK(!pi_tools_ext_allowed(&pt, 0, "tool_search") && pi_tools_ext_allowed(&pt, 0, "bash"));
    pi_tools_exclude(&pt, "mcp__fake__echo");
    CHECK(!pi_tools_mcp_ok(&pt, 0, "mcp__fake__echo", 1) && pi_tools_mcp_ok(&pt, 0, "mcp__fake__other", 1));
    pi_tools_exclude(&pt, 0);
    /* MCP tools under --tools / --no-tools (agent-session.ts:503-506 _allowlistFiltersMcp, 1537 _isAllowedTool, 1549
     * _isActivatable): no allowlist keeps them; a list naming an mcp__ entry or empty filters them; any other list keeps them
     * registered but a direct one is declared only when named, a deferred one is loadable only when tool_search is allowed */
    CHECK(pi_tools_mcp_ok(&pt, 0, "mcp__fake__echo", 1) && pi_tools_mcp_ok(&pt, "+ls", "mcp__fake__echo", 1));
    CHECK(pi_tools_mcp_ok(&pt, "read,mcp__fake__*", "mcp__fake__echo", 1) && !pi_tools_mcp_ok(&pt, "read,mcp__fake__*", "mcp__x__y", 0));
    CHECK(!pi_tools_mcp_ok(&pt, "read", "mcp__fake__echo", 1));                     /* direct and unnamed: not declared */
    CHECK(!pi_tools_mcp_ok(&pt, "read", "mcp__fake__echo", 0));                     /* deferred, no tool_search in the list */
    CHECK(pi_tools_mcp_ok(&pt, "read,tool_search", "mcp__fake__echo", 0) && !pi_tools_mcp_ok(&pt, "read,tool_search", "mcp__fake__echo", 1));
    pi_tools_defaults(&pt, 0, PI_NO_TOOLS_ALL);
    CHECK(!pi_tools_mcp_ok(&pt, 0, "mcp__fake__echo", 0) && !pi_tools_mcp_ok(&pt, "", "mcp__fake__echo", 1));
    CHECK(pi_tools_mcp_ok(&pt, "+mcp__fake__echo", "mcp__fake__echo", 1) && !pi_tools_mcp_ok(&pt, "+mcp__fake__echo", "mcp__x__y", 0));
    CHECK(pi_tools_mcp_ok(&pt, "+tool_search", "mcp__fake__echo", 0));              /* modifiers add tool_search: not an mcp__ entry */
    pi_tools_defaults(&pt, 0, PI_NO_TOOLS_NONE);
    CHECK_INT(pi_args_parse(&a, 3, argv5), 0);
    CHECK(a.no_builtin_tools && !a.no_tools && !a.exclude[0]);
    CHECK_INT(pi_args_parse(&a, 4, argv6), 0);
    CHECK_STR(a.exclude, "bash, mcp__*");
    CHECK_INT(pi_args_parse(&a, 2, argv7), -1);
    CHECK_STR(a.err, "--exclude-tools needs a value");
    CHECK_INT(pi_args_parse(&a, 5, argv), 0);
    CHECK_STR(a.tools, "read,grep");
    CHECK_INT(a.gate, 0);
    CHECK_INT(pi_args_parse(&a, 4, argv2), 0);
    CHECK_INT(a.no_tools, 1);
    CHECK_STR(a.tools, "");
    CHECK_INT(a.gate, 1);
    CHECK_INT(pi_args_parse(&a, 4, argv3), -1);
    CHECK_STR(a.err, "-t: tool names cannot be mixed with +name or -name entries");
    CHECK_INT(pi_args_parse(&a, 4, argv4), 0);
    CHECK_STR(a.tools, "-write,+ls");
    memset(&ses, 0, sizeof(ses));
    ses.call_id = "call_t1";
}

/* ---- P11-I12: the escape sequences of a user command's output (pi 1.1.0 utils/ansi.ts, core/bash-executor.ts:73-115) ---- */

/* the output as pi's executor builds it from chunks: a chunk's unfinished sequence waits for the next one */
static void ansi_chunked(const char *const *chunks, jw *out)
{
    jw pend, buf;
    int i;
    jw_init(&pend);
    for (i = 0; chunks[i]; i++) {
        long keep;
        jw_init(&buf);
        jw_raw(&buf, pend.p ? pend.p : "", pend.n);
        jw_raw(&buf, chunks[i], (long)strlen(chunks[i]));
        keep = pi_ansi_split(buf.p, buf.n);
        jw_reset(&pend);
        jw_raw(&pend, buf.p + keep, buf.n - keep);
        pi_ansi_clean(buf.p, keep, out);
        jw_free(&buf);
    }
    pi_ansi_clean(pend.p ? pend.p : "", pend.n, out);
    jw_free(&pend);
}

static void test_user_bash_ansi(void)
{
    static const char *const split1[] = { "ab\033[3", "1mred\033[0", "m!\n", 0 };         /* a CSI split three times */
    static const char *const split2[] = { "x\033", "[31mred", 0 };                         /* ESC alone at a chunk end */
    static const char *const split3[] = { "a\033]0;ti", "tle\007b", 0 };                   /* an OSC split before its BEL */
    static const char *const split4[] = { "a\033]0;title\033", "\\b", 0 };                 /* ... before its ESC \ */
    jw o;
    char *t;
    int code = 0, stopped = 0;
    jw_init(&o);
    CHECK_INT(pi_ansi_split("plain", 5), 5);
    CHECK_INT(pi_ansi_split("ab\033[31m", 7), 7);                  /* complete */
    CHECK_INT(pi_ansi_split("ab\033[31", 6), 2);                   /* CSI without its final byte */
    CHECK_INT(pi_ansi_split("ab\033", 3), 2);
    CHECK_INT(pi_ansi_split("a\033]0;title", 10), 1);              /* OSC without its terminator */
    CHECK_INT(pi_ansi_split("a\033]0;title\007", 11), 11);
    ansi_chunked(split1, &o);
    CHECK_STR(o.p, "abred!\n");                                     /* no stray "m" nor "1m" */
    jw_reset(&o);
    ansi_chunked(split2, &o);
    CHECK_STR(o.p, "xred");
    jw_reset(&o);
    ansi_chunked(split3, &o);
    CHECK_STR(o.p, "ab");
    jw_reset(&o);
    ansi_chunked(split4, &o);
    CHECK_STR(o.p, "ab");
    jw_reset(&o);
    pi_ansi_clean("a\r\nb\001\033[1;32mc\033[0m\033[K\xef\xbf\xb9z", 24, &o);      /* CR, binary, SGR, EL, U+FFF9 */
    CHECK_STR(o.p, "a\nbcz");
    jw_free(&o);
    /* the user's command (`!cmd`, RPC bash) goes through it: the whole path, pi_bash_exec on the real bash tool */
    memset(&ses, 0, sizeof(ses));
    ses.call_id = "call_t1";
    CHECK_INT(pi_tool_add(&ses, &pt.tool[PT_BASH]), 0);
    jw_init(&o);
    CHECK_INT(pi_bash_exec(&ses, "printf '\\033[31mred\\033[0m \\033]0;title\\007ok\\r\\n'", &o, &code, &stopped), 0);
    t = o.p;
    CHECK_STR(t, "red ok\n");
    CHECK_INT(code, 0);
    jw_free(&o);
    memset(&ses, 0, sizeof(ses));
    ses.call_id = "call_t1";
}

/* ---- D5: the one truncation ---- */

static void test_truncate(void)
{
    pi_trunc r;
    char sz[16], *big, *t, path[600];
    long i;
    jw w;
    /* head by lines: 2500 short lines -> the first 2000 */
    jw_init(&w);
    for (i = 0; i < 2500; i++)
        jw_rawz(&w, "x\n");
    pi_truncate(w.p, w.n, 0, PI_MAX_LINES, PI_MAX_BYTES, &r);
    CHECK_INT(r.by, PT_LINES);
    CHECK_INT(r.total_lines, 2500);
    CHECK_INT(r.out_lines, 2000);
    CHECK_INT(r.n, 3999);
    pi_truncate(w.p, w.n, 1, PI_MAX_LINES, PI_MAX_BYTES, &r);
    CHECK_INT(r.out_lines, 2000);
    CHECK_INT(r.off, 1000);
    jw_free(&w);
    /* fits: nothing cut */
    pi_truncate("a\nb\n", 4, 0, PI_MAX_LINES, PI_MAX_BYTES, &r);
    CHECK_INT(r.by, PT_NONE);
    CHECK_INT(r.total_lines, 2);
    /* by bytes; the first line over the limit; the tail's one line cut at its front */
    pi_truncate("aaaa\nbbbb\ncccc", 14, 0, 100, 9, &r);
    CHECK_INT(r.by, PT_BYTES);
    CHECK_INT(r.n, 9);
    CHECK_INT(r.out_lines, 2);
    pi_truncate("aaaaaaaaaaaa\nb", 14, 0, 100, 5, &r);
    CHECK_INT(r.first_over, 1);
    CHECK_INT(r.n, 0);
    pi_truncate("a\nbbbbbbbbbbbb", 14, 1, 100, 5, &r);
    CHECK_INT(r.partial, 1);
    CHECK_INT(r.n, 5);
    CHECK_INT(r.off, 9);
    pi_size(512, sz, sizeof(sz));
    CHECK_STR(sz, "512B");
    pi_size(PI_MAX_BYTES, sz, sizeof(sz));
    CHECK_STR(sz, "50.0KB");
    pi_size(1536L * 1024, sz, sizeof(sz));
    CHECK_STR(sz, "1.5MB");
    /* read has its own notice (pi: an offset hint, no file): a 60 KB file */
    big = (char *)malloc(62000);
    for (i = 0; i < 61999; i++)
        big[i] = i % 60 == 59 ? '\n' : 'r';
    big[61999] = 0;
    put("big.txt", big, -1);
    free(big);
    ses.call_id = "call_read";
    CHECK_INT(call(PT_READ, "{\"path\":\"big.txt\"}", &t), 0);
    ses.call_id = "call_t1";
    CHECK(has(t, "\n\n[Showing lines 1-853 of 1034 (50.0KB limit). Use offset=854 to continue.]"));
    pi_cut_path(tmpd, "call_read", path, sizeof(path));
    CHECK(!has(t, path));
    free(t);
}

/* ---- D6: the gate ---- */

static long asked;
static int answer;

static int ask_stub(void *u, int tool, const char *name, const char *what, int outside)
{
    (void)u;
    (void)tool;
    (void)name;
    (void)what;
    (void)outside;
    asked++;
    return answer;
}

static llm_ts fixed_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L;
    t.ms = 0;
    return t;
}

#define CH(delta, fr) "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"m\"," \
                      "\"choices\":[{\"index\":0,\"delta\":" delta ",\"finish_reason\":" fr "}]}\n\n"

/* one answer calling the tools (name, arguments pairs, 0-ended) as an openai stream */
static void calls_sse(const char *const *c)
{
    jw w, d;
    int i;
    jw_init(&w);
    for (i = 0; c[2 * i]; i++) {
        char id[16];
        jw_init(&d);
        jw_rawz(&d, "{\"tool_calls\":[{\"index\":");
        jw_long(&d, i);
        jw_rawz(&d, ",\"id\":");
        cl_copy(id, "call_", sizeof(id));
        cl_cat(id, c[2 * i], sizeof(id));
        jw_strz(&d, id);
        jw_rawz(&d, ",\"type\":\"function\",\"function\":{\"name\":");
        jw_strz(&d, c[2 * i]);
        jw_rawz(&d, ",\"arguments\":");
        jw_strz(&d, c[2 * i + 1]);
        jw_rawz(&d, "}}]}");
        jw_rawz(&w, "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"m\","
                    "\"choices\":[{\"index\":0,\"delta\":");
        jw_raw(&w, d.p, d.n);
        jw_rawz(&w, ",\"finish_reason\":null}]}\n\n");
        jw_free(&d);
    }
    jw_rawz(&w, CH("{}", "\"tool_calls\"") "data: [DONE]\n\n");
    ps_sse_text(w.p, w.n, 0);
    jw_free(&w);
}

static llm_model model;
static pi_gate gate;

static void gate_up(int on, const char *settings)
{
    llm_hooks h;
    char err[80];
    int g = -1;
    ps_reset();
    ps_pool_up(LLM_POOL);
    memset(&h, 0, sizeof(h));
    h.poll = ps_poll;
    h.ms = ps_ms;
    h.sleep = ps_sleep;
    h.resolve = ps_resolve;
    model = ps_model_openai("http://192.168.1.10:11434/v1");
    pi_session_init(&ses, &model, 0, &ps_pool, &h);
    ses.now = fixed_now;
    pi_tools_add(&pt, &ses, 0, err, sizeof(err));
    pi_gate_init(&gate, &pt, &ses);
    if (settings)
        CHECK_INT(pi_gate_settings(&gate, settings, (long)strlen(settings), "settings.json", &g), 0);
    gate.on = on;
    pi_ext_add(&ses.ext, &gate.ext);
    ses.ask = ask_stub;
    asked = 0;
    answer = ASK_ONCE;
    memset(&pt.t.perm, 0, sizeof(pt.t.perm));
}

static void gate_down(void)
{
    pi_session_free(&ses);
    pi_gate_free(&gate);
}

static const char *const three[] = { "read", "{\"path\":\"a.txt\"}", "write", "{\"path\":\"g.txt\",\"content\":\"g\"}",
                                     "bash", "{\"command\":\"echo gated\"}", 0, 0 };

static void test_gate(void)
{
    long w0, b0;
    char *f;
    int g = -1;
    /* off: a three-tool turn asks nothing, all three run */
    gate_up(0, 0);
    calls_sse(three);
    ps_sse("../pi/openai_final.sse", 0);
    w0 = pi_tool_runs[PT_WRITE];
    b0 = pi_tool_runs[PT_BASH];
    CHECK_INT(pi_prompt(&ses, "do three things"), LS_STOP);
    CHECK_INT(asked, 0);
    CHECK_INT(pi_tool_runs[PT_WRITE] - w0, 1);
    CHECK_INT(pi_tool_runs[PT_BASH] - b0, 1);
    gate_down();
    /* on: one question per call that is not allowed -- write and bash; the
     * read inside the working directory runs (C:Claude's rule) */
    gate_up(1, 0);
    calls_sse(three);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "do three things"), LS_STOP);
    CHECK_INT(asked, 2);
    CHECK_INT(gate.questions, 2);
    gate_down();
    /* a deny rule blocks without a question; an allow rule passes */
    gate_up(1, "{\"permissions\":{\"deny\":[\"Bash(echo *)\"],\"allow\":[\"Write(g.txt)\"]}}");
    calls_sse(three);
    ps_sse("../pi/openai_final.sse", 0);
    b0 = pi_tool_runs[PT_BASH];
    CHECK_INT(pi_prompt(&ses, "do three things"), LS_STOP);
    CHECK_INT(asked, 0);
    CHECK_INT(pi_tool_runs[PT_BASH] - b0, 0);
    CHECK(ps.nreq == 2 && has(ps.body[1], "Permission to use bash has been denied by the rule Bash(echo *)."));
    gate_down();
    /* the user says no: blocked, nothing written */
    put("g.txt", "old", -1);
    gate_up(1, 0);
    answer = ASK_NO;
    calls_sse(three);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "do three things"), LS_STOP);
    CHECK_INT(asked, 2);
    f = get_rel("g.txt");
    CHECK_STR(f ? f : "", "old");
    free(f);
    CHECK(ps.nreq == 2 && has(ps.body[1], "the user declined this tool call"));
    gate_down();
    /* "yes, this session": the second write asks no more */
    gate_up(1, 0);
    answer = ASK_SESSION;
    {
        static const char *const two[] = { "write", "{\"path\":\"g.txt\",\"content\":\"1\"}", "write",
                                           "{\"path\":\"g.txt\",\"content\":\"2\"}", 0, 0 };
        calls_sse(two);
    }
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "write twice"), LS_STOP);
    CHECK_INT(asked, 1);
    gate_down();
    /* print mode: nobody to ask, so a call that would ask is refused */
    gate_up(1, 0);
    ses.ask = 0;
    calls_sse(three);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "do three things"), LS_STOP);
    CHECK(ps.nreq == 2 && has(ps.body[1], "Permission to use write was denied: nobody can approve it in this run."));
    gate_down();
    /* "yes, always in this project": C:Claude's rule kept in the project's file under the
     * data directory; the same command asks no more, in this run or the next */
    {
        static const char *const two[] = { "bash", "{\"command\":\"echo one\"}", "bash", "{\"command\":\"echo two\"}",
                                           0, 0 };
        static const char *const other[] = { "bash", "{\"command\":\"ls\"}", 0, 0 };
        char data[600], pf[700];
        full("data", data);
        gate_up(1, 0);
        CHECK_INT(pi_gate_project(&gate, data, "/Work/Projects/x"), 0);
        answer = ASK_PROJECT;
        calls_sse(two);
        ps_sse("../pi/openai_final.sse", 0);
        CHECK_INT(pi_prompt(&ses, "echo twice"), LS_STOP);
        CHECK_INT(asked, 1);
        CHECK_INT(gate.kept, 1);
        gate_down();
        CHECK_INT(pi_gate_project_file(data, "/Work/Projects/x", pf, sizeof(pf)), 0);
        CHECK(has(pf, "/data/permissions/--Work-Projects-x--.json"));
        f = get(pf, 0);
        CHECK(has(f, "\"allow\"") && has(f, "\"Bash(echo *)\""));
        free(f);
        /* the next run: read back, no question for echo; ls still asks */
        gate_up(1, 0);
        CHECK_INT(pi_gate_project(&gate, data, "/Work/Projects/x"), 0);
        calls_sse(two);
        ps_sse("../pi/openai_final.sse", 0);
        CHECK_INT(pi_prompt(&ses, "echo twice"), LS_STOP);
        CHECK_INT(asked, 0);
        calls_sse(other);
        ps_sse("../pi/openai_final.sse", 0);
        CHECK_INT(pi_prompt(&ses, "ls"), LS_STOP);
        CHECK_INT(asked, 1);
        gate_down();
        /* another project has its own file: it asks */
        gate_up(1, 0);
        CHECK_INT(pi_gate_project(&gate, data, "/Work/Other"), 0);
        calls_sse(two);
        ps_sse("../pi/openai_final.sse", 0);
        CHECK_INT(pi_prompt(&ses, "echo twice"), LS_STOP);
        CHECK_INT(asked, 2);
        gate_down();
        /* pi's directory naming on AmigaDOS names */
        CHECK_INT(pi_gate_project_file("UP-Term:var/amiga-pi", "Work:Projects/x", pf, sizeof(pf)), 0);
        CHECK_STR(pf, "UP-Term:var/amiga-pi/permissions/--Work-Projects-x--.json");
        CHECK_INT(pi_gate_project_file("ENVARC:amiga-pi", "RAM:", pf, sizeof(pf)), 0);
        CHECK_STR(pf, "ENVARC:amiga-pi/permissions/--RAM---.json");
        /* E8: a long working directory still gives a name an FFS volume takes (30 characters), and two long ones differ */
        {
            char pf2[300];
            const char *name;
            CHECK_INT(pi_gate_project_file("UP-Term:var/amiga-pi", "Work:Projects/amiga-pi/some/deep/tree/of/dirs/x", pf, sizeof(pf)), 0);
            CHECK_INT(pi_gate_project_file("UP-Term:var/amiga-pi", "Work:Projects/amiga-pi/some/deep/tree/of/dirs/y", pf2, sizeof(pf2)), 0);
            name = strrchr(pf, '/') + 1;
            CHECK_INT((int)strlen(name), 30);
            CHECK(strcmp(pf, pf2) != 0);
        }
    }
    /* the setting */
    pi_gate_init(&gate, &pt, &ses);
    CHECK_INT(pi_gate_settings(&gate, "{\"permissionGate\":true}", 23, "s", &g), 0);
    CHECK_INT(g, 1);
    CHECK_INT(pi_gate_settings(&gate, "{\"model\":\"x\"}", 13, "s", &g), 0);
    CHECK_INT(g, -1);
    CHECK_INT(pi_gate_settings(&gate, "[1]", 3, "s", &g), -1);
    pi_gate_free(&gate);
}

/* ---- D7, D9: the reachability test through the program's entry ---- */

static jw out;

static void o_write(void *u, const char *s, long n)
{
    (void)u;
    jw_raw(&out, s, n);
}

static long o_read(void *u, char *b, long cap)
{
    (void)u;
    (void)b;
    (void)cap;
    return -1;
}

static int o_brk(void *u)
{
    (void)u;
    return 0;
}

static long no_env(const char *name, char *o, long cap)
{
    (void)name;
    (void)o;
    (void)cap;
    return -1;
}

static void app_platform(pi_platform *pl, cl_io *io)
{
    memset(io, 0, sizeof(*io));
    io->write = o_write;
    io->read_line = o_read;
    io->brk = o_brk;
    memset(pl, 0, sizeof(*pl));
    pl->io = io;
    pl->pool = &ps_pool;
    pl->h.poll = ps_poll;
    pl->h.sleep = ps_sleep;
    pl->h.ms = ps_ms;
    pl->h.resolve = ps_resolve;
    pl->now = fixed_now;
    pl->getenv = no_env;
    pl->cwd = dir;
    pl->sys = &sys;
    pl->tmp = tmpd;
    pl->agent_dir = dir;
    pl->shell = PI_SH_HOST;
    jw_free(&out);
    jw_init(&out);
    ps_reset();
    ps_pool_up(LLM_POOL);
}

static void test_reach(void)
{
    cl_io io;
    pi_platform pl;
    char *f;
    long e0 = pi_tool_runs[PT_EDIT], b0 = pi_tool_runs[PT_BASH];
    char *argv[] = { "amiga-pi", "--model", "fixture/qwen2.5-coder:7b", "--base-url", "http://192.168.1.10:11434/v1",
                     "-p", "please edit it", 0 };
    char *argv_tools[] = { "amiga-pi", "--model", "fixture/q", "--base-url", "http://192.168.1.10:11434/v1",
                           "--tools", "read,grep,find,ls", "-p", "hello", 0 };
    char *argv_gate[] = { "amiga-pi", "--model", "fixture/q", "--base-url", "http://192.168.1.10:11434/v1",
                          "--gate", "-p", "please edit it", 0 };
    static const char *const edit_bash[] = {
        "edit", "{\"path\":\"pi-test.txt\",\"edits\":[{\"oldText\":\"hello\",\"newText\":\"hello from amiga-pi\"}]}",
        "bash", "{\"command\":\"echo listed; ls pi-test.txt\"}", 0, 0 };
    /* edit then bash: the file changed on disk, each tool ran once */
    put("pi-test.txt", "hello\n", -1);
    app_platform(&pl, &io);
    calls_sse(edit_bash);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_app(7, argv, &pl), 0);
    f = get_rel("pi-test.txt");
    CHECK_STR(f ? f : "", "hello from amiga-pi\n");
    free(f);
    CHECK_INT(pi_tool_runs[PT_EDIT] - e0, 1);
    CHECK_INT(pi_tool_runs[PT_BASH] - b0, 1);
    CHECK_INT(ps.nreq, 2);
    CHECK(ps.nreq == 2 && has(ps.body[1], "Successfully replaced 1 block(s) in pi-test.txt."));
    if (ps.nreq == 2 && !has(ps.body[1], "listed\\npi-test.txt"))
        printf("  body: %s\n", ps.body[1]);
    CHECK(ps.nreq == 2 && has(ps.body[1], "listed\\npi-test.txt"));
    /* the request: pi's four default tools, the shell named in the prompt */
    CHECK(has(ps.body[0], "\"name\":\"read\"") && has(ps.body[0], "\"name\":\"bash\"") &&
          has(ps.body[0], "\"name\":\"edit\"") && has(ps.body[0], "\"name\":\"write\"") &&
          !has(ps.body[0], "\"name\":\"grep\""));
    CHECK(has(ps.body[0], "The bash tool runs your commands with /bin/sh."));
    CHECK(has(ps.body[0], "- Use read to examine files instead of cat or sed."));
    /* --tools */
    app_platform(&pl, &io);
    ps_sse("../pi/openai_text.sse", 0);
    CHECK_INT(pi_app(9, argv_tools, &pl), 0);
    CHECK(ps.nreq == 1 && has(ps.body[0], "\"name\":\"grep\"") && has(ps.body[0], "\"name\":\"ls\"") &&
          !has(ps.body[0], "\"name\":\"bash\""));
    /* --gate in print mode: nobody can approve the edit, the file stays */
    put("pi-test.txt", "hello\n", -1);
    app_platform(&pl, &io);
    calls_sse(edit_bash);
    ps_sse("../pi/openai_final.sse", 0);
    e0 = pi_tool_runs[PT_EDIT];
    CHECK_INT(pi_app(8, argv_gate, &pl), 0);
    CHECK_INT(pi_tool_runs[PT_EDIT] - e0, 0);
    f = get_rel("pi-test.txt");
    CHECK_STR(f ? f : "", "hello\n");
    free(f);
    /* settings.json's permissionGate does the same without the flag */
    put("settings.json", "{\"permissionGate\":true}", -1);
    app_platform(&pl, &io);
    calls_sse(edit_bash);
    ps_sse("../pi/openai_final.sse", 0);
    e0 = pi_tool_runs[PT_EDIT];
    CHECK_INT(pi_app(7, argv, &pl), 0);
    CHECK_INT(pi_tool_runs[PT_EDIT] - e0, 0);
    CHECK(ps.nreq == 2 && has(ps.body[1], "nobody can approve it"));
    /* the project's kept rules (pi_app reads them from the data directory): with Edit and
     * Bash(echo *) kept for this directory the gated print run edits */
    {
        char data[600], pf[700];
        full("pdata", data);
        CHECK_INT(pi_gate_project_file(data, dir, pf, sizeof(pf)), 0);
        dd_mkdirs(&sys, data);
        {
            char perm[700];
            cl_copy(perm, data, sizeof(perm));
            cl_cat(perm, "/permissions", sizeof(perm));
            dd_mkdirs(&sys, perm);
        }
        CHECK_INT(cfg_write_rule(&sys, pf, RULE_ALLOW, "Edit", 1), 0);
        CHECK_INT(cfg_write_rule(&sys, pf, RULE_ALLOW, "Bash(echo *)", 1), 0);
        put("pi-test.txt", "hello\n", -1);
        app_platform(&pl, &io);
        pl.data_dir = data;
        calls_sse(edit_bash);
        ps_sse("../pi/openai_final.sse", 0);
        e0 = pi_tool_runs[PT_EDIT];
        CHECK_INT(pi_app(7, argv, &pl), 0);
        CHECK_INT(pi_tool_runs[PT_EDIT] - e0, 1);
        f = get_rel("pi-test.txt");
        CHECK_STR(f ? f : "", "hello from amiga-pi\n");
        free(f);
    }
    {
        char p[600];
        full("settings.json", p);
        remove(p);
    }
}

void suite_pi_tools(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(dir, base && *base ? base : "/tmp");
    if (dir[strlen(dir) - 1] == '/')
        dir[strlen(dir) - 1] = 0;
    strcat(dir, "/pi_tools_XXXXXX");
    if (!mkdtemp(dir)) {
        CHECK(0);
        return;
    }
    tools_up();
    test_read_write();
    test_edit();
    test_edit_pi();
    test_pi_cases();
    test_details();
    test_ls_order();
    test_images();
    test_bash();
    test_user_bash_ansi();
    test_search();
    test_select();
    test_truncate();
    test_gate();
    test_reach();
    pi_tools_free(&pt);
    ps_reset();
    jw_free(&out);
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, dir);
        CHECK_INT(system(cmd), 0);
    }
}
