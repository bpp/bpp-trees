/* imap.h — read a BPP Imap file (sample -> species) and count individuals
 * per species, used to fill the species&tree block's count line. */
#ifndef BPP_TREE_IMAP_H
#define BPP_TREE_IMAP_H

#include "diag.h"

typedef struct {
    char *species;   /* owned */
    int   count;
} ImapCount;

typedef struct {
    ImapCount *items;
    int        count;
    int        cap;
} Imap;

/* Read `path`. On a system error (open/read), returns NULL and appends a
 * diagnostic to errs. An empty/usable file returns a valid (possibly empty)
 * Imap. */
Imap *imap_read(const char *path, DiagList *errs);
void  imap_free(Imap *m);

/* Individuals mapped to `species`, or -1 if the species is absent. */
int   imap_count_for(const Imap *m, const char *species);

#endif /* BPP_TREE_IMAP_H */
