#include "introgress.h"
#include "tree.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

void introlist_init(IntroList *g) { g->items = NULL; g->count = g->cap = 0; }

void introlist_free(IntroList *g)
{
    for (int i = 0; i < g->count; i++) {
        free(g->items[i].donor);
        free(g->items[i].recip);
        free(g->items[i].label);
        free(g->items[i].label2);
    }
    free(g->items);
    introlist_init(g);
}

static IntroEvent *introlist_grow(IntroList *g)
{
    if (g->count == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        g->items = xrealloc(g->items, (size_t)g->cap * sizeof(IntroEvent));
    }
    IntroEvent *e = &g->items[g->count++];
    memset(e, 0, sizeof(*e));
    e->phi = 0.5; e->phi2 = -1.0; e->src = TAU_BRANCH; e->dst = TAU_BRANCH;
    return e;
}

void introlist_copy(IntroList *dst, const IntroList *src)
{
    introlist_init(dst);
    for (int i = 0; i < src->count; i++) {
        IntroEvent *e = introlist_grow(dst);
        const IntroEvent *s = &src->items[i];
        e->donor = xstrdup(s->donor);
        e->recip = xstrdup(s->recip);
        e->phi = s->phi; e->phi2 = s->phi2; e->bidir = s->bidir;
        e->src = s->src; e->dst = s->dst;
        e->label  = s->label  ? xstrdup(s->label)  : NULL;
        e->label2 = s->label2 ? xstrdup(s->label2) : NULL;
    }
}

int introlist_find_pair(const IntroList *g, const char *a, const char *b)
{
    for (int i = 0; i < g->count; i++) {
        const char *x = g->items[i].donor, *y = g->items[i].recip;
        if ((strcmp(x, a) == 0 && strcmp(y, b) == 0) ||
            (strcmp(x, b) == 0 && strcmp(y, a) == 0)) return i;
    }
    return -1;
}

/* --- parsing ----------------------------------------------------------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    return s;
}

static int parse_tau(const char *v, TauEnd *out)
{
    if (strcmp(v, "branch") == 0 || strcmp(v, "yes") == 0) { *out = TAU_BRANCH; return 1; }
    if (strcmp(v, "node")   == 0 || strcmp(v, "no")  == 0) { *out = TAU_NODE;   return 1; }
    return 0;
}

void introlist_parse(IntroList *g, const char *spec, DiagList *errs)
{
    const char *p = spec;
    while (*p) {
        size_t len = strcspn(p, ",;");
        char *piece = xstrndup(p, len);
        p += len; if (*p) p++;

        int bidir = 0;
        char *arrow = strstr(piece, "<->");
        size_t alen = 3;
        if (arrow) bidir = 1;
        else { arrow = strstr(piece, "->"); alen = 2; }
        if (!arrow) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression '%s' is not of the form DONOR->RECIP (or DONOR<->RECIP).",
                trim(piece));
            free(piece); continue;
        }
        *arrow = '\0';
        char *donor = trim(piece);
        char *rest  = arrow + alen;
        while (*rest == ' ' || *rest == '\t') rest++;
        size_t rlen = strcspn(rest, " \t=");   /* '=' begins an optional '= NAME' */
        char *recip = xstrndup(rest, rlen);
        char *opts  = rest + rlen;

        if (*donor == '\0' || *recip == '\0') {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: missing donor or recipient.");
            free(recip); free(piece); continue;
        }

        IntroEvent ev; memset(&ev, 0, sizeof ev);
        ev.donor = xstrdup(donor); ev.recip = xstrdup(recip);
        ev.phi = 0.5; ev.phi2 = -1.0; ev.src = TAU_BRANCH; ev.dst = TAU_BRANCH;
        ev.bidir = bidir;

        int bad = 0;
        char *o = opts;
        /* optional '= NAME' names the event (synonym for label=NAME), mirroring
         * the join syntax 'A+B = pan'. */
        while (*o == ' ' || *o == '\t') o++;
        if (*o == '=') {
            o++;
            while (*o == ' ' || *o == '\t') o++;
            size_t l = strcspn(o, " \t");
            if (l) { free(ev.label); ev.label = xstrndup(o, l); o += l; }
        }
        while (*o && !bad) {
            while (*o == ' ' || *o == '\t') o++;
            if (!*o) break;
            size_t tlen = strcspn(o, " \t");
            char *tok = xstrndup(o, tlen);
            o += tlen;
            char *eq = strchr(tok, '=');
            if (!eq) {
                diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "introgression: option '%s' is not key=value.", tok);
                bad = 1;
            } else {
                *eq = '\0'; char *key = tok, *val = eq + 1;
                if (strcmp(key, "phi") == 0)        ev.phi = atof(val);
                else if (strcmp(key, "phi2") == 0)  ev.phi2 = atof(val);
                else if (strcmp(key, "label") == 0) { free(ev.label); ev.label = xstrdup(val); }
                else if (strcmp(key, "src") == 0) {
                    if (!parse_tau(val, &ev.src)) {
                        diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                            "introgression: src must be 'branch' or 'node', got '%s'.", val);
                        bad = 1;
                    }
                } else if (strcmp(key, "dst") == 0) {
                    if (!parse_tau(val, &ev.dst)) {
                        diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                            "introgression: dst must be 'branch' or 'node', got '%s'.", val);
                        bad = 1;
                    }
                } else {
                    diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                        "introgression: unknown option '%s'.", key);
                    bad = 1;
                }
            }
            free(tok);
        }

        /* A reciprocal pair (A->B and B->A) is not stacking -- it is an error
         * (or belongs in a bidirectional '<->' event). Same-direction repeats
         * ARE stacking (two pulses on one lineage) and are built by the graph
         * path, so they are allowed through here. */
        int reciprocal = 0;
        for (int i = 0; i < g->count && !reciprocal; i++)
            if (strcmp(g->items[i].donor, ev.recip) == 0 &&
                strcmp(g->items[i].recip, ev.donor) == 0) reciprocal = 1;
        if (!bad && reciprocal) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: more than one event between '%s' and '%s'.",
                ev.donor, ev.recip);
            bad = 1;
        }
        if (bad) {
            free(ev.donor); free(ev.recip); free(ev.label);
        } else {
            IntroEvent *slot = introlist_grow(g);
            *slot = ev;
        }
        free(recip); free(piece);
    }
}

/* --- validation -------------------------------------------------------- */

static int is_anc(const TreeNode *a, const TreeNode *b)
{
    for (const TreeNode *q = b; q; q = q->parent) if (q == a) return 1;
    return 0;
}

static int name_taken(const Resolution *r, const IntroList *g, int upto, const char *name)
{
    if (resolution_find(r, name)) return 1;
    for (int i = 0; i < upto; i++)
        if (g->items[i].label && strcmp(g->items[i].label, name) == 0) return 1;
    return 0;
}

int introlist_apply(IntroList *g, Resolution *r, DiagList *errs)
{
    int ok = 1, autonum = 0;
    for (int k = 0; k < g->count; k++) {
        IntroEvent *e = &g->items[k];
        TreeNode *D = resolution_find(r, e->donor);
        TreeNode *R = resolution_find(r, e->recip);
        if (!D || !R) {
            diag_add(errs, DIAG_INTROGRESSION_UNKNOWN, -1,
                "introgression: %s '%s' is not in the tree.",
                D ? "recipient" : "donor", D ? e->recip : e->donor);
            ok = 0; continue;
        }
        if (D == R) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: donor and recipient are the same ('%s').", e->donor);
            ok = 0; continue;
        }
        if (is_anc(D, R) || is_anc(R, D)) {
            Diagnostic *d = diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: '%s' and '%s' are ancestor and descendant; they do "
                "not coexist in time.", e->donor, e->recip);
            diag_set_hint(d, "an introgression must connect two non-nested "
                             "(contemporaneous) branches.");
            ok = 0; continue;
        }
        if (e->phi <= 0.0 || e->phi >= 1.0) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: phi=%g is out of range (0 < phi < 1).", e->phi);
            ok = 0; continue;
        }
        if (e->bidir) {
            /* BPP Model D requires the two hybrids to be siblings sharing a
             * common parent in the base tree; src/dst placement words are
             * disallowed (BPP rejects any tau annotations on bidir nodes). */
            if (e->src != TAU_BRANCH || e->dst != TAU_BRANCH) {
                Diagnostic *d = diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "introgression: bidirectional events do not accept 'src'/'dst' "
                    "placement (BPP model D requires no tau-parent annotations).");
                diag_set_hint(d, "drop src=/dst= from this <-> event.");
                ok = 0; continue;
            }
            if (!D->parent || D->parent != R->parent) {
                Diagnostic *d = diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "introgression: bidirectional events require '%s' and '%s' to "
                    "be sister branches (share an immediate parent in the tree).",
                    e->donor, e->recip);
                diag_set_hint(d, "model D couples two contemporaneous hybrid "
                                 "lineages; rearrange the tree so they're sisters, "
                                 "or use two unidirectional events between non-sister "
                                 "lineages.");
                ok = 0; continue;
            }
            if (e->phi2 <= 0.0 || e->phi2 >= 1.0)
                e->phi2 = 1.0 - e->phi;   /* sensible default: phi2 = 1 - phi */
        }
        /* a hybrid node has exactly two parents: a node is a recipient once */
        int dup = 0;
        for (int j = 0; j < k; j++)
            if (!g->items[j].bidir && resolution_find(r, g->items[j].recip) == R) { dup = 1; break; }
        if (dup) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: '%s' is the recipient of more than one event (a hybrid "
                "node has two parents).", e->recip);
            ok = 0; continue;
        }

        if (!e->label) {
            char buf[32];
            do { snprintf(buf, sizeof buf, "H%d", ++autonum); }
            while (name_taken(r, g, k, buf));
            e->label = xstrdup(buf);
        }
        if (e->bidir && !e->label2) {
            char buf[32];
            do { snprintf(buf, sizeof buf, "H%d", ++autonum); }
            while (name_taken(r, g, k, buf));
            e->label2 = xstrdup(buf);
        }
        if (!D->is_leaf) D->show_label = 1;   /* donor population must be named */
        if (e->bidir && !R->is_leaf) R->show_label = 1;

        if (e->bidir)
            D->parent->model_d_event = k + 1; /* anchor the coupled Model D form */

        /* display markers. For unidirectional: "H1->" on donor, "->H1(.30)" on
         * recipient. For bidirectional: "Ha<->Hb(.30/.10)" on both endpoints. */
        char p1[16], p2[16];
        snprintf(p1, sizeof p1, "%.2f", e->phi);
        const char *pp1 = p1[0] == '0' ? p1 + 1 : p1;
        if (e->bidir) {
            snprintf(p2, sizeof p2, "%.2f", e->phi2);
            const char *pp2 = p2[0] == '0' ? p2 + 1 : p2;
            char m[96];
            snprintf(m, sizeof m, "%s\xe2\x87\x84%s(%s/%s)",   /* U+21C4 ⇄ */
                     e->label, e->label2, pp1, pp2);
            treenode_add_intro(D, m, k + 1);
            treenode_add_intro(R, m, k + 1);
        } else {
            char dm[48], rm[64];
            snprintf(dm, sizeof dm, "%s\xe2\x87\x9d", e->label);
            snprintf(rm, sizeof rm, "\xe2\x87\x9d%s(%s)", e->label, pp1);
            treenode_add_intro(D, dm, k + 1);
            treenode_add_intro(R, rm, k + 1);
        }
    }
    return ok;
}

/* --- graph construction (stacked networks) ------------------------------ */

typedef struct { const TreeNode *tn; GraphNode *gn; } NodeMap;

static GraphNode *mirror_tree(Graph *g, const TreeNode *tn, GraphNode *parent,
                              NodeMap **map, int *nmap, int *cap)
{
    const char *label = tn->is_leaf ? tn->name : tn->explicit_label;
    GraphNode *n = graph_new_node(g, label);
    n->primary_parent = parent;
    if (*nmap == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *map = xrealloc(*map, (size_t)*cap * sizeof(NodeMap));
    }
    (*map)[*nmap].tn = tn; (*map)[(*nmap)++].gn = n;
    for (int i = 0; i < tn->n_children; i++)
        graph_add_child(n, mirror_tree(g, tn->children[i], n, map, nmap, cap));
    return n;
}

static GraphNode *map_lookup(const NodeMap *map, int n, const TreeNode *tn)
{
    for (int i = 0; i < n; i++) if (map[i].tn == tn) return map[i].gn;
    return NULL;
}

static int name_used(char **names, int n, const Resolution *r, const char *s)
{
    if (resolution_find(r, s)) return 1;
    for (int i = 0; i < n; i++) if (strcmp(names[i], s) == 0) return 1;
    return 0;
}

static void splice_child(GraphNode *parent, GraphNode *old, GraphNode *new)
{
    for (int i = 0; i < parent->n_children; i++)
        if (parent->children[i] == old) { parent->children[i] = new; return; }
}

int introlist_needs_graph(const IntroList *g)
{
    for (int i = 0; i < g->count; i++)
        for (int j = 0; j < i; j++) {
            const char *lab = g->items[j].label;
            if (lab && (strcmp(g->items[i].donor, lab) == 0 ||
                        strcmp(g->items[i].recip, lab) == 0))
                return 1;                              /* endpoint names a prior event */
            if (strcmp(g->items[i].recip, g->items[j].recip) == 0)
                return 1;                              /* recipient repeated -> stacking */
        }
    return 0;
}

Graph *graph_construct(const Resolution *r, const IntroList *events, DiagList *errs)
{
    if (!r || !r->root) return NULL;
    Graph *g = graph_alloc();
    NodeMap *map = NULL; int nmap = 0, mcap = 0;
    g->root = mirror_tree(g, r->root, NULL, &map, &nmap, &mcap);

    char **names = NULL; GraphNode **enode = NULL; int ne = 0;   /* event name -> hybrid */
    int ok = 1, autonum = 0;

    for (int k = 0; k < events->count && ok; k++) {
        const IntroEvent *e = &events->items[k];

        /* resolve endpoints: a prior event's name first, else the base tree */
        GraphNode *R = NULL, *D = NULL;
        for (int i = 0; i < ne; i++) {
            if (strcmp(names[i], e->recip) == 0) R = enode[i];
            if (strcmp(names[i], e->donor) == 0) D = enode[i];
        }
        if (!R) { TreeNode *t = resolution_find(r, e->recip); if (t) R = map_lookup(map, nmap, t); }
        if (!D) { TreeNode *t = resolution_find(r, e->donor); if (t) D = map_lookup(map, nmap, t); }
        if (!R || !D) {
            diag_add(errs, DIAG_INTROGRESSION_UNKNOWN, -1,
                "introgression: %s '%s' is not in the tree.",
                R ? "donor" : "recipient", R ? e->donor : e->recip);
            ok = 0; break;
        }
        if (R == D) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: donor and recipient are the same ('%s').", e->recip);
            ok = 0; break;
        }
        if (!R->primary_parent || !D->primary_parent) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: cannot attach an event at the root.");
            ok = 0; break;
        }
        for (GraphNode *a = R; a && ok; a = a->primary_parent)
            if (a == D) { diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: '%s' is an ancestor of '%s'; they do not coexist.",
                e->donor, e->recip); ok = 0; }
        for (GraphNode *a = D; a && ok; a = a->primary_parent)
            if (a == R) { diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: '%s' is an ancestor of '%s'; they do not coexist.",
                e->recip, e->donor); ok = 0; }
        if (!ok) break;
        if (e->phi <= 0.0 || e->phi >= 1.0) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "introgression: phi=%g is out of range (0 < phi < 1).", e->phi);
            ok = 0; break;
        }

        char *name = NULL;
        if (e->label) {
            if (strchr(e->label, '_')) {
                diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "introgression: name '%s' must not contain '_'.", e->label);
                ok = 0; break;
            }
            if (name_used(names, ne, r, e->label)) {
                diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                    "introgression: name '%s' is already a tip, clade, or event.", e->label);
                ok = 0; break;
            }
            name = xstrdup(e->label);
        } else {
            char buf[32];
            do { snprintf(buf, sizeof buf, "H%d", ++autonum); }
            while (name_used(names, ne, r, buf));
            name = xstrdup(buf);
        }

        /* insert hybrid H immediately above the recipient (latest innermost) */
        GraphNode *pR = R->primary_parent;
        GraphNode *H = graph_new_node(g, name);
        H->is_hybrid = 1;
        H->phi = e->phi;
        H->tau_primary   = (e->dst == TAU_BRANCH);   /* recipient (native) edge */
        H->tau_secondary = (e->src == TAU_BRANCH);   /* donor edge */
        H->primary_parent = pR;
        graph_add_child(H, R);
        R->primary_parent = H;
        splice_child(pR, R, H);

        /* insert an anonymous donor-attachment immediately above the donor. The
         * bare hybrid reference is added as the FIRST child: BPP pairs the phi
         * of stacked hybrids by child order, and rejects ("phi do not sum to 1")
         * a donor attachment whose bare ref trails its subtree. */
        GraphNode *pD = D->primary_parent;
        GraphNode *Dn = graph_new_node(g, NULL);
        Dn->primary_parent = pD;
        splice_child(pD, D, Dn);
        graph_add_child(Dn, H);
        H->secondary_parent = Dn;
        graph_add_child(Dn, D);
        D->primary_parent = Dn;

        names = xrealloc(names, (size_t)(ne + 1) * sizeof *names);
        enode = xrealloc(enode, (size_t)(ne + 1) * sizeof *enode);
        names[ne] = name; enode[ne] = H; ne++;
        g->n_hybrids++;
    }

    for (int i = 0; i < ne; i++) free(names[i]);
    free(names); free(enode); free(map);
    if (!ok) { graph_free(g); return NULL; }
    return g;
}

void introlist_from_graph(IntroList *g, const Graph *gr, Resolution *r)
{
    int n = 0;
    GraphEvent *ev = graph_events(gr, &n);
    for (int k = 0; k < n; k++) {
        IntroEvent *e = introlist_grow(g);
        e->donor = xstrdup(ev[k].donor);
        e->recip = xstrdup(ev[k].recip);
        e->label = xstrdup(ev[k].name);
        e->phi   = ev[k].phi;
        e->phi2  = -1.0;
        e->bidir = 0;
        /* src/dst chosen so model_letter() reproduces the graph's model: a
         * TAU_BRANCH ('own tau') counts as one 'yes'. A=2, B=1, C=0. */
        int yes = ev[k].model == 'A' ? 2 : ev[k].model == 'B' ? 1 : 0;
        e->src = yes >= 1 ? TAU_BRANCH : TAU_NODE;
        e->dst = yes >= 2 ? TAU_BRANCH : TAU_NODE;

        /* mark the resolved base tree (a population may be donor/recipient of
         * more than one event -- stacked pulses -- so just append markers). */
        TreeNode *D = resolution_find(r, e->donor);
        TreeNode *R = resolution_find(r, e->recip);
        if (D && !D->is_leaf) D->show_label = 1;
        char p1[16];
        snprintf(p1, sizeof p1, "%.2f", e->phi);
        const char *pp = p1[0] == '0' ? p1 + 1 : p1;
        char dm[48], rm[64];
        snprintf(dm, sizeof dm, "%s\xe2\x87\x9d", e->label);
        snprintf(rm, sizeof rm, "\xe2\x87\x9d%s(%s)", e->label, pp);
        if (D) treenode_add_intro(D, dm, k + 1);
        if (R) treenode_add_intro(R, rm, k + 1);
    }
    graph_events_free(ev, n);
}

/* --- extended-Newick emission ------------------------------------------ */

static const char *tau_str(TauEnd e) { return e == TAU_NODE ? "no" : "yes"; }

static char model_letter(const IntroEvent *e)
{
    if (e->bidir) return 'D';
    int yes = (e->src == TAU_BRANCH) + (e->dst == TAU_BRANCH);
    return yes == 2 ? 'A' : yes == 1 ? 'B' : 'C';
}

/* Emit the bare subtree of n, with recursive children but without applying any
 * introgression wrapping at n itself (used for the children of a Model D
 * anchor, which carry their normal subtrees inside the coupled form). */
static char *emit_subtree(const IntroList *g, TreeNode **dn, TreeNode **rn,
                          const TreeNode *n);

static char *emit_rec(const IntroList *g, TreeNode **dn, TreeNode **rn,
                      const TreeNode *n)
{
    char *core;
    /* Model D anchor: emit the coupled form for its two children A and B. */
    if (!n->is_leaf && n->model_d_event > 0) {
        const IntroEvent *e = &g->items[n->model_d_event - 1];
        TreeNode *D = dn[n->model_d_event - 1];
        TreeNode *R = rn[n->model_d_event - 1];
        char *as = emit_subtree(g, dn, rn, D);
        char *bs = emit_subtree(g, dn, rn, R);
        /* ((A_sub, Hb[&phi=phi1])Ha, (B_sub, Ha[&phi=phi2])Hb)P
         *   bare Ha annotation = edge Hb->Ha (phi2 = recip->donor)
         *   bare Hb annotation = edge Ha->Hb (phi1 = donor->recip)         */
        core = xasprintf("((%s,%s[&phi=%g])%s,(%s,%s[&phi=%g])%s)%s",
                         as, e->label2, e->phi,  e->label,
                         bs, e->label,  e->phi2, e->label2,
                         n->show_label ? treenode_bpp_name(n) : "");
        free(as); free(bs);
        return core;
    }
    if (n->is_leaf) {
        core = xstrdup(n->name);
    } else {
        core = xstrdup("(");
        for (int i = 0; i < n->n_children; i++) {
            char *c = emit_rec(g, dn, rn, n->children[i]);
            char *t = xasprintf("%s%s%s", core, i ? "," : "", c);
            free(core); free(c); core = t;
        }
        char *t = xasprintf("%s)%s", core, n->show_label ? treenode_bpp_name(n) : "");
        free(core); core = t;
    }
    /* recipient: this lineage flows up into a hybrid node (subtree occurrence) */
    for (int k = 0; k < g->count; k++) {
        if (g->items[k].bidir) continue;
        if (rn[k] == n) {
            char *t = xasprintf("(%s)%s[&tau-parent=%s]",
                                core, g->items[k].label, tau_str(g->items[k].dst));
            free(core); core = t;
            break;
        }
    }
    /* donor: graft the bare hybrid reference (carrying phi) as a sibling */
    for (int k = 0; k < g->count; k++) {
        if (g->items[k].bidir) continue;
        if (dn[k] == n) {
            char *t = xasprintf("(%s,%s[&phi=%g,&tau-parent=%s])",
                                core, g->items[k].label, g->items[k].phi,
                                tau_str(g->items[k].src));
            free(core); core = t;
        }
    }
    return core;
}

static char *emit_subtree(const IntroList *g, TreeNode **dn, TreeNode **rn,
                          const TreeNode *n)
{
    if (n->is_leaf) return xstrdup(n->name);
    char *s = xstrdup("(");
    for (int i = 0; i < n->n_children; i++) {
        char *c = emit_rec(g, dn, rn, n->children[i]);
        char *t = xasprintf("%s%s%s", s, i ? "," : "", c);
        free(s); free(c); s = t;
    }
    char *t = xasprintf("%s)%s", s, n->show_label ? treenode_bpp_name(n) : "");
    free(s);
    return t;
}

char *introgress_newick(const IntroList *g, Resolution *r)
{
    if (g->count == 0 || !r->root) return NULL;
    TreeNode **dn = xmalloc((size_t)g->count * sizeof *dn);
    TreeNode **rn = xmalloc((size_t)g->count * sizeof *rn);
    for (int k = 0; k < g->count; k++) {
        dn[k] = resolution_find(r, g->items[k].donor);
        rn[k] = resolution_find(r, g->items[k].recip);
    }
    char *s = emit_rec(g, dn, rn, r->root);
    free(dn); free(rn);
    return s;
}

void introgress_legend(const IntroList *g, Resolution *r, FILE *fp, int color)
{
    if (g->count == 0) return;
    fputs("introgressions:\n", fp);
    for (int k = 0; k < g->count; k++) {
        const IntroEvent *e = &g->items[k];
        TreeNode *D = resolution_find(r, e->donor);
        TreeNode *R = resolution_find(r, e->recip);
        const char *dn = D ? treenode_bpp_name(D) : e->donor;
        const char *rn = R ? treenode_bpp_name(R) : e->recip;
        fputs("  ", fp);
        if (color) fputs(treenode_mig_color(k + 1), fp);
        fputs(e->label, fp);
        if (e->bidir && e->label2) { fputc('/', fp); fputs(e->label2, fp); }
        if (color) fputs(TREENODE_MIG_RESET, fp);
        if (e->bidir)
            fprintf(fp, ":  %s \xe2\x87\x84 %s   phi=%g / %g  [model D]\n",
                    dn, rn, e->phi, e->phi2);
        else
            fprintf(fp, ":  %s \xe2\x87\x9d %s   phi=%g  [model %c]\n",
                    dn, rn, e->phi, model_letter(e));
    }
}
