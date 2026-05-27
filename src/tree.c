#include "tree.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

TreeNode *treenode_leaf(const char *name)
{
    TreeNode *n = xcalloc(1, sizeof(*n));
    n->name = xstrdup(name);
    n->implicit_label = xstrdup(name);
    n->is_leaf = 1;
    n->join_line = -1;
    n->leaf_names = xmalloc(sizeof(char *));
    n->leaf_names[0] = xstrdup(name);
    n->n_leaf_names = 1;
    return n;
}

TreeNode *treenode_internal(int join_line)
{
    TreeNode *n = xcalloc(1, sizeof(*n));
    n->is_leaf = 0;
    n->join_line = join_line;
    return n;
}

void treenode_add_child(TreeNode *parent, TreeNode *child)
{
    parent->children = xrealloc(parent->children,
                                (size_t)(parent->n_children + 1) * sizeof(TreeNode *));
    parent->children[parent->n_children++] = child;
    child->parent = parent;
}

void treenode_add_ref(TreeNode *node, int line)
{
    if (node->n_refs == node->ref_cap) {
        node->ref_cap = node->ref_cap ? node->ref_cap * 2 : 4;
        node->ref_lines = xrealloc(node->ref_lines,
                                   (size_t)node->ref_cap * sizeof(int));
    }
    node->ref_lines[node->n_refs++] = line;
}

void treenode_finalize(TreeNode *node)
{
    if (node->is_leaf) return;

    /* free any previous computation (so this is safe to call as a recompute) */
    for (int i = 0; i < node->n_leaf_names; i++) free(node->leaf_names[i]);
    free(node->leaf_names);
    node->leaf_names = NULL;
    node->n_leaf_names = 0;
    free(node->implicit_label);
    node->implicit_label = NULL;

    int total = 0;
    for (int i = 0; i < node->n_children; i++)
        total += node->children[i]->n_leaf_names;

    char **names = xmalloc((size_t)(total ? total : 1) * sizeof(char *));
    int n = 0;
    for (int i = 0; i < node->n_children; i++) {
        TreeNode *c = node->children[i];
        for (int k = 0; k < c->n_leaf_names; k++)
            names[n++] = xstrdup(c->leaf_names[k]);
    }
    qsort(names, (size_t)n, sizeof(char *), cmp_str);

    /* drop duplicates (defensive; a valid tree has none) */
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && strcmp(names[i], names[i - 1]) == 0) {
            free(names[i]);
            continue;
        }
        names[m++] = names[i];
    }

    node->leaf_names = names;
    node->n_leaf_names = m;

    /* implicit label = sorted leaf names joined by '_' */
    size_t len = 0;
    for (int i = 0; i < m; i++) len += strlen(names[i]) + 1;
    char *lbl = xmalloc(len ? len : 1);
    lbl[0] = '\0';
    char *w = lbl;
    for (int i = 0; i < m; i++) {
        if (i) *w++ = '_';
        size_t l = strlen(names[i]);
        memcpy(w, names[i], l);
        w += l;
    }
    *w = '\0';
    node->implicit_label = lbl;
    if (!node->name) node->name = xstrdup(lbl);
}

void treenode_recompute(TreeNode *node)
{
    if (!node || node->is_leaf) return;
    for (int i = 0; i < node->n_children; i++)
        treenode_recompute(node->children[i]);
    treenode_finalize(node);
}

void treenode_free(TreeNode *node)
{
    if (!node) return;
    /* Shallow: nodes are owned flatly by the resolver; do not recurse into
     * children (a shared leaf would be double-freed). */
    free(node->name);
    free(node->implicit_label);
    free(node->explicit_label);
    free(node->children);
    for (int i = 0; i < node->n_leaf_names; i++) free(node->leaf_names[i]);
    free(node->leaf_names);
    free(node->ref_lines);
    free(node);
}

static char *str_append(char *buf, size_t *len, size_t *cap, const char *s)
{
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        while (*len + sl + 1 > *cap) *cap = *cap ? *cap * 2 : 64;
        buf = xrealloc(buf, *cap);
    }
    memcpy(buf + *len, s, sl + 1);
    *len += sl;
    return buf;
}

static char *newick_rec(const TreeNode *node, char *buf, size_t *len, size_t *cap)
{
    if (node->is_leaf)
        return str_append(buf, len, cap, node->name);

    buf = str_append(buf, len, cap, "(");
    for (int i = 0; i < node->n_children; i++) {
        if (i) buf = str_append(buf, len, cap, ",");
        buf = newick_rec(node->children[i], buf, len, cap);
    }
    buf = str_append(buf, len, cap, ")");
    return buf;
}

char *treenode_to_newick(const TreeNode *node)
{
    size_t len = 0, cap = 0;
    char *buf = xmalloc(1);
    buf[0] = '\0';
    return newick_rec(node, buf, &len, &cap);
}

void treenode_rotate(TreeNode *node)
{
    if (!node || node->is_leaf) return;
    for (int i = 0, j = node->n_children - 1; i < j; i++, j--) {
        TreeNode *t = node->children[i];
        node->children[i] = node->children[j];
        node->children[j] = t;
    }
}

int treenode_count_internal(const TreeNode *node)
{
    if (!node || node->is_leaf) return 0;
    int c = 1;
    for (int i = 0; i < node->n_children; i++)
        c += treenode_count_internal(node->children[i]);
    return c;
}

void treenode_collect_leaves(const TreeNode *node, TreeNode ***out, int *n, int *cap)
{
    if (node->is_leaf) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *out = xrealloc(*out, (size_t)*cap * sizeof(TreeNode *));
        }
        (*out)[(*n)++] = (TreeNode *)node;
        return;
    }
    for (int i = 0; i < node->n_children; i++)
        treenode_collect_leaves(node->children[i], out, n, cap);
}
