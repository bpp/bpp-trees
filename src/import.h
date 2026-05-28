/* import.h -- read a tree from a file: a Newick string, a species&tree block,
 * a BPP control file, or a stand-alone migration block. Auto-detects the
 * format from the file contents. Recovers MSC-I introgression events from
 * extended-Newick annotations (&phi=, &tau-parent=) and migration bands from
 * a 'migration = N' block.
 *
 * The output is a sequence of join formula strings and rotate specs, plus an
 * optional MigList or IntroList -- exactly the shape NamedTree wants. The
 * caller drives a workspace tree from them.
 */
#ifndef BPP_TREE_IMPORT_H
#define BPP_TREE_IMPORT_H

#include "diag.h"
#include "migrate.h"
#include "introgress.h"

typedef struct {
    char    **joins;      /* join formula strings (one per internal node), owned */
    char    **rotates;    /* rotate specs to recover the original child order */
    int       n_joins, n_rotates;
    char     *newick_in;  /* the Newick/eNewick that was read (owned) */
    int       had_msci;   /* 1 if the input contained &phi=/&tau-parent= */
    MigList   mig;        /* migration bands (empty if none) */
    IntroList intro;      /* introgression events (empty if none) */
} Import;

void import_init(Import *im);
void import_free(Import *im);

/* Read FILE and populate `im`. Returns 1 on success; on parse failure
 * appends diagnostics to errs and returns 0. */
int  import_read(const char *path, Import *im, DiagList *errs);

#endif /* BPP_TREE_IMPORT_H */
