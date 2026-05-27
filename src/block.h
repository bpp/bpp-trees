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

/* Return a copy of BPP control-file `text` with its `species&tree` statement
 * replaced by `new_block`. The statement spans `species&tree = N`, the N
 * species names, the N counts, and the Newick (which may wrap across lines and
 * contain spaces). On failure returns NULL and points *errmsg at a static
 * message. Caller frees the result. */
char *control_replace_block(const char *text, const char *new_block,
                            const char **errmsg);

#endif /* BPP_TREE_BLOCK_H */
