/* block.h — build the BPP species&tree control-file block. */
#ifndef BPP_TREE_BLOCK_H
#define BPP_TREE_BLOCK_H

#include "tree.h"
#include "imap.h"

/* Build the species&tree block for `taxa` (in Newick left-to-right order) and
 * `newick`. If `imap` is non-NULL, per-species individual counts are filled
 * from it; otherwise '?' placeholders are used. Sets *filled to 1 if every
 * count was supplied, and returns the per-taxon counts via *counts_out (-1
 * where unknown; caller frees). Returns a malloc'd block string. */
char *species_block(TreeNode **taxa, int n_taxa, const char *newick,
                    const Imap *imap, int *filled, int **counts_out);

#endif /* BPP_TREE_BLOCK_H */
