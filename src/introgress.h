/* introgress.h — MSC-I introgression / hybridization events over a tree.
 *
 * Each event is a reticulation: the RECIPIENT branch gains a second parent
 * from the DONOR branch, with introgression probability phi (the donor's
 * contribution). On output the species tree becomes an extended-Newick
 * network -- the recipient becomes a hybrid node that appears twice: once
 * carrying its subtree at its own location, once as a bare reference under
 * the donor. phi rides the donor (bare-reference) occurrence; tau-parent
 * annotates each occurrence relative to its parent.
 *
 * tau-parent is a per-end MODEL flag, not a time: TAU_BRANCH ("branch": a new
 * node with its own tau, written tau-parent=yes) or TAU_NODE ("node": shares
 * tau with the hybrid node, tau-parent=no). The two flags select BPP models
 * A=(yes,yes), B=(yes,no), C=(no,no); a bidirectional event is model D.
 *
 * A hybrid species `hybrid H : A, C` is just H grafted beside primary parent A
 * plus an introgression C->H, so it needs no separate representation here.
 */
#ifndef BPP_TREE_INTROGRESS_H
#define BPP_TREE_INTROGRESS_H

#include "resolver.h"
#include "diag.h"
#include "graph.h"

#include <stdio.h>

typedef enum { TAU_BRANCH = 0, TAU_NODE = 1 } TauEnd;   /* -> yes / no */

typedef struct {
    char  *donor;       /* source branch (tip or clade) */
    char  *recip;       /* recipient branch -- the hybrid lineage */
    double phi;         /* donor's contribution, 0 < phi < 1 */
    double phi2;        /* reverse contribution if bidir, else < 0 */
    int    bidir;       /* 1 = bidirectional (BPP model D) */
    TauEnd src;         /* placement at the donor end     -> tau-parent of bare ref */
    TauEnd dst;         /* placement at the recipient end -> tau-parent of subtree */
    char  *label;       /* hybrid node label (donor side for bidir)  */
    char  *label2;      /* second hybrid label, bidirectional only   */
} IntroEvent;

typedef struct {
    IntroEvent *items;
    int         count, cap;
} IntroList;

void introlist_init(IntroList *g);
void introlist_free(IntroList *g);
void introlist_copy(IntroList *dst, const IntroList *src);

/* Index of an event between `a` and `b` regardless of direction, or -1. */
int  introlist_find_pair(const IntroList *g, const char *a, const char *b);

/* Parse one or more events separated by ',' or ';'. Each event is
 *   DONOR->RECIP | DONOR<->RECIP  [phi=P] [phi2=P] [src=branch|node]
 *                                 [dst=branch|node] [label=NAME]
 * Defaults: phi=0.5, src=dst=branch (model A), label auto. The "at most one
 * event per taxon pair" rule and syntax errors are reported to errs. */
void introlist_parse(IntroList *g, const char *spec, DiagList *errs);

/* Validate every event against the resolved tree (endpoints exist, differ,
 * are non-nested/contemporaneous; 0<phi<1; a node is a recipient at most
 * once) and assign auto hybrid labels (H1, H2, ...) avoiding name clashes.
 * Sets show_label on clade endpoints. Returns 1 if all events are valid. */
int  introlist_apply(IntroList *g, Resolution *r, DiagList *errs);

/* 1 if `g` describes a stacked network the flat event list cannot hold: a
 * recipient appears more than once, or an endpoint names a prior event. Such a
 * spec must be built with graph_construct, not introlist_apply. */
int  introlist_needs_graph(const IntroList *g);

/* Build the network graph from a base tree and an ordered event list, applying
 * each event by inserting a hybrid node immediately above the recipient and an
 * anonymous donor-attachment above the donor (latest event innermost = stacking).
 * Endpoints resolve to a tip, a clade (resolution_find), or a prior event's
 * name. With check_names set, user-supplied names are validated (unique, no
 * '_', no tip/clade/event collision); pass 0 when re-pinning an imported
 * network whose hybrid labels are already valid (and may contain '_'). Returns
 * NULL on an unresolved/invalid event (diagnostic appended). Caller frees with
 * graph_free. */
Graph *graph_construct(const Resolution *r, const IntroList *events,
                       int check_names, DiagList *errs);

/* Append one event per reticulation (donor/recipient base-tree populations,
 * phi, model via src/dst) derived from the graph -- the event list that,
 * replayed by graph_construct, rebuilds the network. No Resolution needed and
 * no display side effects; used to re-pin events after a base-tree edit. */
void introlist_events(IntroList *g, const Graph *gr);

/* Place display markers on the resolved base tree for the events already in
 * `g` (donor "NAME⇝", recipient "⇝NAME(.P)"), and flag donor clades to show
 * their label. */
void introlist_mark(IntroList *g, Resolution *r);

/* Populate `g` (assumed empty) from a stacked network carried as a graph, and
 * mark the resolved base tree for display: one event per reticulation with the
 * donor/recipient base-tree populations, displayed phi, and model letter (via
 * src/dst). Re-emission still comes from the graph itself; this drives the
 * legend, markers, JSON and the MSC-I note. Skips the strict one-recipient and
 * model-D validation in introlist_apply (a lineage may host several events). */
void introlist_from_graph(IntroList *g, const Graph *gr, Resolution *r);

/* Extended-Newick for the network (no trailing ';'); call after a successful
 * introlist_apply. Returns NULL when there are no events. Caller frees. */
char *introgress_newick(const IntroList *g, Resolution *r);

/* Print a "H1:  C => A   phi=0.30  [model A]" legend (colourised when color). */
void  introgress_legend(const IntroList *g, Resolution *r, FILE *fp, int color);

#endif /* BPP_TREE_INTROGRESS_H */
