/* graph_roundtrip — exercise the network graph (src/graph.c) directly.
 *
 *   graph_roundtrip FILE
 *
 * Reads the first Newick / extended-Newick string in FILE (everything from the
 * first '(' through the matching ';'), builds the graph, serialises it, then
 * re-parses and re-serialises the result. Prints the serialised network and
 * exits:
 *   0  built and round-tripped; serialisation is idempotent (s1 == s2)
 *   1  parse or graph-build failed (diagnostics printed to stderr)
 *   3  built but NOT idempotent (the two serialisations differ)
 *   2  usage / I/O error
 *
 * This is the structural oracle for the graph: "parse -> graph -> string"
 * must be byte-stable across a second pass, for every network including the
 * stacked (M3) form.
 */
#define _POSIX_C_SOURCE 200809L

#include "../src/newick.h"
#include "../src/graph.h"
#include "../src/diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    size_t r;
    while ((r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
    }
    b[len] = '\0';
    fclose(f);
    return b;
}

static void print_diags(const DiagList *errs)
{
    for (int i = 0; i < errs->count; i++)
        fprintf(stderr, "  [%s] %s\n", errs->items[i].code, errs->items[i].message);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s FILE\n", argv[0]); return 2; }

    char *text = slurp(argv[1]);
    if (!text) { fprintf(stderr, "cannot read '%s'\n", argv[1]); return 2; }
    const char *p = strchr(text, '(');
    if (!p) { fprintf(stderr, "no Newick in '%s'\n", argv[1]); free(text); return 2; }

    DiagList errs; diag_init(&errs);
    int rc = 0;

    NwkNode *root = nwk_parse_root(p, &errs);
    if (!root) { fprintf(stderr, "parse failed:\n"); print_diags(&errs); rc = 1; goto done; }

    Graph *g = graph_from_newick(root, &errs);
    if (!g) { fprintf(stderr, "graph build failed:\n"); print_diags(&errs); nwk_free(root); rc = 1; goto done; }

    char *s1 = graph_to_newick(g);

    NwkNode *root2 = nwk_parse_root(s1, &errs);
    Graph *g2 = root2 ? graph_from_newick(root2, &errs) : NULL;
    char *s2 = g2 ? graph_to_newick(g2) : NULL;

    printf("%s;\n", s1);
    if (!s2 || strcmp(s1, s2) != 0) {
        fprintf(stderr, "NOT IDEMPOTENT:\n  s1=%s\n  s2=%s\n", s1, s2 ? s2 : "(rebuild failed)");
        rc = 3;
    }

    free(s2); graph_free(g2); nwk_free(root2);
    free(s1); graph_free(g); nwk_free(root);

done:
    diag_free(&errs);
    free(text);
    return rc;
}
