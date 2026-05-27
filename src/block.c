#include "block.h"
#include "util.h"

#include <stdlib.h>

char *species_block(TreeNode **taxa, int n_taxa, const char *newick,
                    const Imap *imap, int *filled, int **counts_out)
{
    int *counts = xmalloc((size_t)(n_taxa ? n_taxa : 1) * sizeof(int));
    int all = (imap != NULL);
    for (int i = 0; i < n_taxa; i++) {
        counts[i] = imap ? imap_count_for(imap, taxa[i]->name) : -1;
        if (counts[i] < 0) all = 0;
    }
    *filled = all;

    /* line 1: "species&tree = N  n1  n2 ..." */
    char *out = xasprintf("species&tree = %d", n_taxa);
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = xasprintf("%s  %s", out, taxa[i]->name);
        free(out); out = tmp;
    }
    /* line 2: counts */
    { char *tmp = xasprintf("%s\n ", out); free(out); out = tmp; }
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = (counts[i] >= 0) ? xasprintf("%s  %d", out, counts[i])
                                     : xasprintf("%s  ?", out);
        free(out); out = tmp;
    }
    /* line 3: newick */
    { char *tmp = xasprintf("%s\n  %s", out, newick); free(out); out = tmp; }

    *counts_out = counts;
    return out;
}
