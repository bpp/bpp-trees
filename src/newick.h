/* newick.h — a faithful (extended-)Newick parser.
 *
 * Parses a Newick or BPP extended-Newick string into a plain tree of NwkNode,
 * preserving every label and every `[&...]` annotation verbatim (brackets
 * stripped). A hybrid label simply appears twice in the resulting tree -- this
 * parser does no MSC-I interpretation; it is the shared front end for both the
 * importer (src/import.c) and the network graph builder (src/graph.c).
 */
#ifndef BPP_TREE_NEWICK_H
#define BPP_TREE_NEWICK_H

#include "diag.h"

typedef struct NwkNode {
    char            *label;       /* tip name or internal label (may be NULL) */
    char            *annotation;  /* the [&...] content, sans brackets; or NULL */
    struct NwkNode **children;
    int              n_children, cap;
} NwkNode;

/* Parse a whole Newick string (optional trailing ';'). Returns the root, or
 * NULL on syntax error (diagnostics appended to errs). Caller frees with
 * nwk_free. */
NwkNode *nwk_parse_root(const char *text, DiagList *errs);

/* Recursively free a node and its children. */
void     nwk_free(NwkNode *n);

/* Extract a `key=val` token from an annotation body like
 * "&phi=0.3,&tau-parent=yes" (sans brackets). Returns the value substring
 * (caller frees), or NULL if the key is absent. Case-insensitive on the key. */
char    *nwk_anno_get(const char *anno, const char *key);

#endif /* BPP_TREE_NEWICK_H */
