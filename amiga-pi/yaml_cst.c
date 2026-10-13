/* yaml_cst.c -- the CST parser of the yaml package (yaml 2.9.0 parse/parser.js): lexemes in, the tokens it yields out (events).
 * The generator becomes a function that appends what it yields to a list; the control flow is the original's. */
#include <stdlib.h>
#include <string.h>
#include "yaml_int.h"

typedef struct parser {
    ya *a;
    int atNewLine, atScalar;
    long indent, offset;
    int onKeyLine;
    cn **stack;
    int nstack, capstack;
    const char *source;
    long srclen;
    int type;
    cevents *ev;
    int oom;
} parser;

static void on_newline(parser *P, long off)
{
    cevents *e = P->ev;
    if (e->nnl == e->capnl) {
        long nc = e->capnl ? e->capnl * 2 : 64;
        long *q = (long *)realloc(e->nl, sizeof(long) * (size_t)nc);
        if (!q) {
            P->oom = 1;
            return;
        }
        e->nl = q;
        e->capnl = nc;
    }
    e->nl[e->nnl++] = off;
}

/* every line break inside a source that is a node's: the line counter's starts */
static void source_newlines(parser *P)
{
    long i;
    for (i = 0; i < P->srclen; i++)
        if (P->source[i] == '\n')
            on_newline(P, P->offset + i + 1);
}

int cnl_push(ya *a, cnl *l, cn *t)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 4;
        cn **q = (cn **)ya_alloc(a, sizeof(cn *) * (size_t)nc);
        if (!q)
            return -1;
        if (l->n)
            memcpy(q, l->v, sizeof(cn *) * (size_t)l->n);
        l->v = q;
        l->cap = nc;
    }
    l->v[l->n++] = t;
    return 0;
}

static void l_append(parser *P, cnl *dst, const cnl *src)
{
    int i;
    for (i = 0; i < src->n; i++)
        if (cnl_push(P->a, dst, src->v[i]))
            P->oom = 1;
}

static void l_push(parser *P, cnl *l, cn *t)
{
    if (cnl_push(P->a, l, t))
        P->oom = 1;
}

static int includes_token(const cnl *l, int type)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->v[i]->type == type)
            return 1;
    return 0;
}

static int find_non_empty(const cnl *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        switch (l->v[i]->type) {
        case T_SPACE:
        case T_COMMENT:
        case T_NEWLINE:
            break;
        default:
            return i;
        }
    return -1;
}

static int is_flow_token(const cn *t)
{
    if (!t)
        return 0;
    switch (t->type) {
    case T_ALIAS:
    case T_SCALAR:
    case T_SQ_SCALAR:
    case T_DQ_SCALAR:
    case T_FLOW_COLL:
        return 1;
    default:
        return 0;
    }
}

static int has_indent(const cn *t)
{
    return t->type != T_DOCUMENT && t->type != T_ERROR && t->type != T_DOC_END && t->type != T_DIRECTIVE;
}

static cn *new_cn(parser *P, int type)
{
    cn *t = (cn *)ya_alloc(P->a, sizeof(cn));
    if (!t) {
        P->oom = 1;
        return 0;
    }
    t->type = type;
    return t;
}

static void ev_push(parser *P, cn *t)
{
    cevents *e = P->ev;
    if (!t)
        return;
    if (e->n == e->cap) {
        long nc = e->cap ? e->cap * 2 : 64;
        cn **q = (cn **)realloc(e->v, sizeof(cn *) * (size_t)nc);
        if (!q) {
            P->oom = 1;
            return;
        }
        e->v = q;
        e->cap = nc;
    }
    e->v[e->n++] = t;
}

static void st_push(parser *P, cn *t)
{
    if (!t)
        return;
    if (P->nstack == P->capstack) {
        int nc = P->capstack ? P->capstack * 2 : 16;
        cn **q = (cn **)realloc(P->stack, sizeof(cn *) * (size_t)nc);
        if (!q) {
            P->oom = 1;
            return;
        }
        P->stack = q;
        P->capstack = nc;
    }
    P->stack[P->nstack++] = t;
}

static cn *peek(parser *P, int n)
{
    return P->nstack - n >= 0 ? P->stack[P->nstack - n] : 0;
}

static citem *items_push(parser *P, cn *c)
{
    if (c->nitems == c->capitems) {
        int nc = c->capitems ? c->capitems * 2 : 4;
        citem *q = (citem *)ya_alloc(P->a, sizeof(citem) * (size_t)nc);
        if (!q) {
            P->oom = 1;
            return 0;
        }
        if (c->nitems)
            memcpy(q, c->items, sizeof(citem) * (size_t)c->nitems);
        c->items = q;
        c->capitems = nc;
    }
    memset(&c->items[c->nitems], 0, sizeof(citem));
    return &c->items[c->nitems++];
}

static citem *last_item(cn *c)
{
    return c->nitems ? &c->items[c->nitems - 1] : 0;
}

/* the token as the parser's `sourceToken` getter makes it */
static cn *source_token(parser *P)
{
    cn *t = new_cn(P, P->type);
    if (!t)
        return 0;
    t->offset = P->offset;
    t->indent = (int)P->indent;
    t->src = P->source;
    t->srclen = P->srclen;
    t->has_src = 1;
    return t;
}

static cn *error_token(parser *P, const char *msg)
{
    cn *t = new_cn(P, T_ERROR);
    if (!t)
        return 0;
    t->offset = P->offset;
    t->msg = msg;
    t->src = P->source;
    t->srclen = P->srclen;
    t->has_src = 1;
    return t;
}

static void pop(parser *P, cn *error);
static void step(parser *P);

/* getPrevProps(parent): the list the key's start properties are taken from (0: a list that is not there) */
static cnl *get_prev_props(cn *parent)
{
    citem *it;
    switch (parent->type) {
    case T_DOCUMENT:
        return &parent->start;
    case T_BLOCK_MAP:
        it = last_item(parent);
        return it->has_sep ? &it->sep : &it->start;
    case T_BLOCK_SEQ:
        it = last_item(parent);
        return &it->start;
    default:
        return 0;
    }
}

/* getFirstKeyStartProps(prev): takes the tail out of prev and returns it */
static cnl get_first_key_start_props(parser *P, cnl *prev)
{
    cnl out;
    int i, k;
    memset(&out, 0, sizeof(out));
    if (!prev || prev->n == 0)
        return out;
    i = prev->n;
    while (--i >= 0) {
        int t = prev->v[i]->type;
        if (t == T_DOC_START || t == T_EXPL_KEY || t == T_MAP_VAL || t == T_SEQ_IND || t == T_NEWLINE)
            break;
    }
    while (++i < prev->n && prev->v[i]->type == T_SPACE)
        ;
    if (i > prev->n)
        i = prev->n;
    for (k = i; k < prev->n; k++)
        l_push(P, &out, prev->v[k]);
    prev->n = i;
    return out;
}

static void fix_flow_seq_items(parser *P, cn *fc)
{
    int i;
    if (fc->fstart->type != T_FLOW_SEQ_START)
        return;
    for (i = 0; i < fc->nitems; i++) {
        citem *it = &fc->items[i];
        if (it->has_sep && !it->value && !includes_token(&it->start, T_EXPL_KEY) && !includes_token(&it->sep, T_MAP_VAL)) {
            if (it->key)
                it->value = it->key;
            it->key = 0;
            if (is_flow_token(it->value)) {
                if (it->value->has_end)
                    l_append(P, &it->value->end, &it->sep);
                else {
                    it->value->end = it->sep;
                    it->value->has_end = 1;
                }
            } else
                l_append(P, &it->start, &it->sep);
            memset(&it->sep, 0, sizeof(it->sep));
            it->has_sep = 0;
        }
    }
}

/* pop(error?) */
static void pop(parser *P, cn *error)
{
    cn *token, *top;
    if (error)
        token = error;
    else if (P->nstack > 0)
        token = P->stack[--P->nstack];
    else
        token = 0;
    if (!token) {
        cn *e = new_cn(P, T_ERROR);
        if (e) {
            e->offset = P->offset;
            e->msg = "Tried to pop an empty stack";
            e->src = "";
            e->has_src = 1;
            ev_push(P, e);
        }
        return;
    }
    if (P->nstack == 0) {
        ev_push(P, token);
        return;
    }
    top = peek(P, 1);
    if (token->type == T_BLOCK_SCALAR)
        token->indent = has_indent(top) ? top->indent : 0;
    else if (token->type == T_FLOW_COLL && top->type == T_DOCUMENT)
        token->indent = 0;
    if (token->type == T_FLOW_COLL)
        fix_flow_seq_items(P, token);
    switch (top->type) {
    case T_DOCUMENT:
        top->value = token;
        break;
    case T_BLOCK_SCALAR:
        l_push(P, &top->props, token);
        break;
    case T_BLOCK_MAP: {
        citem *it = last_item(top);
        if (it->value) {
            citem *n = items_push(P, top);
            if (n) {
                n->key = token;
                n->has_sep = 1;
            }
            P->onKeyLine = 1;
            return;
        } else if (it->has_sep) {
            it->value = token;
        } else {
            it->key = token;
            it->has_sep = 1;
            P->onKeyLine = !it->explicitKey;
            return;
        }
        break;
    }
    case T_BLOCK_SEQ: {
        citem *it = last_item(top);
        if (it->value) {
            citem *n = items_push(P, top);
            if (n)
                n->value = token;
        } else
            it->value = token;
        break;
    }
    case T_FLOW_COLL: {
        citem *it = last_item(top);
        if (!it || it->value) {
            citem *n = items_push(P, top);
            if (n) {
                n->key = token;
                n->has_sep = 1;
            }
        } else if (it->has_sep)
            it->value = token;
        else {
            it->key = token;
            it->has_sep = 1;
        }
        return;
    }
    default:
        pop(P, 0);
        pop(P, token);
    }
    if ((top->type == T_DOCUMENT || top->type == T_BLOCK_MAP || top->type == T_BLOCK_SEQ) &&
        (token->type == T_BLOCK_MAP || token->type == T_BLOCK_SEQ)) {
        citem *last = last_item(token);
        if (last && !last->has_sep && !last->value && last->start.n > 0 && find_non_empty(&last->start) == -1) {
            int ok = token->indent == 0, i;
            if (!ok) {
                ok = 1;
                for (i = 0; i < last->start.n; i++)
                    if (last->start.v[i]->type == T_COMMENT && !(last->start.v[i]->indent < token->indent))
                        ok = 0;
            }
            if (ok) {
                if (top->type == T_DOCUMENT) {
                    top->end = last->start;
                    top->has_end = 1;
                } else {
                    citem *n = items_push(P, top);
                    if (n)
                        n->start = last->start;
                }
                token->nitems -= 1;
            }
        }
    }
}

static cn *flow_scalar(parser *P, int type)
{
    cn *t = new_cn(P, type);
    source_newlines(P);
    if (!t)
        return 0;
    t->offset = P->offset;
    t->indent = (int)P->indent;
    t->src = P->source;
    t->srclen = P->srclen;
    t->has_src = 1;
    return t;
}

static cn *start_block_value(parser *P, cn *parent)
{
    cn *t;
    citem *it;
    switch (P->type) {
    case T_ALIAS:
    case T_SCALAR:
    case T_SQ_SCALAR:
    case T_DQ_SCALAR:
        return flow_scalar(P, P->type);
    case T_BLOCK_HDR:
        t = new_cn(P, T_BLOCK_SCALAR);
        if (!t)
            return 0;
        t->offset = P->offset;
        t->indent = (int)P->indent;
        l_push(P, &t->props, source_token(P));
        t->src = "";
        t->srclen = 0;
        t->has_src = 1;
        return t;
    case T_FLOW_MAP_START:
    case T_FLOW_SEQ_START:
        t = new_cn(P, T_FLOW_COLL);
        if (!t)
            return 0;
        t->offset = P->offset;
        t->indent = (int)P->indent;
        t->fstart = source_token(P);
        t->has_end = 1;
        return t;
    case T_SEQ_IND:
        t = new_cn(P, T_BLOCK_SEQ);
        if (!t)
            return 0;
        t->offset = P->offset;
        t->indent = (int)P->indent;
        it = items_push(P, t);
        if (it)
            l_push(P, &it->start, source_token(P));
        return t;
    case T_EXPL_KEY: {
        cnl *prev, start;
        P->onKeyLine = 1;
        prev = get_prev_props(parent);
        start = get_first_key_start_props(P, prev);
        l_push(P, &start, source_token(P));
        t = new_cn(P, T_BLOCK_MAP);
        if (!t)
            return 0;
        t->offset = P->offset;
        t->indent = (int)P->indent;
        it = items_push(P, t);
        if (it) {
            it->start = start;
            it->explicitKey = 1;
        }
        return t;
    }
    case T_MAP_VAL: {
        cnl *prev, start;
        P->onKeyLine = 1;
        prev = get_prev_props(parent);
        start = get_first_key_start_props(P, prev);
        t = new_cn(P, T_BLOCK_MAP);
        if (!t)
            return 0;
        t->offset = P->offset;
        t->indent = (int)P->indent;
        it = items_push(P, t);
        if (it) {
            it->start = start;
            it->key = 0;
            it->has_sep = 1;
            l_push(P, &it->sep, source_token(P));
        }
        return t;
    }
    default:
        break;
    }
    return 0;
}

static void line_end(parser *P, cn *token)
{
    switch (P->type) {
    case T_COMMA:
    case T_DOC_START:
    case T_DOC_END:
    case T_FLOW_SEQ_END:
    case T_FLOW_MAP_END:
    case T_MAP_VAL:
        pop(P, 0);
        step(P);
        break;
    default:
        if (P->type == T_NEWLINE)
            P->onKeyLine = 0;
        if (!token->has_end) {
            token->has_end = 1;
            memset(&token->end, 0, sizeof(token->end));
        }
        l_push(P, &token->end, source_token(P));
        if (P->type == T_NEWLINE)
            pop(P, 0);
    }
}

static int at_indented_comment(parser *P, const cnl *start, long indent)
{
    int i;
    if (P->type != T_COMMENT)
        return 0;
    if (P->indent <= indent)
        return 0;
    for (i = 0; i < start->n; i++)
        if (start->v[i]->type != T_NEWLINE && start->v[i]->type != T_SPACE)
            return 0;
    return 1;
}

static void stream_step(parser *P)
{
    switch (P->type) {
    case T_DIRECTIVE_LINE: {
        cn *t = new_cn(P, T_DIRECTIVE);
        if (t) {
            t->offset = P->offset;
            t->src = P->source;
            t->srclen = P->srclen;
            t->has_src = 1;
            ev_push(P, t);
        }
        return;
    }
    case T_BOM:
    case T_SPACE:
    case T_COMMENT:
    case T_NEWLINE:
        ev_push(P, source_token(P));
        return;
    case T_DOC_MODE:
    case T_DOC_START: {
        cn *doc = new_cn(P, T_DOCUMENT);
        if (!doc)
            return;
        doc->offset = P->offset;
        if (P->type == T_DOC_START)
            l_push(P, &doc->start, source_token(P));
        st_push(P, doc);
        return;
    }
    default:
        break;
    }
    {
        const char *nm = yaml_type_name(P->type);
        char *m = (char *)ya_alloc(P->a, strlen(nm) + 64);
        if (m) {
            strcpy(m, "Unexpected ");
            strcat(m, nm);
            strcat(m, " token in YAML stream");
            ev_push(P, error_token(P, m));
        }
    }
}

static void document_step(parser *P, cn *doc)
{
    cn *bv;
    if (doc->value) {
        line_end(P, doc);
        return;
    }
    switch (P->type) {
    case T_DOC_START:
        if (find_non_empty(&doc->start) != -1) {
            pop(P, 0);
            step(P);
        } else
            l_push(P, &doc->start, source_token(P));
        return;
    case T_ANCHOR:
    case T_TAG:
    case T_SPACE:
    case T_COMMENT:
    case T_NEWLINE:
        l_push(P, &doc->start, source_token(P));
        return;
    default:
        break;
    }
    bv = start_block_value(P, doc);
    if (bv)
        st_push(P, bv);
    else {
        const char *nm = yaml_type_name(P->type);
        char *m = (char *)ya_alloc(P->a, strlen(nm) + 64);
        if (m) {
            strcpy(m, "Unexpected ");
            strcat(m, nm);
            strcat(m, " token in YAML document");
            ev_push(P, error_token(P, m));
        }
    }
}

static void scalar_step(parser *P, cn *scalar)
{
    if (P->type == T_MAP_VAL) {
        cn *pp = peek(P, 2);
        cnl *prev = pp ? get_prev_props(pp) : 0, start, sep;
        cn *map;
        citem *it;
        start = get_first_key_start_props(P, prev);
        memset(&sep, 0, sizeof(sep));
        if (scalar->has_end) {
            sep = scalar->end;
            l_push(P, &sep, source_token(P));
            scalar->has_end = 0;
            memset(&scalar->end, 0, sizeof(scalar->end));
        } else
            l_push(P, &sep, source_token(P));
        map = new_cn(P, T_BLOCK_MAP);
        if (!map)
            return;
        map->offset = scalar->offset;
        map->indent = scalar->indent;
        it = items_push(P, map);
        if (it) {
            it->start = start;
            it->key = scalar;
            it->sep = sep;
            it->has_sep = 1;
        }
        P->onKeyLine = 1;
        P->stack[P->nstack - 1] = map;
    } else
        line_end(P, scalar);
}

static void block_scalar_step(parser *P, cn *scalar)
{
    switch (P->type) {
    case T_SPACE:
    case T_COMMENT:
    case T_NEWLINE:
        l_push(P, &scalar->props, source_token(P));
        return;
    case T_SCALAR:
        scalar->src = P->source;
        scalar->srclen = P->srclen;
        P->atNewLine = 1;
        P->indent = 0;
        source_newlines(P);
        pop(P, 0);
        break;
    default:
        pop(P, 0);
        step(P);
    }
}

static void block_map_step(parser *P, cn *map)
{
    citem *it = last_item(map);
    switch (P->type) {
    case T_NEWLINE:
        P->onKeyLine = 0;
        if (it->value) {
            cn *lastt = it->value->has_end && it->value->end.n ? it->value->end.v[it->value->end.n - 1] : 0;
            if (lastt && lastt->type == T_COMMENT)
                l_push(P, &it->value->end, source_token(P));
            else {
                citem *n = items_push(P, map);
                if (n)
                    l_push(P, &n->start, source_token(P));
            }
        } else if (it->has_sep)
            l_push(P, &it->sep, source_token(P));
        else
            l_push(P, &it->start, source_token(P));
        return;
    case T_SPACE:
    case T_COMMENT:
        if (it->value) {
            citem *n = items_push(P, map);
            if (n)
                l_push(P, &n->start, source_token(P));
        } else if (it->has_sep)
            l_push(P, &it->sep, source_token(P));
        else {
            if (at_indented_comment(P, &it->start, map->indent)) {
                citem *prev = map->nitems >= 2 ? &map->items[map->nitems - 2] : 0;
                if (prev && prev->value && prev->value->has_end) {
                    l_append(P, &prev->value->end, &it->start);
                    l_push(P, &prev->value->end, source_token(P));
                    map->nitems -= 1;
                    return;
                }
            }
            l_push(P, &it->start, source_token(P));
        }
        return;
    default:
        break;
    }
    if (P->indent >= map->indent) {
        int atMapIndent = !P->onKeyLine && P->indent == map->indent;
        int atNextItem = atMapIndent && (it->has_sep || it->explicitKey) && P->type != T_SEQ_IND;
        cnl start;
        memset(&start, 0, sizeof(start));
        if (atNextItem && it->has_sep && !it->value) {
            int nl[64], nnl = 0, i;
            for (i = 0; i < it->sep.n; i++) {
                cn *st = it->sep.v[i];
                switch (st->type) {
                case T_NEWLINE:
                    if (nnl < 64)
                        nl[nnl++] = i;
                    break;
                case T_SPACE:
                    break;
                case T_COMMENT:
                    if (st->indent > map->indent)
                        nnl = 0;
                    break;
                default:
                    nnl = 0;
                }
            }
            if (nnl >= 2) {
                int k;
                for (k = nl[1]; k < it->sep.n; k++)
                    l_push(P, &start, it->sep.v[k]);
                it->sep.n = nl[1];
            }
        }
        switch (P->type) {
        case T_ANCHOR:
        case T_TAG:
            if (atNextItem || it->value) {
                citem *n;
                l_push(P, &start, source_token(P));
                n = items_push(P, map);
                if (n)
                    n->start = start;
                P->onKeyLine = 1;
            } else if (it->has_sep)
                l_push(P, &it->sep, source_token(P));
            else
                l_push(P, &it->start, source_token(P));
            return;
        case T_EXPL_KEY:
            if (!it->has_sep && !it->explicitKey) {
                l_push(P, &it->start, source_token(P));
                it->explicitKey = 1;
            } else if (atNextItem || it->value) {
                citem *n;
                l_push(P, &start, source_token(P));
                n = items_push(P, map);
                if (n) {
                    n->start = start;
                    n->explicitKey = 1;
                }
            } else {
                cn *bm = new_cn(P, T_BLOCK_MAP);
                if (bm) {
                    citem *n;
                    bm->offset = P->offset;
                    bm->indent = (int)P->indent;
                    n = items_push(P, bm);
                    if (n) {
                        l_push(P, &n->start, source_token(P));
                        n->explicitKey = 1;
                    }
                    st_push(P, bm);
                }
            }
            P->onKeyLine = 1;
            return;
        case T_MAP_VAL:
            if (it->explicitKey) {
                if (!it->has_sep) {
                    if (includes_token(&it->start, T_NEWLINE)) {
                        it->key = 0;
                        it->has_sep = 1;
                        l_push(P, &it->sep, source_token(P));
                    } else {
                        cnl st2 = get_first_key_start_props(P, &it->start);
                        cn *bm = new_cn(P, T_BLOCK_MAP);
                        if (bm) {
                            citem *n;
                            bm->offset = P->offset;
                            bm->indent = (int)P->indent;
                            n = items_push(P, bm);
                            if (n) {
                                n->start = st2;
                                n->has_sep = 1;
                                l_push(P, &n->sep, source_token(P));
                            }
                            st_push(P, bm);
                        }
                    }
                } else if (it->value) {
                    citem *n = items_push(P, map);
                    if (n) {
                        n->has_sep = 1;
                        l_push(P, &n->sep, source_token(P));
                    }
                } else if (includes_token(&it->sep, T_MAP_VAL)) {
                    cn *bm = new_cn(P, T_BLOCK_MAP);
                    if (bm) {
                        citem *n;
                        bm->offset = P->offset;
                        bm->indent = (int)P->indent;
                        n = items_push(P, bm);
                        if (n) {
                            n->start = start;
                            n->has_sep = 1;
                            l_push(P, &n->sep, source_token(P));
                        }
                        st_push(P, bm);
                    }
                } else if (is_flow_token(it->key) && !includes_token(&it->sep, T_NEWLINE)) {
                    cnl st2 = get_first_key_start_props(P, &it->start);
                    cn *key = it->key, *bm;
                    cnl sep = it->sep;
                    l_push(P, &sep, source_token(P));
                    it->key = 0;
                    it->has_sep = 0;
                    memset(&it->sep, 0, sizeof(it->sep));
                    bm = new_cn(P, T_BLOCK_MAP);
                    if (bm) {
                        citem *n;
                        bm->offset = P->offset;
                        bm->indent = (int)P->indent;
                        n = items_push(P, bm);
                        if (n) {
                            n->start = st2;
                            n->key = key;
                            n->sep = sep;
                            n->has_sep = 1;
                        }
                        st_push(P, bm);
                    }
                } else if (start.n > 0) {
                    l_append(P, &it->sep, &start);
                    l_push(P, &it->sep, source_token(P));
                } else
                    l_push(P, &it->sep, source_token(P));
            } else {
                if (!it->has_sep) {
                    it->key = 0;
                    it->has_sep = 1;
                    l_push(P, &it->sep, source_token(P));
                } else if (it->value || atNextItem) {
                    citem *n = items_push(P, map);
                    if (n) {
                        n->start = start;
                        n->has_sep = 1;
                        l_push(P, &n->sep, source_token(P));
                    }
                } else if (includes_token(&it->sep, T_MAP_VAL)) {
                    cn *bm = new_cn(P, T_BLOCK_MAP);
                    if (bm) {
                        citem *n;
                        bm->offset = P->offset;
                        bm->indent = (int)P->indent;
                        n = items_push(P, bm);
                        if (n) {
                            n->has_sep = 1;
                            l_push(P, &n->sep, source_token(P));
                        }
                        st_push(P, bm);
                    }
                } else
                    l_push(P, &it->sep, source_token(P));
            }
            P->onKeyLine = 1;
            return;
        case T_ALIAS:
        case T_SCALAR:
        case T_SQ_SCALAR:
        case T_DQ_SCALAR: {
            cn *fs = flow_scalar(P, P->type);
            if (atNextItem || it->value) {
                citem *n = items_push(P, map);
                if (n) {
                    n->start = start;
                    n->key = fs;
                    n->has_sep = 1;
                }
                P->onKeyLine = 1;
            } else if (it->has_sep)
                st_push(P, fs);
            else {
                it->key = fs;
                it->has_sep = 1;
                P->onKeyLine = 1;
            }
            return;
        }
        default: {
            cn *bv = start_block_value(P, map);
            if (bv) {
                if (bv->type == T_BLOCK_SEQ) {
                    if (!it->explicitKey && it->has_sep && !includes_token(&it->sep, T_NEWLINE)) {
                        pop(P, error_token(P, "Unexpected block-seq-ind on same line with key"));
                        return;
                    }
                } else if (atMapIndent) {
                    citem *n = items_push(P, map);
                    if (n)
                        n->start = start;
                }
                st_push(P, bv);
                return;
            }
        }
        }
    }
    pop(P, 0);
    step(P);
}

static void block_sequence_step(parser *P, cn *seq)
{
    citem *it = last_item(seq);
    switch (P->type) {
    case T_NEWLINE:
        if (it->value) {
            cn *lastt = it->value->has_end && it->value->end.n ? it->value->end.v[it->value->end.n - 1] : 0;
            if (lastt && lastt->type == T_COMMENT)
                l_push(P, &it->value->end, source_token(P));
            else {
                citem *n = items_push(P, seq);
                if (n)
                    l_push(P, &n->start, source_token(P));
            }
        } else
            l_push(P, &it->start, source_token(P));
        return;
    case T_SPACE:
    case T_COMMENT:
        if (it->value) {
            citem *n = items_push(P, seq);
            if (n)
                l_push(P, &n->start, source_token(P));
        } else {
            if (at_indented_comment(P, &it->start, seq->indent)) {
                citem *prev = seq->nitems >= 2 ? &seq->items[seq->nitems - 2] : 0;
                if (prev && prev->value && prev->value->has_end) {
                    l_append(P, &prev->value->end, &it->start);
                    l_push(P, &prev->value->end, source_token(P));
                    seq->nitems -= 1;
                    return;
                }
            }
            l_push(P, &it->start, source_token(P));
        }
        return;
    case T_ANCHOR:
    case T_TAG:
        if (it->value || P->indent <= seq->indent)
            break;
        l_push(P, &it->start, source_token(P));
        return;
    case T_SEQ_IND:
        if (P->indent != seq->indent)
            break;
        if (it->value || includes_token(&it->start, T_SEQ_IND)) {
            citem *n = items_push(P, seq);
            if (n)
                l_push(P, &n->start, source_token(P));
        } else
            l_push(P, &it->start, source_token(P));
        return;
    default:
        break;
    }
    if (P->indent > seq->indent) {
        cn *bv = start_block_value(P, seq);
        if (bv) {
            st_push(P, bv);
            return;
        }
    }
    pop(P, 0);
    step(P);
}

static void flow_collection_step(parser *P, cn *fc)
{
    citem *it = last_item(fc);
    if (P->type == T_FLOW_ERROR_END) {
        cn *top;
        do {
            pop(P, 0);
            top = peek(P, 1);
        } while (top && top->type == T_FLOW_COLL);
    } else if (fc->end.n == 0) {
        cn *bv;
        switch (P->type) {
        case T_COMMA:
        case T_EXPL_KEY:
            if (!it || it->has_sep) {
                citem *n = items_push(P, fc);
                if (n)
                    l_push(P, &n->start, source_token(P));
            } else
                l_push(P, &it->start, source_token(P));
            return;
        case T_MAP_VAL:
            if (!it || it->value) {
                citem *n = items_push(P, fc);
                if (n) {
                    n->has_sep = 1;
                    l_push(P, &n->sep, source_token(P));
                }
            } else if (it->has_sep)
                l_push(P, &it->sep, source_token(P));
            else {
                it->key = 0;
                it->has_sep = 1;
                l_push(P, &it->sep, source_token(P));
            }
            return;
        case T_SPACE:
        case T_COMMENT:
        case T_NEWLINE:
        case T_ANCHOR:
        case T_TAG:
            if (!it || it->value) {
                citem *n = items_push(P, fc);
                if (n)
                    l_push(P, &n->start, source_token(P));
            } else if (it->has_sep)
                l_push(P, &it->sep, source_token(P));
            else
                l_push(P, &it->start, source_token(P));
            return;
        case T_ALIAS:
        case T_SCALAR:
        case T_SQ_SCALAR:
        case T_DQ_SCALAR: {
            cn *fs = flow_scalar(P, P->type);
            if (!it || it->value) {
                citem *n = items_push(P, fc);
                if (n) {
                    n->key = fs;
                    n->has_sep = 1;
                }
            } else if (it->has_sep)
                st_push(P, fs);
            else {
                it->key = fs;
                it->has_sep = 1;
            }
            return;
        }
        case T_FLOW_MAP_END:
        case T_FLOW_SEQ_END:
            l_push(P, &fc->end, source_token(P));
            return;
        default:
            break;
        }
        bv = start_block_value(P, fc);
        if (bv)
            st_push(P, bv);
        else {
            pop(P, 0);
            step(P);
        }
    } else {
        cn *parent = peek(P, 2);
        if (parent->type == T_BLOCK_MAP &&
            ((P->type == T_MAP_VAL && parent->indent == fc->indent) ||
             (P->type == T_NEWLINE && !last_item(parent)->has_sep))) {
            pop(P, 0);
            step(P);
        } else if (P->type == T_MAP_VAL && parent->type != T_FLOW_COLL) {
            cnl *prev = get_prev_props(parent), start, sep;
            cn *map;
            citem *n;
            int k;
            start = get_first_key_start_props(P, prev);
            fix_flow_seq_items(P, fc);
            memset(&sep, 0, sizeof(sep));
            for (k = 1; k < fc->end.n; k++)
                l_push(P, &sep, fc->end.v[k]);
            if (fc->end.n > 1)
                fc->end.n = 1;
            l_push(P, &sep, source_token(P));
            map = new_cn(P, T_BLOCK_MAP);
            if (!map)
                return;
            map->offset = fc->offset;
            map->indent = fc->indent;
            n = items_push(P, map);
            if (n) {
                n->start = start;
                n->key = fc;
                n->sep = sep;
                n->has_sep = 1;
            }
            P->onKeyLine = 1;
            P->stack[P->nstack - 1] = map;
        } else
            line_end(P, fc);
    }
}

static void document_end_step(parser *P, cn *de)
{
    if (P->type != T_DOC_MODE) {
        if (de->has_end)
            l_push(P, &de->end, source_token(P));
        else {
            de->has_end = 1;
            memset(&de->end, 0, sizeof(de->end));
            l_push(P, &de->end, source_token(P));
        }
        if (P->type == T_NEWLINE)
            pop(P, 0);
    }
}

static void step(parser *P)
{
    cn *top = peek(P, 1);
    if (P->oom)
        return;
    if (P->type == T_DOC_END && (!top || top->type != T_DOC_END)) {
        cn *de;
        while (P->nstack > 0)
            pop(P, 0);
        de = new_cn(P, T_DOC_END);
        if (de) {
            de->offset = P->offset;
            de->src = P->source;
            de->srclen = P->srclen;
            de->has_src = 1;
            st_push(P, de);
        }
        return;
    }
    if (!top) {
        stream_step(P);
        return;
    }
    switch (top->type) {
    case T_DOCUMENT:
        document_step(P, top);
        return;
    case T_ALIAS:
    case T_SCALAR:
    case T_SQ_SCALAR:
    case T_DQ_SCALAR:
        scalar_step(P, top);
        return;
    case T_BLOCK_SCALAR:
        block_scalar_step(P, top);
        return;
    case T_BLOCK_MAP:
        block_map_step(P, top);
        return;
    case T_BLOCK_SEQ:
        block_sequence_step(P, top);
        return;
    case T_FLOW_COLL:
        flow_collection_step(P, top);
        return;
    case T_DOC_END:
        document_end_step(P, top);
        return;
    default:
        break;
    }
    pop(P, 0);
}

static void next_lexeme(parser *P, const char *s, long n)
{
    int type;
    P->source = s;
    P->srclen = n;
    if (P->atScalar) {
        P->atScalar = 0;
        step(P);
        P->offset += n;
        return;
    }
    type = yaml_token_type(s, n);
    if (!type) {
        char *m = (char *)ya_alloc(P->a, (size_t)n + 32);
        if (m) {
            strcpy(m, "Not a YAML token: ");
            memcpy(m + strlen(m), s, (size_t)n);
            m[strlen("Not a YAML token: ") + (size_t)n] = 0;
            pop(P, error_token(P, m));
        }
        P->offset += n;
    } else if (type == T_SCALAR) {
        P->atNewLine = 0;
        P->atScalar = 1;
        P->type = T_SCALAR;
    } else {
        P->type = type;
        step(P);
        switch (type) {
        case T_NEWLINE:
            P->atNewLine = 1;
            P->indent = 0;
            on_newline(P, P->offset + n);
            break;
        case T_SPACE:
            if (P->atNewLine && s[0] == ' ')
                P->indent += n;
            break;
        case T_EXPL_KEY:
        case T_MAP_VAL:
        case T_SEQ_IND:
            if (P->atNewLine)
                P->indent += n;
            break;
        case T_DOC_MODE:
        case T_FLOW_ERROR_END:
            return;
        default:
            P->atNewLine = 0;
        }
        P->offset += n;
    }
}

int yaml_cst(ya *a, const lexemes *lx, cevents *out)
{
    parser P;
    long i;
    memset(&P, 0, sizeof(P));
    P.a = a;
    P.atNewLine = 1;
    P.ev = out;
    out->v = 0;
    out->n = out->cap = 0;
    out->nl = 0;
    out->nnl = out->capnl = 0;
    on_newline(&P, 0);
    for (i = 0; i < lx->n && !P.oom; i++)
        next_lexeme(&P, lx->v[i].p, lx->v[i].n);
    while (P.nstack > 0 && !P.oom)
        pop(&P, 0);
    free(P.stack);
    return P.oom ? -1 : 0;
}
