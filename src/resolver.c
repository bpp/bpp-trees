#include "resolver.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* --- small helpers ------------------------------------------------------ */

static int cmp_leaf_by_name(const void *a, const void *b)
{
    const TreeNode *na = *(const TreeNode *const *)a;
    const TreeNode *nb = *(const TreeNode *const *)b;
    return strcmp(na->name, nb->name);
}

static int count_parts(const char *op)
{
    int n = 1;
    for (const char *p = op; *p; p++) if (*p == '_') n++;
    return n;
}

/* Canonical (sorted) form of a '_'-separated leaf-set label. */
static char *canon_label(const char *op)
{
    int n = count_parts(op);
    char **parts = xmalloc((size_t)n * sizeof(char *));
    int idx = 0;
    const char *start = op;
    for (const char *p = op;; p++) {
        if (*p == '_' || *p == '\0') {
            parts[idx++] = xstrndup(start, (size_t)(p - start));
            start = p + 1;
            if (*p == '\0') break;
        }
    }
    qsort(parts, (size_t)n, sizeof(char *), cmp_str);

    size_t len = 0;
    for (int i = 0; i < n; i++) len += strlen(parts[i]) + 1;
    char *out = xmalloc(len ? len : 1);
    char *w = out;
    for (int i = 0; i < n; i++) {
        if (i) *w++ = '_';
        size_t l = strlen(parts[i]);
        memcpy(w, parts[i], l);
        w += l;
    }
    *w = '\0';
    for (int i = 0; i < n; i++) free(parts[i]);
    free(parts);
    return out;
}

/* --- leaf interning ----------------------------------------------------- */

static TreeNode *get_leaf(Resolution *r, int *cap, const char *name)
{
    for (int i = 0; i < r->n_leaves; i++)
        if (strcmp(r->leaves[i]->name, name) == 0)
            return r->leaves[i];

    if (r->n_leaves == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        r->leaves = xrealloc(r->leaves, (size_t)*cap * sizeof(TreeNode *));
    }
    TreeNode *leaf = treenode_leaf(name);
    r->leaves[r->n_leaves++] = leaf;
    return leaf;
}

/* --- clade lookup ------------------------------------------------------- */

/* The clade a token refers to, or NULL. A '_'-token is a leaf-set reference
 * (matched by canonical leaf set against built clades, including auto-created
 * pairs); a plain token matches a resolved join's explicit label. *out_ji is
 * set to the producing join index, or -1 (tip / auto-created clade). */
static TreeNode *find_clade(const Resolution *r, const char *op, int *out_ji)
{
    *out_ji = -1;
    if (strchr(op, '_')) {
        char *canon = canon_label(op);
        for (int j = 0; j < r->n_joins; j++)
            if (r->resolved[j] &&
                strcmp(r->join_node[j]->implicit_label, canon) == 0) {
                *out_ji = j; free(canon); return r->join_node[j];
            }
        for (int a = 0; a < r->n_auto; a++)
            if (strcmp(r->auto_nodes[a]->implicit_label, canon) == 0) {
                free(canon); return r->auto_nodes[a];
            }
        free(canon);
        return NULL;
    }
    for (int j = 0; j < r->n_joins; j++)
        if (r->resolved[j] && strcmp(r->join_name[j], op) == 0) {
            *out_ji = j; return r->join_node[j];
        }
    return NULL;
}

static int find_explicit(const JoinList *joins, const char *op)
{
    for (int j = 0; j < joins->count; j++)
        if (joins->items[j].label && strcmp(joins->items[j].label, op) == 0)
            return j;
    return -1;
}

/* Create a 2-leaf clade for a '_'-token that no join builds (forced shape). */
static TreeNode *make_auto_pair(Resolution *r, int *leaf_cap, const char *op, int line)
{
    const char *us = strchr(op, '_');
    char *a = xstrndup(op, (size_t)(us - op));
    char *b = xstrdup(us + 1);
    const char *first = a, *second = b;
    if (strcmp(a, b) > 0) { first = b; second = a; }

    TreeNode *node = treenode_internal(line);
    TreeNode *la = get_leaf(r, leaf_cap, first);
    treenode_add_child(node, la); treenode_add_ref(la, line);
    TreeNode *lb = get_leaf(r, leaf_cap, second);
    treenode_add_child(node, lb); treenode_add_ref(lb, line);
    treenode_finalize(node);

    r->auto_nodes = xrealloc(r->auto_nodes, (size_t)(r->n_auto + 1) * sizeof(TreeNode *));
    r->auto_nodes[r->n_auto++] = node;
    free(a); free(b);
    return node;
}

/* --- resolution --------------------------------------------------------- */

static void resolve_join(Resolution *r, const JoinList *joins, int j, int *leaf_cap)
{
    const JoinStmt *st = &joins->items[j];
    TreeNode *node = treenode_internal(st->line_no);
    for (int o = 0; o < st->n_operands; o++) {
        const char *op = st->operands[o];
        int ji;
        TreeNode *child = find_clade(r, op, &ji);
        if (!child) child = get_leaf(r, leaf_cap, op);  /* plain tip */
        treenode_add_child(node, child);
        treenode_add_ref(child, st->line_no);
        if (ji >= 0) r->referenced[ji] = 1;
    }
    treenode_finalize(node);
    r->join_node[j] = node;
    r->join_name[j] = st->label ? xstrdup(st->label)
                                 : xstrdup(node->implicit_label);
    r->resolved[j] = 1;
}

/* Resolve join j if every operand is a tip, a built clade, or an explicit
 * label already resolved. A '_'-clade not yet built makes us wait (it may be
 * built or auto-created later). Returns 1 if resolved. */
static int try_resolve(Resolution *r, const JoinList *joins, int j, int *leaf_cap)
{
    const JoinStmt *st = &joins->items[j];
    for (int o = 0; o < st->n_operands; o++) {
        const char *op = st->operands[o];
        int ji;
        if (find_clade(r, op, &ji)) continue;          /* built clade */
        if (!strchr(op, '_')) {
            if (find_explicit(joins, op) >= 0) return 0; /* wait for label's join */
            continue;                                    /* plain tip */
        }
        return 0;                                        /* '_'-clade not built yet */
    }
    resolve_join(r, joins, j, leaf_cap);
    return 1;
}

/* comma/"and" line list, e.g. "1, 2 and 3" */
static char *lines_str(const int *lines, int n)
{
    char *s = NULL; size_t len = 0;
    for (int i = 0; i < n; i++) {
        char *p = xasprintf("%d", lines[i]);
        const char *sep = i == 0 ? "" : (i == n - 1 ? " and " : ", ");
        s = xrealloc(s, len + strlen(sep) + strlen(p) + 1);
        memcpy(s + len, sep, strlen(sep)); len += strlen(sep);
        memcpy(s + len, p, strlen(p) + 1); len += strlen(p);
        free(p);
    }
    return s ? s : xstrdup("");
}

Resolution *resolve_tree(const JoinList *joins, DiagList *errs)
{
    Resolution *r = xcalloc(1, sizeof(*r));
    r->n_joins = joins->count;
    if (r->n_joins) {
        r->join_node  = xcalloc((size_t)r->n_joins, sizeof(TreeNode *));
        r->join_name  = xcalloc((size_t)r->n_joins, sizeof(char *));
        r->resolved   = xcalloc((size_t)r->n_joins, sizeof(unsigned char));
        r->referenced = xcalloc((size_t)r->n_joins, sizeof(unsigned char));
    }

    int leaf_cap = 0;
    int remaining = r->n_joins;

    for (;;) {
        int progressed = 0;
        for (int j = 0; j < r->n_joins; j++) {
            if (r->resolved[j]) continue;
            if (try_resolve(r, joins, j, &leaf_cap)) { progressed = 1; remaining--; }
        }
        if (progressed) { if (remaining == 0) break; else continue; }
        if (remaining == 0) break;

        /* Stuck: auto-create any blocked 2-member clade (forced shape). */
        int created = 0;
        for (int j = 0; j < r->n_joins; j++) {
            if (r->resolved[j]) continue;
            const JoinStmt *st = &joins->items[j];
            for (int o = 0; o < st->n_operands; o++) {
                const char *op = st->operands[o];
                if (!strchr(op, '_')) continue;
                int ji;
                if (find_clade(r, op, &ji)) continue;
                if (count_parts(op) == 2) { make_auto_pair(r, &leaf_cap, op, st->line_no); created = 1; }
            }
        }
        if (!created) break;  /* remaining are 3+-member refs or a cycle */
    }

    if (r->n_leaves > 1)
        qsort(r->leaves, (size_t)r->n_leaves, sizeof(TreeNode *), cmp_leaf_by_name);

    if (remaining > 0) {
        /* A 3+-member reference no join builds is ambiguous (root cause);
         * only when none of those exist are the leftovers a real cycle. */
        int ambiguous = 0;
        for (int j = 0; j < r->n_joins; j++) {
            if (r->resolved[j]) continue;
            const JoinStmt *st = &joins->items[j];
            for (int o = 0; o < st->n_operands; o++) {
                const char *op = st->operands[o];
                int ji;
                if (find_clade(r, op, &ji)) continue;
                if (!strchr(op, '_') || count_parts(op) < 3) continue;
                int seen = 0;  /* report each distinct reference once */
                for (int k = 0; k < j && !seen; k++) {
                    if (r->resolved[k]) continue;
                    for (int oo = 0; oo < joins->items[k].n_operands; oo++)
                        if (strcmp(joins->items[k].operands[oo], op) == 0) { seen = 1; break; }
                }
                if (seen) continue;
                /* suggest a concrete starting pair from the first two taxa */
                char *canon = canon_label(op);
                char *us = strchr(canon, '_');
                char *t0 = xstrndup(canon, (size_t)(us - canon));
                char *rest = us + 1;
                char *us2 = strchr(rest, '_');
                char *t1 = us2 ? xstrndup(rest, (size_t)(us2 - rest)) : xstrdup(rest);
                Diagnostic *d = diag_add(errs, DIAG_AMBIGUOUS_CLADE, st->line_no,
                    "'%s' (%d taxa) is referenced but not built; its branching "
                    "order can't be inferred from the name.", op, count_parts(op));
                diag_set_hint(d, "build it with binary joins — group two taxa, then "
                                 "add the rest one at a time (e.g. start '%s+%s').", t0, t1);
                free(canon); free(t0); free(t1);
                ambiguous++;
            }
        }
        if (!ambiguous) {
            char *names = NULL; size_t len = 0;
            for (int j = 0; j < r->n_joins; j++) {
                if (r->resolved[j]) continue;
                const char *nm = joins->items[j].label;
                char *piece = nm ? xasprintf("%s", nm)
                                 : xasprintf("join on line %d", joins->items[j].line_no);
                names = xrealloc(names, len + strlen(piece) + 3);
                if (len) { memcpy(names + len, ", ", 2); len += 2; }
                memcpy(names + len, piece, strlen(piece) + 1); len += strlen(piece);
                free(piece);
            }
            Diagnostic *d = diag_add(errs, DIAG_CYCLE, -1,
                "dependency cycle detected involving: %s.", names ? names : "(unknown)");
            diag_set_hint(d, "two joins reference each other's labels. Break the "
                             "cycle so the joins form a tree.");
            free(names);
        }
        return r;
    }

    /* A taxon or clade referenced by more than one join has more than one
     * parent (a DAG, not a tree) — catches double joins under any names. */
    int joined_twice = 0;
    for (int i = 0; i < r->n_leaves; i++) {
        TreeNode *nd = r->leaves[i];
        if (nd->n_refs >= 2) {
            char *ls = lines_str(nd->ref_lines, nd->n_refs);
            Diagnostic *d = diag_add(errs, DIAG_TAXON_JOINED_TWICE, -1,
                "taxon '%s' appears in multiple joins (lines %s).", nd->name, ls);
            diag_set_hint(d, "each taxon may be joined only once.");
            free(ls); joined_twice = 1;
        }
    }
    for (int j = 0; j < r->n_joins; j++) {
        TreeNode *nd = r->join_node[j];
        if (nd && nd->n_refs >= 2) {
            char *ls = lines_str(nd->ref_lines, nd->n_refs);
            Diagnostic *d = diag_add(errs, DIAG_CLADE_JOINED_TWICE, -1,
                "clade '%s' appears in multiple joins (lines %s).", r->join_name[j], ls);
            diag_set_hint(d, "each clade may be joined only once.");
            free(ls); joined_twice = 1;
        }
    }
    for (int a = 0; a < r->n_auto; a++) {
        TreeNode *nd = r->auto_nodes[a];
        if (nd->n_refs >= 2) {
            char *ls = lines_str(nd->ref_lines, nd->n_refs);
            Diagnostic *d = diag_add(errs, DIAG_CLADE_JOINED_TWICE, -1,
                "clade '%s' appears in multiple joins (lines %s).", nd->implicit_label, ls);
            diag_set_hint(d, "each clade may be joined only once.");
            free(ls); joined_twice = 1;
        }
    }
    if (joined_twice) return r;  /* DAG, not a tree */

    /* roots = resolved joins whose product is never used as an operand */
    for (int j = 0; j < r->n_joins; j++)
        if (r->resolved[j] && !r->referenced[j]) {
            r->roots = xrealloc(r->roots, (size_t)(r->n_roots + 1) * sizeof(int));
            r->roots[r->n_roots++] = j;
        }

    if (r->n_roots == 1) {
        r->root = r->join_node[r->roots[0]];
    } else if (r->n_roots == 2) {
        /* exactly one rooted tree contains both halves — join them */
        TreeNode *root = treenode_internal(-1);
        treenode_add_child(root, r->join_node[r->roots[0]]);
        treenode_add_child(root, r->join_node[r->roots[1]]);
        treenode_finalize(root);
        r->synth_root = root;
        r->root = root;
    } else if (r->n_roots > 2) {
        char *list = NULL; size_t len = 0;
        for (int i = 0; i < r->n_roots; i++) {
            int j = r->roots[i];
            char *piece = xasprintf("\n  '%s'  (line %d)", r->join_name[j],
                                    r->join_node[j]->join_line);
            list = xrealloc(list, len + strlen(piece) + 1);
            memcpy(list + len, piece, strlen(piece) + 1); len += strlen(piece);
            free(piece);
        }
        Diagnostic *d = diag_add(errs, DIAG_DISCONNECTED, -1,
            "tree is incomplete: %d separate clades remain (need to reach 2, "
            "which are then joined automatically):%s", r->n_roots, list);
        char *suggest = NULL; size_t sl = 0;
        for (int i = 0; i < r->n_roots; i++) {
            const char *nm = r->join_name[r->roots[i]];
            suggest = xrealloc(suggest, sl + strlen(nm) + 2);
            if (i) suggest[sl++] = '+';
            memcpy(suggest + sl, nm, strlen(nm) + 1); sl += strlen(nm);
        }
        diag_set_hint(d, "join some of these, e.g.: %s — or any pair, until two remain.",
                      suggest);
        free(list); free(suggest);
    }

    return r;
}

/* Internal node matching id (leaf-set label or explicit label), or NULL. */
static TreeNode *find_internal(const Resolution *r, const char *id)
{
    if (strchr(id, '_')) {
        char *canon = canon_label(id);
        TreeNode *res = NULL;
        for (int j = 0; j < r->n_joins && !res; j++)
            if (r->resolved[j] && strcmp(r->join_node[j]->implicit_label, canon) == 0)
                res = r->join_node[j];
        for (int a = 0; a < r->n_auto && !res; a++)
            if (strcmp(r->auto_nodes[a]->implicit_label, canon) == 0)
                res = r->auto_nodes[a];
        if (!res && r->synth_root &&
            strcmp(r->synth_root->implicit_label, canon) == 0)
            res = r->synth_root;
        free(canon);
        return res;
    }
    for (int j = 0; j < r->n_joins; j++)   /* explicit label */
        if (r->resolved[j] && strcmp(r->join_name[j], id) == 0)
            return r->join_node[j];
    return NULL;
}

static int is_tip_name(const Resolution *r, const char *id)
{
    for (int i = 0; i < r->n_leaves; i++)
        if (strcmp(r->leaves[i]->name, id) == 0) return 1;
    return 0;
}

void resolution_rotate(Resolution *r, const char *spec, DiagList *errs, DiagList *warns)
{
    if (!r->root) return;
    const char *p = spec;
    while (*p) {
        size_t len = strcspn(p, ",;");
        const char *s = p, *e = p + len;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > s) {
            char *tok = xstrndup(s, (size_t)(e - s));
            TreeNode *node = find_internal(r, tok);
            if (node) {
                treenode_rotate(node);
            } else if (!strchr(tok, '_') && is_tip_name(r, tok)) {
                diag_add(warns, DIAG_ROTATE_IGNORED_TIP, -1,
                    "rotate: '%s' is a tip and has no children to rotate; ignored.",
                    tok);
            } else {
                Diagnostic *d = diag_add(errs, DIAG_ROTATE_UNKNOWN, -1,
                    "rotate: '%s' is not a clade in the tree.", tok);
                diag_set_hint(d, "name a clade by its members (e.g. 'A_B') or by "
                                 "its explicit label.");
            }
            free(tok);
        }
        p += len;
        if (*p) p++;
    }
}

void resolution_free(Resolution *r)
{
    if (!r) return;
    for (int j = 0; j < r->n_joins; j++) {
        treenode_free(r->join_node[j]);
        free(r->join_name[j]);
    }
    free(r->join_node);
    free(r->join_name);
    free(r->resolved);
    free(r->referenced);
    for (int i = 0; i < r->n_leaves; i++) treenode_free(r->leaves[i]);
    free(r->leaves);
    for (int a = 0; a < r->n_auto; a++) treenode_free(r->auto_nodes[a]);
    free(r->auto_nodes);
    treenode_free(r->synth_root);
    free(r->roots);
    free(r);
}
