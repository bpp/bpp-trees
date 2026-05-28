#define _POSIX_C_SOURCE 200809L

#include "import.h"
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

/* --- Newick parser ------------------------------------------------------- */

typedef struct NwkNode {
    char            *label;       /* tip name or internal label (may be empty) */
    char            *annotation;  /* the [&...] content, sans brackets; NULL */
    struct NwkNode **children;
    int              n_children, cap;
} NwkNode;

static NwkNode *nwk_new(void)
{
    NwkNode *n = xcalloc(1, sizeof *n);
    return n;
}

static void nwk_free(NwkNode *n)
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

static NwkNode *nwk_parse_root(const char *text, DiagList *errs)
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

/* Extract a `key=val` token from "&phi=0.3,&tau-parent=yes" (sans brackets).
 * Returns the value substring (caller frees), or NULL if not present. */
static char *anno_get(const char *anno, const char *key)
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

/* --- MSC-I recovery ----------------------------------------------------- */

/* Find every occurrence (label, parent, index_among_siblings, this_node) for
 * a given label. Each hybrid label has exactly two occurrences (one labelled,
 * one bare; or two labelled in some eNewick forms). */
typedef struct { NwkNode *parent; NwkNode *node; } LabOcc;

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
            char *pi = anno_get(occ[i].node->annotation, "phi");
            char *pb = anno_get(occ[best].node->annotation, "phi");
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
    /* Restrict to labels that appear more than once: those are hybrid nodes. */
    const char **labels = NULL; int n = 0;
    for (int i = 0; i < total; i++)
        if (label_count(root, all[i]) >= 2) {
            labels = xrealloc(labels, (size_t)(n + 1) * sizeof(*labels));
            labels[n++] = all[i];
        }
    free(all);

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
        /* phi: prefer the ref's annotation; fall back to defn's */
        char *phi_s = anno_get(refn->annotation, "phi");
        if (!phi_s) phi_s = anno_get(defn->annotation, "phi");
        char *def_tau = anno_get(defn->annotation, "tau-parent");
        char *ref_tau = anno_get(refn->annotation, "tau-parent");

        IntroEvent ev; memset(&ev, 0, sizeof ev);
        ev.phi   = phi_s ? atof(phi_s) : 0.5;
        ev.phi2  = -1.0;
        ev.src   = (ref_tau && strcasecmp(ref_tau, "no") == 0) ? TAU_NODE : TAU_BRANCH;
        ev.dst   = (def_tau && strcasecmp(def_tau, "no") == 0) ? TAU_NODE : TAU_BRANCH;
        ev.label = xstrdup(labels[i]);
        /* recipient: the underlying lineage carried by the labelled hybrid
         * occurrence -- the first child of defn that isn't itself a bare
         * reference to another hybrid (which is what makes Model D's defn
         * have two children rather than one). */
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
        if (!recip_n) recip_n = defn;          /* defensive */
        ev.recip = node_name(recip_n);
        /* donor: the non-bare-ref sibling of the bare reference under refp */
        NwkNode *donor_n = NULL;
        for (int k = 0; k < refp->n_children; k++) {
            if (refp->children[k] == refn) continue;
            donor_n = refp->children[k]; break;
        }
        ev.donor = donor_n ? node_name(donor_n) : xstrdup("?");

        if (im->intro.count == im->intro.cap) {
            im->intro.cap = im->intro.cap ? im->intro.cap * 2 : 4;
            im->intro.items = xrealloc(im->intro.items,
                                       (size_t)im->intro.cap * sizeof(IntroEvent));
        }
        im->intro.items[im->intro.count++] = ev;

        /* Rewrite the tree to its base form: keep the definition (strip the
         * hybrid wrapper); remove the bare reference and its enclosing pair. */
        if (refp && defp) {
            /* The defn is wrapped as (real_subtree)label. Replace defn with its
             * single real child, dropping the hybrid wrap. */
            if (defn->n_children == 1) {
                NwkNode *child = defn->children[0];
                /* splice child in place of defn under defp */
                for (int k = 0; k < defp->n_children; k++) {
                    if (defp->children[k] == defn) {
                        defp->children[k] = child;
                        defn->n_children = 0; free(defn->children); defn->children = NULL;
                        nwk_free(defn);
                        break;
                    }
                }
            } else {
                strip_annotation(defn);            /* multi-child defn: just strip annotation */
            }
            /* refp pattern: (donor_subtree, bare_ref) -- drop the bare ref so refp
             * keeps only the donor subtree. If refp ends up with one child, splice
             * it through (suppress the now-unary node). */
            drop_child(refp, refn);
        }
        free(phi_s); free(def_tau); free(ref_tau);
    }
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
