#define _POSIX_C_SOURCE 200809L

#include "newick.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- node construction --------------------------------------------------- */

static NwkNode *nwk_new(void)
{
    NwkNode *n = xcalloc(1, sizeof *n);
    return n;
}

void nwk_free(NwkNode *n)
{
    if (!n) return;
    for (int i = 0; i < n->n_children; i++) nwk_free(n->children[i]);
    free(n->children); free(n->label); free(n->annotation);
    free(n);
}

static void nwk_add_child(NwkNode *p, NwkNode *c)
{
    if (p->n_children == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->children = xrealloc(p->children, (size_t)p->cap * sizeof(NwkNode *));
    }
    p->children[p->n_children++] = c;
}

/* --- lexer --------------------------------------------------------------- */

typedef struct { const char *s; size_t pos; DiagList *errs; int ok; } NwkLex;

static void nwk_skip_ws(NwkLex *L)
{
    while (L->s[L->pos] && isspace((unsigned char)L->s[L->pos])) L->pos++;
}

/* Read a [...] annotation into `out` (caller frees). Strips the brackets.
 * Returns 1 if an annotation was consumed. */
static int nwk_read_annotation(NwkLex *L, char **out)
{
    nwk_skip_ws(L);
    if (L->s[L->pos] != '[') return 0;
    L->pos++;
    size_t start = L->pos, depth = 1;
    while (L->s[L->pos] && depth) {
        if (L->s[L->pos] == '[') depth++;
        else if (L->s[L->pos] == ']') { depth--; if (!depth) break; }
        L->pos++;
    }
    size_t end = L->pos;
    if (L->s[L->pos] == ']') L->pos++;
    if (*out) {                              /* concatenate if a prior one exists */
        char *cur = *out;
        *out = xasprintf("%s,%.*s", cur, (int)(end - start), L->s + start);
        free(cur);
    } else {
        *out = xstrndup(L->s + start, end - start);
    }
    return 1;
}

/* Read a label: identifier characters until a Newick-special. Returns NULL if
 * empty (anonymous internal node). */
static char *nwk_read_label(NwkLex *L)
{
    nwk_skip_ws(L);
    size_t start = L->pos;
    while (L->s[L->pos]) {
        char c = L->s[L->pos];
        if (c == '(' || c == ')' || c == ',' || c == ';' ||
            c == ':' || c == '[' || isspace((unsigned char)c)) break;
        L->pos++;
    }
    if (L->pos == start) return NULL;
    return xstrndup(L->s + start, L->pos - start);
}

/* Skip a :branch_length (also any inline :tau or #theta forms used by BPP). */
static void nwk_skip_branch(NwkLex *L)
{
    nwk_skip_ws(L);
    while (L->s[L->pos] == ':' || L->s[L->pos] == '#') {
        L->pos++;
        while (L->s[L->pos] &&
               (isdigit((unsigned char)L->s[L->pos]) || L->s[L->pos] == '.' ||
                L->s[L->pos] == 'e' || L->s[L->pos] == 'E' ||
                L->s[L->pos] == '+' || L->s[L->pos] == '-')) L->pos++;
        nwk_skip_ws(L);
    }
}

static NwkNode *nwk_parse_subtree(NwkLex *L);

static NwkNode *nwk_parse_subtree(NwkLex *L)
{
    nwk_skip_ws(L);
    NwkNode *n = nwk_new();
    if (L->s[L->pos] == '(') {
        L->pos++;
        for (;;) {
            NwkNode *c = nwk_parse_subtree(L);
            if (!c) { nwk_free(n); return NULL; }
            nwk_add_child(n, c);
            nwk_skip_ws(L);
            if (L->s[L->pos] == ',') { L->pos++; continue; }
            if (L->s[L->pos] == ')') { L->pos++; break; }
            diag_add(L->errs, DIAG_SYNTAX, -1,
                "Newick: expected ',' or ')' at offset %zu (saw '%c').",
                L->pos, L->s[L->pos] ? L->s[L->pos] : '?');
            L->ok = 0; nwk_free(n); return NULL;
        }
    }
    n->label = nwk_read_label(L);
    /* annotations and branch lengths may appear in either order; consume all */
    while (nwk_read_annotation(L, &n->annotation) || (nwk_skip_branch(L), 0)) {
        if (L->s[L->pos] != '[' && L->s[L->pos] != ':' && L->s[L->pos] != '#') break;
    }
    nwk_skip_branch(L);
    return n;
}

NwkNode *nwk_parse_root(const char *text, DiagList *errs)
{
    NwkLex L = { .s = text, .pos = 0, .errs = errs, .ok = 1 };
    nwk_skip_ws(&L);
    NwkNode *root = nwk_parse_subtree(&L);
    if (!root || !L.ok) { nwk_free(root); return NULL; }
    nwk_skip_ws(&L);
    if (L.s[L.pos] == ';') L.pos++;
    return root;
}

/* --- annotation lookup --------------------------------------------------- */

char *nwk_anno_get(const char *anno, const char *key)
{
    if (!anno) return NULL;
    size_t klen = strlen(key);
    const char *p = anno;
    while (*p) {
        while (*p == '&' || *p == ',' || *p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = v;
            while (*e && *e != ',' && *e != ' ' && *e != '\t') e++;
            return xstrndup(v, (size_t)(e - v));
        }
        while (*p && *p != ',') p++;
    }
    return NULL;
}
