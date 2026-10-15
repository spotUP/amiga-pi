/* see prompt.h */
#include <string.h>
#include "prompt.h"
#include "resources.h"
#include "claude/amiga_text.h"
#include "claude/util.h"

static const char preamble[] =
    "You are an expert coding assistant operating inside amiga-pi, a coding agent harness. You help users by "
    "reading files, executing commands, editing code, and writing new files.";

/* the tools pi's prompt names: amiga-pi's own, all declared (no hidden ones yet) */
static int has(const pi_session *s, const char *name)
{
    int i;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, name))
            return 1;
    return 0;
}

/* a section's text: pi wraps every one but the preamble in a tag of its name */
static void open_tag(jw *w, const char *name)
{
    jw_rawz(w, "<");
    jw_rawz(w, name);
    jw_rawz(w, ">\n");
}

static void close_tag(jw *w, const char *name)
{
    jw_rawz(w, "\n</");
    jw_rawz(w, name);
    jw_rawz(w, ">");
}

/* the section just written (from at to the end of text) recorded under name */
static void section(pi_psec *o, const char *name, long at)
{
    if (o->n < PI_PSECS) {
        cl_copy(o->name[o->n], name, sizeof(o->name[0]));
        o->off[o->n] = at;
        o->len[o->n] = o->text.n - at;
        o->n++;
    }
}

/* the default prompt's tools, rules and amiga sections */
static void prefix(const pi_session *s, pi_psec *out)
{
    int i, k = 0;
    long at;
    /* tools: "- name: snippet" for the tools that have one */
    at = out->text.n;
    open_tag(&out->text, "tools");
    for (i = 0; i < s->ntools; i++)
        if (s->tools[i]->snippet) {
            jw_rawz(&out->text, k++ ? "\n- " : "- ");
            jw_rawz(&out->text, s->tools[i]->name);
            jw_rawz(&out->text, ": ");
            jw_rawz(&out->text, s->tools[i]->snippet);
        }
    if (!k)
        jw_rawz(&out->text, "(none)");
    jw_rawz(&out->text, "\n\nIn addition to the tools above, you may have access to other custom tools depending on "
                        "the project.");
    close_tag(&out->text, "tools");
    section(out, "tools", at);
    /* rules: buildRules */
    at = out->text.n;
    open_tag(&out->text, "rules");
    if (has(s, "bash") && !has(s, "grep") && !has(s, "find") && !has(s, "ls"))
        jw_rawz(&out->text, "- Use bash for file operations like ls, rg, find\n");
    /* each tool's guidelines, in tool order, each once (pi's addRule) */
    for (i = 0; i < s->ntools; i++) {
        const char *const *g = s->tools[i]->guide;
        for (; g && *g; g++) {
            int j, seen = 0;
            for (j = 0; j < i && !seen; j++) {
                const char *const *h = s->tools[j]->guide;
                for (; h && *h && !seen; h++)
                    seen = !strcmp(*h, *g);
            }
            if (seen)
                continue;
            jw_rawz(&out->text, "- ");
            jw_rawz(&out->text, *g);
            jw_rawz(&out->text, "\n");
        }
    }
    jw_rawz(&out->text, "- Be concise in your responses\n- Show file paths clearly when working with files");
    close_tag(&out->text, "rules");
    section(out, "rules", at);
    /* amiga: the machine, as C:Claude tells it (claude/amiga_text.h) */
    at = out->text.n;
    open_tag(&out->text, "amiga");
    /* split: C90 only guarantees string literals of 509 bytes, and CL_AMIGA_PATHS alone is longer than that */
    jw_rawz(&out->text, "You run as a native program on " CL_AMIGA_MACHINE ". ");
    jw_rawz(&out->text, CL_AMIGA_PATHS);
    jw_rawz(&out->text, "Relative paths start from the working directory (cwd). The machine is slow and has little "
                        "memory: prefer targeted reads and searches to reading whole trees.");
    if (s->shell_note) {
        jw_rawz(&out->text, " ");
        jw_rawz(&out->text, s->shell_note);
    }
    close_tag(&out->text, "amiga");
    section(out, "amiga", at);
}

int pi_prompt_sections(const pi_session *s, const char *cwd, pi_psec *out)
{
    int i;
    long at;
    const pi_res *r = s->res;
    const char *c;
    int custom;
    memset(out, 0, sizeof(*out));
    jw_init(&out->text);
    at = out->text.n;
    /* pi's customPrompt: SYSTEM.md is the preamble; no tools, rules or docs (amiga) */
    custom = r && r->system && r->system[0];
    jw_rawz(&out->text, custom ? r->system : preamble);
    section(out, "preamble", at);
    if (!custom)
        prefix(s, out);
    if (r && r->append && r->append[0]) {
        at = out->text.n;
        open_tag(&out->text, "addendum");
        jw_rawz(&out->text, r->append);
        close_tag(&out->text, "addendum");
        section(out, "addendum", at);
    }
    if (r && r->nf) {
        /* pi's renderProjectContext */
        at = out->text.n;
        open_tag(&out->text, "project_context");
        jw_rawz(&out->text, "Project-specific instructions and guidelines:");
        for (i = 0; i < r->nf; i++) {
            jw_rawz(&out->text, "\n\n<project_instructions path=\"");
            jw_rawz(&out->text, r->f[i].path);
            jw_rawz(&out->text, "\">\n");
            jw_raw(&out->text, r->f[i].text, r->f[i].n);
            jw_rawz(&out->text, "\n</project_instructions>");
        }
        close_tag(&out->text, "project_context");
        section(out, "project_context", at);
    }
    if (r && r->skills.n) {
        /* pi's skills section: the reader named is the first of read and bash that the session has */
        jw sk;
        jw_init(&sk);
        if ((has(s, "read") || has(s, "bash")) && pi_skills_format(&r->skills, has(s, "read") ? "read" : "bash", &sk) == 0 && sk.n) {
            at = out->text.n;
            open_tag(&out->text, "skills");
            jw_raw(&out->text, sk.p, sk.n);
            close_tag(&out->text, "skills");
            section(out, "skills", at);
        }
        jw_free(&sk);
    }
    at = out->text.n;
    open_tag(&out->text, "cwd");
    for (c = cwd ? cwd : ""; *c; c++)
        jw_raw(&out->text, *c == '\\' ? "/" : c, 1);     /* pi: cwd.replace(/\\/g, "/") */
    close_tag(&out->text, "cwd");
    section(out, "cwd", at);
    if (s->mcp_section) {
        /* pi's MCP extension adds it in before_agent_start: after the sections before it */
        at = out->text.n;
        open_tag(&out->text, "mcp_servers");
        jw_rawz(&out->text, s->mcp_section);
        close_tag(&out->text, "mcp_servers");
        section(out, "mcp_servers", at);
    }
    return out->text.oom ? -1 : 0;
}

void pi_psec_free(pi_psec *p)
{
    jw_free(&p->text);
    p->n = 0;
}

/* getSystemMessageText of a message that is all sections: the texts joined by a blank line */
int pi_system_prompt(const pi_session *s, const char *cwd, jw *out)
{
    pi_psec p;
    int i;
    if (pi_prompt_sections(s, cwd, &p)) {
        pi_psec_free(&p);
        return -1;
    }
    for (i = 0; i < p.n; i++) {
        if (i)
            jw_rawz(out, "\n\n");
        jw_raw(out, p.text.p + p.off[i], p.len[i]);
    }
    pi_psec_free(&p);
    return out->oom ? -1 : 0;
}
