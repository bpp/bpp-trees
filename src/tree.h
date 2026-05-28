/* tree.h — resolved tree nodes and Newick serialisation. */
#ifndef BPP_TREE_TREE_H
#define BPP_TREE_TREE_H

#include <stdio.h>

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

    int             *mig;            /* migration marks: +k source / -k dest of band k */
    int              n_mig;

    char           **intro_mark;     /* introgression display markers (owned) */
    int             *intro_key;      /* colour key per marker (event number) */
    int              n_intro;

    int              show_label;     /* emit this internal node's label in Newick */
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

/* Record a migration mark on a node: +k if it is the source of band k, -k if
 * the destination (k >= 1). Shown after the label by treenode_display. */
void      treenode_add_mig(TreeNode *node, int signed_band);

/* Record an introgression display marker (e.g. "H1\xe2\x87\x9d" on the donor,
 * "\xe2\x87\x9dH1(.30)" on the recipient) coloured by event number `key`.
 * The string is copied. Shown after the label by treenode_display. */
void      treenode_add_intro(TreeNode *node, const char *mark, int key);

/* The node's BPP population name: a tip's name, or a clade's explicit label
 * (falling back to its implicit leaf-set label). */
const char *treenode_bpp_name(const TreeNode *node);

/* ANSI colour escape for migration band k (k >= 1), and the reset code. */
const char *treenode_mig_color(int k);
#define TREENODE_MIG_RESET "\x1b[0m"

/* Whether to colourise output: not ASCII mode, fp is a TTY, NO_COLOR unset. */
int       treenode_use_color(int ascii, FILE *fp);

/* Print the subtree as an indented, root-at-left branching diagram. Each line
 * is prefixed with `lead`. Tips show their name; internal nodes show their
 * explicit label, or their implicit leaf-set label if unlabelled. `ascii`
 * selects plain ASCII connectors instead of Unicode box-drawing characters. */
void      treenode_display(const TreeNode *root, FILE *fp, int ascii,
                           const char *lead);

#endif /* BPP_TREE_TREE_H */
