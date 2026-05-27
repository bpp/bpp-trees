#include "parser.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void joinlist_init(JoinList *j)
{
    j->items = NULL;
    j->count = 0;
    j->cap = 0;
}

void joinlist_free(JoinList *j)
{
    for (int i = 0; i < j->count; i++) {
        JoinStmt *s = &j->items[i];
        for (int k = 0; k < s->n_operands; k++) free(s->operands[k]);
        free(s->operands);
        free(s->label);
    }
    free(j->items);
    joinlist_init(j);
}

static int is_token_char(int c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static void skip_ws(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    *pp = p;
}

static char *read_token(const char **pp)
{
    const char *p = *pp;
    const char *start = p;
    while (is_token_char((unsigned char)*p)) p++;
    if (p == start) return NULL;
    char *t = xstrndup(start, (size_t)(p - start));
    *pp = p;
    return t;
}

static void append_join(JoinList *out, char **ops, int n, char *label, int line_no)
{
    if (out->count == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 8;
        out->items = xrealloc(out->items, (size_t)out->cap * sizeof(*out->items));
    }
    JoinStmt *s = &out->items[out->count++];
    s->operands = ops;
    s->n_operands = n;
    s->label = label;
    s->line_no = line_no;
}

/* Parse a single statement (one line, or one comma/semicolon segment).
 * Returns 1 if a syntax error was added, else 0. Blank/comment-only input
 * adds nothing and returns 0. */
static int parse_one(const char *s, int line_no, JoinList *out, DiagList *errs)
{
    const char *p = s;
    skip_ws(&p);
    if (*p == '\0' || *p == '#') return 0;  /* blank or comment */

    char **ops = NULL;
    int n = 0, cap = 0;
    char *label = NULL;

    for (;;) {
        skip_ws(&p);
        char *tok = read_token(&p);
        if (!tok) {
            diag_add(errs, DIAG_SYNTAX, line_no,
                     "expected a taxon or clade name, found '%c'.",
                     *p ? *p : ' ');
            goto fail;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            ops = xrealloc(ops, (size_t)cap * sizeof(*ops));
        }
        ops[n++] = tok;

        skip_ws(&p);
        if (*p == '+') { p++; continue; }
        break;
    }

    skip_ws(&p);
    if (*p == '=') {
        p++;
        skip_ws(&p);
        label = read_token(&p);
        if (!label) {
            diag_add(errs, DIAG_SYNTAX, line_no,
                     "expected a label after '='.");
            goto fail;
        }
        skip_ws(&p);
    }

    if (*p != '\0' && *p != '#') {
        diag_add(errs, DIAG_SYNTAX, line_no,
                 "unexpected character '%c'. Tokens may contain only "
                 "letters, digits, '_' and '-'; use '+' to join and "
                 "'= label' to name a clade.", *p);
        goto fail;
    }

    append_join(out, ops, n, label, line_no);
    return 0;

fail:
    for (int k = 0; k < n; k++) free(ops[k]);
    free(ops);
    free(label);
    return 1;
}

int parse_joins_text(const char *text, JoinList *out, DiagList *errs)
{
    int n_err = 0;
    int line_no = 1;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xstrndup(p, len);
        n_err += parse_one(line, line_no, out, errs);
        free(line);
        line_no++;
        if (!nl) break;
        p = nl + 1;
    }
    return n_err;
}

int parse_joins_string(const char *str, JoinList *out, DiagList *errs)
{
    int n_err = 0;
    int idx = 1;
    const char *p = str;
    while (*p) {
        size_t len = strcspn(p, ",;");
        char *seg = xstrndup(p, len);
        n_err += parse_one(seg, idx, out, errs);
        free(seg);
        idx++;
        p += len;
        if (*p) p++;  /* skip the separator */
    }
    return n_err;
}
