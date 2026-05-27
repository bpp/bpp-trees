/* tree.h — resolved tree nodes and Newick serialisation. */
#ifndef BPP_TREE_TREE_H
#define BPP_TREE_TREE_H

typedef struct TreeNode {
    char            *name;           /* leaf name, or clade label (owned) */
    char            *implicit_label; /* sorted leaf names joined by '_' (owned) */
    char            *explicit_label; /* explicit clade label, or NULL (owned) */
    struct TreeNode **children;      /* owned array; nodes owned by tree */
    int              n_children;
    struct TreeNode *parent;         /* back-reference, non-owning; NULL at root */
    int              is_leaf;
    int              join_line;      /* source line that produced this node, or -1 */

    char           **leaf_names;     /* sorted unique descendant leaf names (owned) */
    int              n_leaf_names;

    int             *ref_lines;      /* source lines of joins referencing this node */
    int              n_refs;
    int              ref_cap;
} TreeNode;

TreeNode *treenode_leaf(const char *name);
TreeNode *treenode_internal(int join_line);
void      treenode_add_child(TreeNode *parent, TreeNode *child);

/* Record that the join on `line` references this node as an operand. Used to
 * detect a taxon or clade joined more than once (more than one parent). */
void      treenode_add_ref(TreeNode *node, int line);

/* Finalise an internal node: collect/sort descendant leaf names and compute
 * implicit_label. Safe to call again to recompute (frees prior results). */
void      treenode_finalize(TreeNode *node);

/* Recompute leaf sets and implicit labels for the whole subtree, bottom-up
 * (after a structural edit such as a move). */
void      treenode_recompute(TreeNode *node);

/* Recursively free a node and its children (children are owned). Leaf nodes
 * shared across the tree are owned once; free the root only. */
void      treenode_free(TreeNode *node);

/* Append the Newick representation of the subtree (no trailing ';') to a
 * malloc'd buffer. Returns a newly allocated string (caller frees). */
char     *treenode_to_newick(const TreeNode *node);

/* Collect leaves in left-to-right Newick order into *out (grows the array). */
void      treenode_collect_leaves(const TreeNode *node,
                                  TreeNode ***out, int *n, int *cap);

/* Number of internal (non-leaf) nodes in the subtree. */
int       treenode_count_internal(const TreeNode *node);

/* Reverse the order of a node's children (a no-op for leaves). Changes the
 * Newick string but not the topology. */
void      treenode_rotate(TreeNode *node);

#endif /* BPP_TREE_TREE_H */
