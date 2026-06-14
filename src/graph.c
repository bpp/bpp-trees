#define _POSIX_C_SOURCE 200809L

#include "graph.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- ownership ----------------------------------------------------------- */

static GraphNode *gn_alloc(Graph *g)
{
    GraphNode *n = xcalloc(1, sizeof *n);
    if (g->n_nodes == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 32;
        g->nodes = xrealloc(g->nodes, (size_t)g->cap * sizeof(GraphNode *));
    }
    g->nodes[g->n_nodes++] = n;
    return n;
}

static void gn_add_child(GraphNode *p, GraphNode *c)
{
    if (p->n_children == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->children = xrealloc(p->children, (size_t)p->cap * sizeof(GraphNode *));
    }
    p->children[p->n_children++] = c;
}

Graph *graph_alloc(void)
{
    return xcalloc(1, sizeof(Graph));
}

GraphNode *graph_new_node(Graph *g, const char *label)
{
    GraphNode *n = gn_alloc(g);
    n->label = label ? xstrdup(label) : NULL;
    return n;
}

void graph_add_child(GraphNode *parent, GraphNode *child)
{
    gn_add_child(parent, child);
}

void graph_free(Graph *g)
{
    if (!g) return;
    for (int i = 0; i < g->n_nodes; i++) {
        free(g->nodes[i]->children);
        free(g->nodes[i]->label);
        free(g->nodes[i]);
    }
    free(g->nodes);
    free(g);
}

/* --- hybrid-label table -------------------------------------------------- */

typedef struct {
    char      *label;     /* aliases a tree label; not owned */
    GraphNode *node;      /* the single shared hybrid node */
    int        saw_def;   /* the subtree-bearing (primary) occurrence seen */
    int        saw_bare;  /* the bare (secondary/donor) occurrence seen */
    int        phi_set;   /* phi read from one of the occurrences */
} HybEnt;

typedef struct { HybEnt *ent; int n; } HybTab;

static int hyb_index(const HybTab *ht, const char *label)
{
    if (!label) return -1;
    for (int i = 0; i < ht->n; i++)
        if (strcmp(ht->ent[i].label, label) == 0) return i;
    return -1;
}

static int label_count(const NwkNode *n, const char *lab)
{
    int c = (n->label && strcmp(n->label, lab) == 0) ? 1 : 0;
    for (int i = 0; i < n->n_children; i++) c += label_count(n->children[i], lab);
    return c;
}

/* Collect every distinct non-empty label (aliasing the tree's own strings). */
static void collect_labels(const NwkNode *n, const char ***out, int *cnt, int *cap)
{
    if (n->label && *n->label) {
        int seen = 0;
        for (int i = 0; i < *cnt; i++)
            if (strcmp((*out)[i], n->label) == 0) { seen = 1; break; }
        if (!seen) {
            if (*cnt == *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *out = xrealloc(*out, (size_t)*cap * sizeof(const char *));
            }
            (*out)[(*cnt)++] = n->label;
        }
    }
    for (int i = 0; i < n->n_children; i++)
        collect_labels(n->children[i], out, cnt, cap);
}

/* --- faithful build ------------------------------------------------------ */

static int tau_yes(const char *anno)
{
    char *t = nwk_anno_get(anno, "tau-parent");
    int yes = !t || strcasecmp(t, "yes") == 0;   /* default: own tau */
    free(t);
    return yes;
}

static GraphNode *build(Graph *g, const NwkNode *nn, GraphNode *parent,
                        HybTab *ht, DiagList *errs, int *bad)
{
    int hi = hyb_index(ht, nn->label);
    if (hi >= 0) {
        HybEnt *e = &ht->ent[hi];
        GraphNode *H = e->node;
        H->is_hybrid = 1;
        char *phis = nwk_anno_get(nn->annotation, "phi");
        if (nn->n_children > 0) {                /* definition / primary occurrence */
            if (e->saw_def) {
                diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "imported network: hybrid '%s' has two subtree occurrences.",
                    nn->label);
                *bad = 1;
            }
            e->saw_def = 1;
            if (!H->label) H->label = xstrdup(nn->label);
            H->primary_parent = parent;
            H->tau_primary = tau_yes(nn->annotation);
            if (phis) { H->phi = 1.0 - atof(phis); e->phi_set = 1; }  /* primary edge weight -> complement */
            for (int i = 0; i < nn->n_children; i++)
                gn_add_child(H, build(g, nn->children[i], H, ht, errs, bad));
        } else {                                 /* bare / secondary (donor) occurrence */
            if (e->saw_bare) {
                diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "imported network: hybrid '%s' has two bare occurrences.",
                    nn->label);
                *bad = 1;
            }
            e->saw_bare = 1;
            H->secondary_parent = parent;
            H->tau_secondary = tau_yes(nn->annotation);
            if (phis) { H->phi = atof(phis); e->phi_set = 1; } /* secondary edge weight -> donor's, as-is */
        }
        free(phis);
        return H;
    }

    GraphNode *n = gn_alloc(g);
    n->label = nn->label ? xstrdup(nn->label) : NULL;
    n->primary_parent = parent;
    for (int i = 0; i < nn->n_children; i++)
        gn_add_child(n, build(g, nn->children[i], n, ht, errs, bad));
    return n;
}

Graph *graph_from_newick(const NwkNode *root, DiagList *errs)
{
    Graph *g = xcalloc(1, sizeof *g);

    /* Identify hybrid labels: those occurring more than once. */
    const char **all = NULL; int total = 0, total_cap = 0;
    collect_labels(root, &all, &total, &total_cap);

    HybTab ht = { NULL, 0 };
    int bad = 0;
    for (int i = 0; i < total; i++) {
        int c = label_count(root, all[i]);
        if (c < 2) continue;
        if (c > 2) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "imported network: label '%s' appears %d times (a hybrid node "
                "appears exactly twice).", all[i], c);
            bad = 1;
            continue;
        }
        ht.ent = xrealloc(ht.ent, (size_t)(ht.n + 1) * sizeof(HybEnt));
        ht.ent[ht.n].label    = (char *)all[i];
        ht.ent[ht.n].node     = gn_alloc(g);
        ht.ent[ht.n].saw_def  = 0;
        ht.ent[ht.n].saw_bare = 0;
        ht.ent[ht.n].phi_set  = 0;
        ht.n++;
    }
    free(all);
    if (bad) { free(ht.ent); graph_free(g); return NULL; }

    g->root = build(g, root, NULL, &ht, errs, &bad);
    g->n_hybrids = ht.n;

    /* Finalise and validate each hybrid. */
    for (int i = 0; i < ht.n && !bad; i++) {
        HybEnt *e = &ht.ent[i];
        GraphNode *H = e->node;
        if (!e->saw_def || !e->saw_bare) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "imported network: hybrid '%s' is missing its %s occurrence.",
                e->label, e->saw_def ? "donor (bare)" : "recipient (subtree)");
            bad = 1; break;
        }
        if (!e->phi_set) H->phi = 0.5;
        /* model D: two hybrids that are each other's donor (mutual secondary). */
        GraphNode *G = H->secondary_parent;
        if (G && G->is_hybrid && G->secondary_parent == H) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "imported network: '%s'/'%s' form a bidirectional (model D) "
                "reticulation, which the graph model does not yet represent.",
                e->label, G->label ? G->label : "?");
            bad = 1; break;
        }
    }

    free(ht.ent);
    if (bad) { graph_free(g); return NULL; }
    return g;
}

/* --- serialisation ------------------------------------------------------- */

static char *bare_ref(const GraphNode *h)
{
    return xasprintf("%s[&phi=%g,&tau-parent=%s]",
                     h->label ? h->label : "?", h->phi,
                     h->tau_secondary ? "yes" : "no");
}

static char *emit_node(const GraphNode *n)
{
    char *core;
    if (n->n_children == 0) {
        core = xstrdup(n->label ? n->label : "");
    } else {
        core = xstrdup("(");
        for (int i = 0; i < n->n_children; i++) {
            const GraphNode *c = n->children[i];
            char *piece = (c->is_hybrid && c->secondary_parent == n)
                          ? bare_ref(c) : emit_node(c);
            char *t = xasprintf("%s%s%s", core, i ? "," : "", piece);
            free(core); free(piece); core = t;
        }
        char *t = xasprintf("%s)%s", core, n->label ? n->label : "");
        free(core); core = t;
    }
    if (n->is_hybrid) {                          /* primary occurrence carries tau, not phi */
        char *t = xasprintf("%s[&tau-parent=%s]", core, n->tau_primary ? "yes" : "no");
        free(core); core = t;
    }
    return core;
}

char *graph_to_newick(const Graph *g)
{
    if (!g || !g->root) return NULL;
    return emit_node(g->root);
}

/* --- base-tree projection ------------------------------------------------ */

int graph_is_simple(const Graph *g)
{
    if (!g) return 1;
    for (int i = 0; i < g->n_nodes; i++) {
        const GraphNode *n = g->nodes[i];
        if (!n->is_hybrid) continue;
        if ((n->primary_parent   && n->primary_parent->is_hybrid) ||
            (n->secondary_parent && n->secondary_parent->is_hybrid))
            return 0;                            /* a reticulation stacks on another */
    }
    return 1;
}

/* Emit n's base subtree: skip the secondary (donor-ref) children, then suppress
 * this node if it is a hybrid or has collapsed to a single child (a donor
 * attachment whose bare ref was dropped). */
static char *emit_base(const GraphNode *n)
{
    const GraphNode *kids[64]; int nk = 0;
    for (int i = 0; i < n->n_children && nk < 64; i++) {
        const GraphNode *c = n->children[i];
        if (c->is_hybrid && c->secondary_parent == n) continue;  /* drop secondary edge */
        kids[nk++] = c;
    }
    if (nk == 0) return xstrdup(n->label ? n->label : "");       /* leaf */
    if (n->is_hybrid || nk == 1) return emit_base(kids[0]);      /* suppress this node */

    char *core = xstrdup("(");
    for (int i = 0; i < nk; i++) {
        char *piece = emit_base(kids[i]);
        char *t = xasprintf("%s%s%s", core, i ? "," : "", piece);
        free(core); free(piece); core = t;
    }
    char *t = xasprintf("%s)%s", core, n->label ? n->label : "");
    free(core);
    return t;
}

char *graph_base_newick(const Graph *g)
{
    if (!g || !g->root) return NULL;
    return emit_base(g->root);
}

/* --- event projection (display / legend) --------------------------------- */

/* The base-tree node that represents n: descend through hybrid nodes and
 * unary donor-attachment nodes (whose bare ref was dropped) to the surviving
 * clade or tip. */
static const GraphNode *project(const GraphNode *n)
{
    for (;;) {
        const GraphNode *only = NULL; int nk = 0;
        for (int i = 0; i < n->n_children; i++) {
            const GraphNode *c = n->children[i];
            if (c->is_hybrid && c->secondary_parent == n) continue;  /* secondary edge */
            only = c; nk++;
        }
        if (nk == 0) return n;                    /* tip */
        if (n->is_hybrid || nk == 1) { n = only; continue; }  /* suppressed node */
        return n;                                 /* a real internal clade */
    }
}

/* Primary-tree leaf names under n (skipping secondary edges), for an implicit
 * '_'-joined label when a clade has no explicit one. */
static void base_leaves(const GraphNode *n, char ***out, int *cnt, int *cap)
{
    int has_kid = 0;
    for (int i = 0; i < n->n_children; i++) {
        const GraphNode *c = n->children[i];
        if (c->is_hybrid && c->secondary_parent == n) continue;
        has_kid = 1;
        base_leaves(c, out, cnt, cap);
    }
    if (!has_kid) {
        if (*cnt == *cap) { *cap = *cap ? *cap * 2 : 8;
                            *out = xrealloc(*out, (size_t)*cap * sizeof(char *)); }
        (*out)[(*cnt)++] = n->label ? n->label : (char *)"?";
    }
}

static char *project_name(const GraphNode *n)
{
    const GraphNode *p = project(n);
    if (p->label && *p->label) return xstrdup(p->label);
    char **lv = NULL; int c = 0, cap = 0;
    base_leaves(p, &lv, &c, &cap);
    qsort(lv, (size_t)c, sizeof(char *), cmp_str);
    size_t len = 1; for (int i = 0; i < c; i++) len += strlen(lv[i]) + 1;
    char *s = xmalloc(len); s[0] = '\0';
    for (int i = 0; i < c; i++) { if (i) strcat(s, "_"); strcat(s, lv[i]); }
    free(lv);
    return s;
}

GraphEvent *graph_events(const Graph *g, int *n_out)
{
    int n = g ? g->n_hybrids : 0;
    *n_out = n;
    if (!n) return NULL;
    GraphEvent *ev = xcalloc((size_t)n, sizeof *ev);
    int k = 0;
    for (int i = 0; i < g->n_nodes && k < n; i++) {
        const GraphNode *H = g->nodes[i];
        if (!H->is_hybrid) continue;

        /* recipient = H's (single) primary child */
        const GraphNode *recip = NULL;
        for (int j = 0; j < H->n_children; j++) {
            const GraphNode *c = H->children[j];
            if (c->is_hybrid && c->secondary_parent == H) continue;
            recip = c; break;
        }
        /* donor edge is the tau-parent=no side; donor node is its other child */
        const GraphNode *dp = !H->tau_secondary ? H->secondary_parent
                            : !H->tau_primary   ? H->primary_parent
                                                : H->secondary_parent; /* A: pick 2ndary */
        const GraphNode *donor = NULL;
        if (dp) for (int j = 0; j < dp->n_children; j++)
            if (dp->children[j] != H) { donor = dp->children[j]; break; }

        ev[k].name  = xstrdup(H->label ? H->label : "?");
        ev[k].recip = recip ? project_name(recip) : xstrdup("?");
        ev[k].donor = donor ? project_name(donor) : xstrdup("?");
        ev[k].phi   = !H->tau_secondary ? H->phi : 1.0 - H->phi;
        int yes = H->tau_primary + H->tau_secondary;
        ev[k].model = yes == 2 ? 'A' : yes == 1 ? 'B' : 'C';
        int donor_is_secondary = (dp == H->secondary_parent);
        ev[k].tau_src = donor_is_secondary ? H->tau_secondary : H->tau_primary;
        ev[k].tau_dst = donor_is_secondary ? H->tau_primary   : H->tau_secondary;
        k++;
    }
    return ev;
}

void graph_events_free(GraphEvent *ev, int n)
{
    if (!ev) return;
    for (int i = 0; i < n; i++) { free(ev[i].name); free(ev[i].donor); free(ev[i].recip); }
    free(ev);
}
