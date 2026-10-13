/* yaml_int.h -- internals of yaml.c, yaml_lex.c and yaml_cst.c: a port of the `yaml` package pi 1.1.0 parses frontmatter with
 * (yaml 2.9.0: parse/lexer.js, parse/parser.js, compose, all files), so that the errors it reports, their order and their text are
 * its own. Not for other modules: yaml.h is the interface. */
#ifndef PI_YAML_INT_H
#define PI_YAML_INT_H

#include <stddef.h>

/* ---- memory: one arena per parse, freed at once ---- */
typedef struct ya_blk {
    struct ya_blk *next;
} ya_blk;

typedef struct ya {
    ya_blk *head;
    int oom;
} ya;

void *ya_alloc(ya *a, size_t n);        /* zeroed; 0 (and a->oom) when out of memory */
char *ya_dup(ya *a, const char *s, long n);
void ya_free(ya *a);

/* ---- CST token and node types (yaml's cst.tokenType and the structure types of parser.js) ---- */
enum {
    T_NONE = 0,
    /* tokens of the lexer */
    T_BOM, T_DOC_MODE, T_FLOW_ERROR_END, T_SCALAR, T_DOC_START, T_DOC_END, T_NEWLINE, T_SEQ_IND, T_EXPL_KEY, T_MAP_VAL,
    T_FLOW_MAP_START, T_FLOW_MAP_END, T_FLOW_SEQ_START, T_FLOW_SEQ_END, T_COMMA, T_SPACE, T_COMMENT, T_DIRECTIVE_LINE,
    T_ALIAS, T_ANCHOR, T_TAG, T_SQ_SCALAR, T_DQ_SCALAR, T_BLOCK_HDR,
    /* structure the parser builds */
    T_DOCUMENT, T_BLOCK_MAP, T_BLOCK_SEQ, T_FLOW_COLL, T_BLOCK_SCALAR, T_ERROR, T_DIRECTIVE
};

typedef struct cn cn;

typedef struct cnl {
    cn **v;
    int n, cap;
} cnl;

typedef struct citem {
    cnl start;
    cn *key;
    cnl sep;
    int has_sep;                /* `sep` is an array (maybe empty) */
    cn *value;
    int explicitKey;
} citem;

struct cn {
    int type;
    long offset;
    int indent;
    const char *src;            /* `source` of a token or scalar */
    long srclen;
    int has_src;                /* `source` is a string (collections have none) */
    const char *msg;            /* error */
    cnl start;                  /* document */
    cn *value;                  /* document */
    cnl end;
    int has_end;                /* `end` is an array */
    cnl props;                  /* block scalar */
    cn *fstart;                 /* flow collection: its start token */
    citem *items;
    int nitems, capitems;
};

/* ---- the lexer: all lexemes of the text (the source is complete) ---- */
typedef struct lexeme {
    const char *p;
    long n;
} lexeme;

typedef struct lexemes {
    lexeme *v;
    long n, cap;
} lexemes;

/* 0, -1 out of memory */
int yaml_lex(ya *a, const char *buf, long blen, lexemes *out);

/* the type of a lexeme (cst.tokenType), T_NONE when it is none */
int yaml_token_type(const char *s, long n);
/* the name yaml gives a token type ("map-value-ind") */
const char *yaml_type_name(int type);

/* ---- the CST parser: the events it yields, in order ---- */
typedef struct cevents {
    cn **v;
    long n, cap;
    long *nl;                   /* the line counter's line starts (onNewLine), in the order the parser reports them */
    long nnl, capnl;
} cevents;

int yaml_cst(ya *a, const lexemes *lx, cevents *out);

/* list helpers shared by the parser and the composer */
int cnl_push(ya *a, cnl *l, cn *t);

#endif
