#define _POSIX_C_SOURCE 200809L

#include "import.h"
#include "newick.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void import_init(Import *im)
{
    memset(im, 0, sizeof *im);
    miglist_init(&im->mig);
    introlist_init(&im->intro);
}

void import_free(Import *im)
{
    for (int i = 0; i < im->n_joins;   i++) free(im->joins[i]);
    for (int i = 0; i < im->n_rotates; i++) free(im->rotates[i]);
    free(im->joins); free(im->rotates); free(im->newick_in);
    miglist_free(&im->mig);
    introlist_free(&im->intro);
    graph_free(im->graph);
    memset(im, 0, sizeof *im);
}

/* --- base-tree -> joins/rotates emission -------------------------------- */

/* Suppress any unary internal nodes left over from the introgression
 * unwrapping above. (Newick may also have these from the input.) */
static NwkNode *suppress_unary(NwkNode *n)
{
    for (int i = 0; i < n->n_children; i++)
        n->children[i] = suppress_unary(n->children[i]);
    if (!n->n_children) return n;
    if (n->n_children == 1) {
        NwkNode *child = n->children[0];
        free(n->children); free(n->label); free(n->annotation);
        free(n);
        return child;
    }
    return n;
}

/* "Canonical name" used by the join formula: a tip's name, or an internal
 * label, or an implicit underscore-joined leaf set. */
static int collect_leaf_names(const NwkNode *n, char **out, int *cnt, int cap);

static int collect_leaf_names(const NwkNode *n, char **out, int *cnt, int cap)
{
    if (!n->n_children) {
        if (*cnt < cap) out[(*cnt)++] = n->label ? n->label : "?";
        return 1;
    }
    for (int i = 0; i < n->n_children; i++)
        collect_leaf_names(n->children[i], out, cnt, cap);
    return 1;
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char *implicit_label(const NwkNode *n)
{
    char *tmp[256] = {0}; int nleaf = 0;
    collect_leaf_names(n, tmp, &nleaf, 256);
    qsort(tmp, (size_t)nleaf, sizeof(char *), cmp_strp);
    size_t len = 0; for (int i = 0; i < nleaf; i++) len += strlen(tmp[i]) + 1;
    char *s = xmalloc(len ? len : 1); s[0] = '\0';
    for (int i = 0; i < nleaf; i++) {
        if (i) strcat(s, "_");
        strcat(s, tmp[i]);
    }
    return s;
}

static char *node_name(const NwkNode *n)
{
    if (!n->n_children) return xstrdup(n->label ? n->label : "?");
    if (n->label && *n->label) return xstrdup(n->label);
    return implicit_label(n);
}

/* Walk post-order, emit one join per internal node:
 *   <left_name> + <right_name> [= label]
 * Records a rotate spec if the child order in the Newick is non-alphabetical
 * (since our default Newick emits children in the order joined, and our join
 * `A+B` outputs `(A,B)`, we only need a rotation when right < left). */
static void walk(NwkNode *n, Import *im)
{
    for (int i = 0; i < n->n_children; i++) walk(n->children[i], im);
    if (n->n_children < 2) return;            /* polytomies degrade to nested pairs below */
    char *left = node_name(n->children[0]);
    for (int i = 1; i < n->n_children; i++) {
        char *right = node_name(n->children[i]);
        /* Emit an explicit label only when the user gave one in the Newick AND
         * it differs from the implicit '_'-joined leaf-set label (which the
         * resolver auto-generates and which we forbid as an explicit label). */
        char *implicit = (i == n->n_children - 1) ? implicit_label(n) : NULL;
        const char *lbl = NULL;
        if (i == n->n_children - 1 && n->label && *n->label &&
            strcmp(n->label, implicit ? implicit : "") != 0)
            lbl = n->label;
        char *spec = lbl ? xasprintf("%s+%s=%s", left, right, lbl)
                         : xasprintf("%s+%s", left, right);
        im->joins = xrealloc(im->joins, (size_t)(im->n_joins + 1) * sizeof(char *));
        im->joins[im->n_joins++] = spec;
        free(right);
        char *new_left = lbl ? xstrdup(lbl) : (implicit ? xstrdup(implicit) : implicit_label(n));
        free(left); left = new_left;
        free(implicit);
    }
    free(left);
}

/* --- block / control-file extraction ------------------------------------ */

/* Try to extract a Newick substring from text: prefer the species&tree block
 * if present, otherwise return text itself (trimmed). The migration block is
 * also lifted out (if any). Caller frees *newick. */
static char *extract_newick_and_blocks(const char *text, MigList *mig)
{
    /* species&tree = N  name1 ... nameN  c1 ... cN  NEWICK ; ...  */
    const char *st = strstr(text, "species&tree");
    if (st) {
        const char *p = st + 12;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') goto raw;
        int N = 0;
        while (*p >= '0' && *p <= '9') N = N * 10 + (*p++ - '0');
        for (int k = 0; k < 2 * N; k++) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        /* now at the Newick */
        const char *nstart = p;
        const char *nend = nstart;
        if (*p == '(') {
            int depth = 0;
            while (*nend) {
                if (*nend == '(') depth++;
                else if (*nend == ')') { if (!--depth) { nend++; break; } }
                nend++;
            }
        } else {
            while (*nend && *nend != ';' && !isspace((unsigned char)*nend)) nend++;
        }
        if (*nend == ';') nend++;
        char *nwk = xstrndup(nstart, (size_t)(nend - nstart));

        /* optional migration block */
        const char *mg = strstr(text, "migration");
        if (mg) {
            const char *q = mg + 9;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q >= '0' && *q <= '9') {
                int M = 0;
                while (*q >= '0' && *q <= '9') M = M * 10 + (*q++ - '0');
                while (*q && *q != '\n') q++;
                if (*q) q++;
                for (int k = 0; k < M; k++) {
                    while (*q == ' ' || *q == '\t') q++;
                    const char *a = q; while (*q && !isspace((unsigned char)*q)) q++;
                    char *src = xstrndup(a, (size_t)(q - a));
                    while (*q == ' ' || *q == '\t') q++;
                    const char *b = q; while (*q && !isspace((unsigned char)*q)) q++;
                    char *dst = xstrndup(b, (size_t)(q - b));
                    if (*src && *dst) miglist_add(mig, src, dst);
                    free(src); free(dst);
                    while (*q && *q != '\n') q++;
                    if (*q) q++;
                }
            }
        }
        return nwk;
    }
raw:;
    /* trim */
    while (*text && isspace((unsigned char)*text)) text++;
    return xstrdup(text);
}

/* --- driver ------------------------------------------------------------- */

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    size_t cap = 4096, len = 0; char *b = xmalloc(cap); size_t r;
    while ((r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; b = xrealloc(b, cap); }
    }
    b[len] = '\0'; fclose(f); return b;
}

int import_read(const char *path, Import *im, DiagList *errs)
{
    char *text = read_all(path);
    if (!text) {
        diag_add(errs, DIAG_SYNTAX, -1, "cannot read '%s'.", path);
        return 0;
    }
    char *nwk = extract_newick_and_blocks(text, &im->mig);
    free(text);
    im->newick_in = xstrdup(nwk);

    NwkNode *root = nwk_parse_root(nwk, errs);
    free(nwk);
    if (!root) { return 0; }

    /* Build the faithful network graph. EVERY MSC-I network -- models A/B/C and
     * now D (bidirectional) -- is carried as the graph: the base tree follows
     * native (own-tau) edges (BPP's species tree) and re-emission serialises the
     * graph directly (faithful + idempotent). A NULL graph means doubled labels
     * that are not a buildable network (e.g. a label that occurs other than
     * twice) -- a genuine error. A graph with no hybrids is a plain tree. */
    Graph *g = graph_from_newick(root, errs);
    if (!g) { nwk_free(root); return 0; }       /* errs holds the diagnostic */

    if (g->n_hybrids > 0) {
        im->graph = g;
        im->graph_only = 1;
        im->had_msci = 1;
        /* expose the events as a flat list too, so consumers that read im->intro
         * (the REPL, JSON) see them without touching the graph. */
        introlist_events(&im->intro, g);
        char *base = graph_base_newick(g);
        NwkNode *broot = base ? nwk_parse_root(base, errs) : NULL;
        free(base);
        nwk_free(root);
        if (!broot) {
            diag_add(errs, DIAG_SYNTAX, -1, "imported network: base tree is empty.");
            return 0;
        }
        broot = suppress_unary(broot);
        walk(broot, im);
        nwk_free(broot);
        return 1;
    }
    graph_free(g);                              /* plain tree: no reticulations */

    im->had_msci = 0;
    root = suppress_unary(root);
    if (root->n_children == 0) {
        diag_add(errs, DIAG_SYNTAX, -1, "imported tree is empty.");
        nwk_free(root); return 0;
    }
    walk(root, im);
    nwk_free(root);
    return 1;
}
