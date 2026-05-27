#include "block.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

char *species_block(TreeNode **taxa, int n_taxa, const char *newick,
                    const Imap *imap, int *filled, int **counts_out)
{
    int *counts = xmalloc((size_t)(n_taxa ? n_taxa : 1) * sizeof(int));
    int all = (imap != NULL);
    for (int i = 0; i < n_taxa; i++) {
        counts[i] = imap ? imap_count_for(imap, taxa[i]->name) : -1;
        if (counts[i] < 0) all = 0;
    }
    *filled = all;

    /* line 1: "species&tree = N  n1  n2 ..." */
    char *out = xasprintf("species&tree = %d", n_taxa);
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = xasprintf("%s  %s", out, taxa[i]->name);
        free(out); out = tmp;
    }
    /* line 2: counts */
    { char *tmp = xasprintf("%s\n ", out); free(out); out = tmp; }
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = (counts[i] >= 0) ? xasprintf("%s  %d", out, counts[i])
                                     : xasprintf("%s  ?", out);
        free(out); out = tmp;
    }
    /* line 3: newick */
    { char *tmp = xasprintf("%s\n  %s", out, newick); free(out); out = tmp; }

    *counts_out = counts;
    return out;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Locate the species&tree statement: [*start,*end). Returns 1 on success. */
static int find_species_tree(const char *t, size_t *start, size_t *end)
{
    size_t n = strlen(t), i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && (t[j] == ' ' || t[j] == '\t')) j++;   /* line indent */
        if (strncmp(t + j, "species&tree", 12) == 0) {
            char a = t[j + 12];
            if (a == ' ' || a == '\t' || a == '=' || a == '\0') {
                size_t p = j + 12;
                while (p < n && (t[p] == ' ' || t[p] == '\t')) p++;
                if (p < n && t[p] == '=') p++;
                while (p < n && is_ws(t[p])) p++;
                if (p >= n || t[p] < '0' || t[p] > '9') return 0;   /* expect N */
                int N = 0;
                while (p < n && t[p] >= '0' && t[p] <= '9') N = N * 10 + (t[p++] - '0');
                for (int k = 0; k < 2 * N; k++) {        /* N names + N counts */
                    while (p < n && is_ws(t[p])) p++;
                    if (p >= n) return 0;
                    while (p < n && !is_ws(t[p])) p++;
                }
                while (p < n && is_ws(t[p])) p++;        /* the Newick */
                if (p >= n) return 0;
                if (t[p] == '(') {
                    int depth = 0;
                    while (p < n) {
                        if (t[p] == '(') depth++;
                        else if (t[p] == ')') { if (--depth == 0) { p++; break; } }
                        p++;
                    }
                    if (depth != 0) return 0;
                } else {                                  /* single-tip tree */
                    while (p < n && t[p] != ';' && !is_ws(t[p])) p++;
                }
                if (p < n && t[p] == ';') p++;
                *start = j;
                *end = p;
                return 1;
            }
        }
        while (i < n && t[i] != '\n') i++;
        if (i < n) i++;
    }
    return 0;
}

char *control_replace_block(const char *text, const char *new_block,
                            const char **errmsg)
{
    size_t s, e;
    if (!find_species_tree(text, &s, &e)) {
        *errmsg = "no species&tree block found in the file";
        return NULL;
    }
    size_t blen = strlen(new_block), tlen = strlen(text);
    char *out = xmalloc(s + blen + (tlen - e) + 1);
    memcpy(out, text, s);
    memcpy(out + s, new_block, blen);
    memcpy(out + s + blen, text + e, tlen - e + 1);   /* incl. NUL */
    return out;
}
