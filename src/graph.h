/* graph.h — the MSC-I network as a single graph (design: docs/graph-model.md).
 *
 * A tree whose branches may carry hybrid nodes. A hybrid node has TWO parents:
 * its `primary_parent` (the recipient's own lineage, the vertical edge) and its
 * `secondary_parent` (the donor attachment, the horizontal/introgression edge).
 * `phi` is the donor's contribution -- the inheritance weight on the secondary
 * edge -- so the recipient keeps `1 - phi` from its primary parent.
 *
 * The graph is built FAITHFULLY from extended Newick: each label that occurs
 * twice is one hybrid node; the occurrence carrying a subtree fixes its primary
 * parent and child, the bare occurrence fixes its secondary parent. No tree
 * rewriting, no donor/recipient guessing -- so stacked introgressions (two
 * reticulations on one lineage, e.g. the akey "M3" network) parse with no
 * special handling. Re-serialising the graph is idempotent by construction.
 *
 * Bidirectional (BPP model D) networks are not yet represented here; building
 * one reports a diagnostic and returns NULL (callers fall back to the legacy
 * IntroList path). Models A/B/C -- every unidirectional MSC-I network -- are
 * fully supported.
 */
#ifndef BPP_TREE_GRAPH_H
#define BPP_TREE_GRAPH_H

#include "newick.h"
#include "diag.h"

typedef struct GraphNode GraphNode;

struct GraphNode {
    char       *label;            /* tip / clade / hybrid(event) name; owned. NULL = anon internal */
    GraphNode **children;         /* primary-tree children (owned array; nodes owned by Graph) */
    int         n_children, cap;
    GraphNode  *primary_parent;   /* non-owning; NULL at the root */

    int         is_hybrid;        /* 1 -> this node is a reticulation (two parents) */
    /* hybrid-only fields: */
    GraphNode  *secondary_parent; /* the donor attachment node; non-owning */
    double      phi;              /* donor's contribution, weight on the secondary edge, in (0,1) */
    int         tau_primary;      /* 1 = own tau (tau-parent=yes) on the primary (recipient) edge */
    int         tau_secondary;    /* 1 = own tau (tau-parent=yes) on the secondary (donor) edge */
};

typedef struct {
    GraphNode  *root;
    GraphNode **nodes;            /* every node, owned -- the free list */
    int         n_nodes, cap;
    int         n_hybrids;        /* number of reticulations */
} Graph;

/* Build the network graph from a parsed extended-Newick tree. Returns a new
 * Graph (caller frees with graph_free), or NULL on a malformed network (a
 * diagnostic is appended to errs: a label occurring other than twice, a hybrid
 * missing one of its occurrences, or a model-D bidirectional form). */
Graph *graph_from_newick(const NwkNode *root, DiagList *errs);

void   graph_free(Graph *g);

/* 1 if the network is representable by the legacy flat event list: no hybrid
 * sits on another hybrid's lineage (no stacking). 0 means at least one
 * reticulation stacks on another -- only the graph can represent it. A graph
 * with no hybrids is trivially simple. */
int    graph_is_simple(const Graph *g);

/* Serialise the BASE species tree (no trailing ';'): drop every secondary
 * (introgression) edge and suppress each hybrid node and each now-unary
 * donor-attachment node, keeping the real internal clade labels. This is the
 * tree the join formulas describe; parse it back to recover joins for editing
 * and display. Caller frees. */
char  *graph_base_newick(const Graph *g);

/* One reticulation reduced to base-tree populations, for display/legend. The
 * donor is the population on the tau-parent=no (introgression) side; the
 * recipient is the hybrid lineage; phi is the donor's contribution along that
 * edge; model is the BPP letter from the two tau flags. */
typedef struct {
    char  *name;    /* hybrid / event label (owned) */
    char  *donor;   /* base-tree donor population name (owned) */
    char  *recip;   /* base-tree recipient population name (owned) */
    double phi;     /* donor's contribution as displayed, in (0,1) */
    char   model;   /* 'A' | 'B' | 'C' */
} GraphEvent;

/* One GraphEvent per hybrid (count = g->n_hybrids), in node order. Names are
 * projected to the surviving base-tree population (a label, or the implicit
 * '_'-joined leaf set when unlabelled). Caller frees with graph_events_free. */
GraphEvent *graph_events(const Graph *g, int *n_out);
void        graph_events_free(GraphEvent *ev, int n);

/* Serialise the graph back to extended Newick (no trailing ';'). phi is written
 * on the donor-side bare reference as the donor's contribution; each hybrid's
 * primary occurrence carries its subtree and tau-parent flag. Deterministic and
 * idempotent: parse -> graph -> string -> parse -> string is byte-stable.
 * Caller frees. */
char  *graph_to_newick(const Graph *g);

#endif /* BPP_TREE_GRAPH_H */
