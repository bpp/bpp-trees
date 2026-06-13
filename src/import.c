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
    memset(im, 0, sizeof *im);
}

/* --- MSC-I recovery ----------------------------------------------------- */

/* Find every occurrence (label, parent, index_among_siblings, this_node) for
 * a given label. Each hybrid label has exactly two occurrences (one labelled,
 * one bare; or two labelled in some eNewick forms). */
typedef struct { NwkNode *parent; NwkNode *node; } LabOcc;

/* During recovery the tree still contains the bare hybrid references of events
 * not yet unwrapped. Leaf-name collection (for a clade's implicit label) must
 * ignore them, or a recipient/donor clade that encloses another event's bare
 * ref would get a polluted implicit label like 'ALTAI_CHAG_VINDIJA_nh_hyb'.
 * Set for the duration of recover_introgressions, cleared before it returns. */
static const char **g_hyb_labels = NULL;
static int g_hyb_count = 0;

static int is_hybrid_label(const char *s)
{
    if (!s) return 0;
    for (int i = 0; i < g_hyb_count; i++)
        if (strcmp(g_hyb_labels[i], s) == 0) return 1;
    return 0;
}

static void find_occ(NwkNode *n, NwkNode *parent, const char *label,
                     LabOcc *out, int *count)
{
    if (n->label && strcmp(n->label, label) == 0) {
        out[(*count)++] = (LabOcc){ parent, n };
    }
    for (int i = 0; i < n->n_children; i++) find_occ(n->children[i], n, label, out, count);
}

/* All distinct labels in the tree (caller frees the array but not the
 * strings, which alias the tree's own labels). */
static void collect_labels(NwkNode *n, const char ***out, int *cnt, int *cap)
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
    for (int i = 0; i < n->n_children; i++) collect_labels(n->children[i], out, cnt, cap);
}

/* Pick the "definition" occurrence (the one carrying the real subtree).
 * Heuristic that matches both forms used in BPP examples: the occurrence
 * with the most children is the definition; ties break to the one without
 * &phi= (phi annotates the bare/donor reference). */
static int pick_def(const LabOcc *occ, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++) {
        int wi = occ[i].node->n_children, wb = occ[best].node->n_children;
        if (wi > wb) { best = i; continue; }
        if (wi == wb) {
            char *pi = nwk_anno_get(occ[i].node->annotation, "phi");
            char *pb = nwk_anno_get(occ[best].node->annotation, "phi");
            if (pi && !pb) {}                  /* keep best (it lacks phi)   */
            else if (!pi && pb) { best = i; }  /* i lacks phi, prefer it     */
            free(pi); free(pb);
        }
    }
    return best;
}

/* Drop the bare-reference occurrence from its parent's child list (it's a
 * pointer into the tree, not the definition). */
static void drop_child(NwkNode *parent, NwkNode *child)
{
    for (int i = 0; i < parent->n_children; i++) {
        if (parent->children[i] == child) {
            nwk_free(child);
            for (int k = i; k < parent->n_children - 1; k++)
                parent->children[k] = parent->children[k + 1];
            parent->n_children--;
            return;
        }
    }
}

/* Convert an unsigned-int annotated node back to its plain label. */
static void strip_annotation(NwkNode *n) { free(n->annotation); n->annotation = NULL; }

static char *node_name(const NwkNode *n);    /* forward decl: defined below */

/* Recover MSC-I events from doubled hybrid labels. Builds events into
 * im->intro and rewrites the tree in place to its base topology by removing
 * bare-reference occurrences. Returns 1 if any MSC-I annotation was found. */
/* Count occurrences of a label in the tree. */
static int label_count(NwkNode *n, const char *L)
{
    int c = (n->label && strcmp(n->label, L) == 0) ? 1 : 0;
    for (int i = 0; i < n->n_children; i++) c += label_count(n->children[i], L);
    return c;
}

static int recover_introgressions(NwkNode *root, Import *im, DiagList *errs)
{
    const char **all = NULL; int total = 0, total_cap = 0;
    collect_labels(root, &all, &total, &total_cap);
    /* Restrict to labels that appear more than once: those are hybrid nodes.
     * Own a COPY of each: rewriting the tree below frees hybrid nodes (and their
     * label strings) as events are unwrapped, but labels[] is consulted across
     * all iterations (e.g. the bare-hybrid check), so it must not alias freed
     * tree strings. */
    char **labels = NULL; int n = 0;
    for (int i = 0; i < total; i++)
        if (label_count(root, all[i]) >= 2) {
            labels = xrealloc(labels, (size_t)(n + 1) * sizeof(*labels));
            labels[n++] = xstrdup(all[i]);
        }
    free(all);
    g_hyb_labels = (const char **)labels; g_hyb_count = n;

    int had_anno = 0;
    for (int i = 0; i < n; i++) {
        LabOcc occ[8]; int oc = 0;
        find_occ(root, NULL, labels[i], occ, &oc);
        if (oc < 2) continue;                              /* unique label -- plain internal */
        had_anno = 1;
        if (oc > 2) {
            diag_add(errs, DIAG_INTROGRESSION_INVALID, -1,
                "imported tree: label '%s' appears %d times (hybrid nodes have 2).",
                labels[i], oc);
            continue;
        }
        int def = pick_def(occ, 2);
        int ref = 1 - def;
        NwkNode *defn = occ[def].node, *refn = occ[ref].node;
        NwkNode *defp = occ[def].parent, *refp = occ[ref].parent;
        /* phi may be annotated on EITHER occurrence. Per BPP, &phi=X on an
         * occurrence is the inheritance weight of the edge (that occurrence's
         * parent -> hybrid node); the other occurrence's edge carries 1-X.
         * Our ev.phi is the DONOR's contribution -- the weight on the donor
         * edge -- so phi read from the donor-side occurrence is used as-is, and
         * phi read from the recipient's primary-parent occurrence is
         * complemented to 1-X. The donor side is refn normally, defn on a swap
         * (computed just below). */
        char *phi_ref = nwk_anno_get(refn->annotation, "phi");
        char *phi_def = nwk_anno_get(defn->annotation, "phi");
        double phi_val; int phi_on_ref;
        if (phi_ref)      { phi_val = atof(phi_ref); phi_on_ref = 1; }
        else if (phi_def) { phi_val = atof(phi_def); phi_on_ref = 0; }
        else              { phi_val = 0.5;           phi_on_ref = 1; }
        char *def_tau = nwk_anno_get(defn->annotation, "tau-parent");
        char *ref_tau = nwk_anno_get(refn->annotation, "tau-parent");

        /* Per BPP semantics, the species-tree parent (where the recipient lives
         * in the base tree) is the one with `tau-parent=yes` (its own tau).
         * The labelled occurrence carries the subtree but it might be at the
         * SECONDARY parent's location -- as in the yeast example, where the
         * labelled (Sbay)H is at D (tau-parent=no) and the bare H is at R
         * (tau-parent=yes), so Sbay is basal in the base tree, not Skud-sister.
         * When that's so we swap: move the recipient subtree to refp. */
        int swap = (ref_tau && strcasecmp(ref_tau, "yes") == 0 &&
                    def_tau && strcasecmp(def_tau, "no")  == 0);

        IntroEvent ev; memset(&ev, 0, sizeof ev);
        /* donor side is refn (no swap) or defn (swap); use phi as-is there,
         * else complement. */
        ev.phi   = (swap ? !phi_on_ref : phi_on_ref) ? phi_val : 1.0 - phi_val;
        ev.phi2  = -1.0;
        ev.label = xstrdup(labels[i]);
        /* In our overlay-emit, src tau is on the bare ref (donor's location).
         * Standard: bare = refn (under refp); src = ref_tau.  After swap:
         * donor stays at defp (def_tau drives src); recipient ends up at refp
         * (ref_tau drives dst). */
        ev.src = ((swap ? def_tau : ref_tau)
                  && strcasecmp(swap ? def_tau : ref_tau, "no") == 0) ? TAU_NODE : TAU_BRANCH;
        ev.dst = ((swap ? ref_tau : def_tau)
                  && strcasecmp(swap ? ref_tau : def_tau, "no") == 0) ? TAU_NODE : TAU_BRANCH;

        /* recipient = the labelled occurrence's `real' subtree (skipping any
         * bare-hybrid children -- the (B)Y / (X) pieces in Model D). */
        NwkNode *recip_n = NULL;
        for (int k = 0; k < defn->n_children; k++) {
            NwkNode *c = defn->children[k];
            int bare_hybrid = 0;
            if (c->n_children == 0 && c->label) {
                for (int q = 0; q < n; q++)
                    if (strcmp(c->label, labels[q]) == 0) { bare_hybrid = 1; break; }
            }
            if (!bare_hybrid) { recip_n = c; break; }
        }
        if (!recip_n) recip_n = defn;
        ev.recip = node_name(recip_n);
        /* donor = sibling of the hybrid-bearing child at the SECONDARY parent.
         * Standard: secondary = refp, hybrid-bearing = refn.
         * Swap:     secondary = defp, hybrid-bearing = defn. */
        NwkNode *sec_p = swap ? defp : refp;
        NwkNode *sec_h = swap ? defn : refn;
        NwkNode *donor_n = NULL;
        for (int k = 0; k < sec_p->n_children; k++) {
            if (sec_p->children[k] == sec_h) continue;
            donor_n = sec_p->children[k]; break;
        }
        ev.donor = donor_n ? node_name(donor_n) : xstrdup("?");

        if (im->intro.count == im->intro.cap) {
            im->intro.cap = im->intro.cap ? im->intro.cap * 2 : 4;
            im->intro.items = xrealloc(im->intro.items,
                                       (size_t)im->intro.cap * sizeof(IntroEvent));
        }
        im->intro.items[im->intro.count++] = ev;

        /* Rewrite the tree to its base form. */
        if (refp && defp) {
            if (swap) {
                /* The species-tree parent is refp. Move recip_n there (in
                 * place of refn), then drop defn from defp -- defn is gone, the
                 * recipient subtree now lives at refp. */
                for (int k = 0; k < defn->n_children; k++) {
                    if (defn->children[k] == recip_n) {
                        for (int q = k; q < defn->n_children - 1; q++)
                            defn->children[q] = defn->children[q + 1];
                        defn->n_children--;
                        break;
                    }
                }
                for (int k = 0; k < refp->n_children; k++) {
                    if (refp->children[k] == refn) {
                        refp->children[k] = recip_n;
                        break;
                    }
                }
                nwk_free(refn);
                for (int k = 0; k < defp->n_children; k++) {
                    if (defp->children[k] == defn) {
                        for (int q = k; q < defp->n_children - 1; q++)
                            defp->children[q] = defp->children[q + 1];
                        defp->n_children--;
                        break;
                    }
                }
                nwk_free(defn);
            } else {
                /* Standard: keep defn's location, strip the hybrid wrap; remove
                 * the bare reference and its enclosing pair. */
                if (defn->n_children == 1) {
                    NwkNode *child = defn->children[0];
                    for (int k = 0; k < defp->n_children; k++) {
                        if (defp->children[k] == defn) {
                            defp->children[k] = child;
                            defn->n_children = 0; free(defn->children); defn->children = NULL;
                            nwk_free(defn);
                            break;
                        }
                    }
                } else {
                    strip_annotation(defn);
                }
                drop_child(refp, refn);
            }
        }
        free(phi_ref); free(phi_def); free(def_tau); free(ref_tau);
    }
    g_hyb_labels = NULL; g_hyb_count = 0;
    for (int i = 0; i < n; i++) free(labels[i]);
    free(labels);

    /* Detect Model D: pairs of recovered events with reciprocal donor/recipient
     * are a single bidirectional event coupled across two hybrid nodes. Merge
     * them so re-emitting reproduces the coupled BPP Model D form. */
    for (int i = 0; i < im->intro.count; i++) {
        for (int j = i + 1; j < im->intro.count; j++) {
            IntroEvent *a = &im->intro.items[i], *b = &im->intro.items[j];
            if (a->bidir || b->bidir) continue;
            if (strcmp(a->donor, b->recip) == 0 && strcmp(a->recip, b->donor) == 0) {
                /* Merge into one bidir event in b's direction (donor=A, recip=B).
                 * In the emitted form '((A,label2[phi])label,(B,label[phi2])label2)',
                 * `label` sits at the donor's location and `label2` at the recipient's.
                 * From the recovery: a was the (B->A) decomposition whose `label`
                 * was at A's location -> that becomes the donor-side label; b was
                 * the (A->B) one whose `label` was at B's location -> label2. */
                b->bidir = 1;
                b->phi2 = a->phi;                  /* B->A direction's phi */
                free(b->label2); b->label2 = b->label;     /* was at B's loc */
                b->label = a->label; a->label = NULL;      /* now at A's loc */
                free(a->donor); free(a->recip);
                for (int k = i; k < im->intro.count - 1; k++)
                    im->intro.items[k] = im->intro.items[k + 1];
                im->intro.count--;
                i--; break;
            }
        }
    }

    return had_anno;
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
        if (is_hybrid_label(n->label)) return 1;   /* skip bare hybrid references */
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

    im->had_msci = recover_introgressions(root, im, errs);
    root = suppress_unary(root);

    if (root->n_children == 0) {
        diag_add(errs, DIAG_SYNTAX, -1, "imported tree is empty.");
        nwk_free(root); return 0;
    }
    walk(root, im);
    nwk_free(root);
    return 1;
}
