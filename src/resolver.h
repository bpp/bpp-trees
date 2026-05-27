/* resolver.h — identify leaves, compute clade labels, order joins by
 * dependency, build the tree, and find the root.
 *
 * Leaf identification follows the spec's Rule 1: a token is a leaf iff it is
 * never produced as the label (explicit or implicit) of any join. Joins may
 * be given in any order; dependencies are resolved by an iterative fixpoint
 * (equivalent to a topological sort). Implicit labels always contain '_', so
 * a single-token operand can never be one — this disambiguates leaves from
 * clade references without knowing the full leaf set up front.
 */
#ifndef BPP_TREE_RESOLVER_H
#define BPP_TREE_RESOLVER_H

#include "parser.h"
#include "tree.h"
#include "diag.h"

typedef struct {
    int            n_joins;
    TreeNode     **join_node;   /* [n_joins] built node, or NULL if unresolved */
    char         **join_name;   /* [n_joins] canonical name (explicit or implicit) */
    unsigned char *resolved;    /* [n_joins] */
    unsigned char *referenced;  /* [n_joins] product used as an operand elsewhere */

    TreeNode     **leaves;      /* unique leaf nodes, alphabetical (owned) */
    int            n_leaves;

    TreeNode     **auto_nodes;  /* implicitly-created pair clades (owned) */
    int            n_auto;

    TreeNode     **move_nodes;  /* nodes created by --move grafts (owned) */
    int            n_move;

    TreeNode      *synth_root;  /* root synthesised by joining 2 roots (owned) */

    TreeNode      *root;        /* the tree root, or NULL if incomplete */
    int           *roots;       /* indices of unreferenced (root) joins */
    int            n_roots;
} Resolution;

/* Resolve the joins, auto-resolving forced choices (pair clades, and a final
 * join of exactly two roots). AMBIGUOUS_CLADE / DISCONNECTED / CYCLE and any
 * joined-twice diagnostics are appended to errs. Always returns a Resolution
 * (free with resolution_free); the caller runs further validation before
 * producing output. */
Resolution *resolve_tree(const JoinList *joins, DiagList *errs);
void        resolution_free(Resolution *r);

/* Rotate (reverse the children of) each clade named in `spec`, a ','/';'
 * separated list of clade identifiers (a leaf-set label like 'A_B', or an
 * explicit label). Tip entries are ignored (a note is added to warns); an
 * identifier matching no clade is an error in errs. */
void        resolution_rotate(Resolution *r, const char *spec,
                              DiagList *errs, DiagList *warns);

/* Apply subtree prune-and-regraft moves. `spec` is a ','/';' separated list
 * of 'SOURCE->TARGET' moves, applied in order: SOURCE is detached and
 * regrafted as the sister of TARGET. Invalid moves are errors in errs;
 * a move that is already in place is a note in warns. */
void        resolution_move(Resolution *r, const char *spec,
                            DiagList *errs, DiagList *warns);

/* Graft new tips onto the tree. `spec` is a ','/';' separated list of
 * 'NEW->TARGET' grafts: a new tip NEW is added as the sister of TARGET.
 * Errors (NEW already present, TARGET missing, bad name) go to errs. */
void        resolution_graft(Resolution *r, const char *spec,
                             DiagList *errs, DiagList *warns);

/* Remove tips/subtrees. `spec` is a ','/';' separated list of clade or tip
 * names; each is pruned and its now-unary parent is suppressed. Removing the
 * root, or an unknown name, is an error in errs. */
void        resolution_prune(Resolution *r, const char *spec,
                             DiagList *errs, DiagList *warns);

#endif /* BPP_TREE_RESOLVER_H */
