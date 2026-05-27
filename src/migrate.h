/* migrate.h — MSC-M migration bands attached to a tree.
 *
 * A band is a directional pair source->target between two branches (tips or
 * clades). A band is valid only between non-nested, contemporaneous branches:
 * source and target must differ and neither may be the other's ancestor.
 */
#ifndef BPP_TREE_MIGRATE_H
#define BPP_TREE_MIGRATE_H

#include "resolver.h"
#include "diag.h"

#include <stdio.h>

typedef struct { char *src, *dst; } MigBand;

typedef struct {
    MigBand *items;
    int      count, cap;
} MigList;

void miglist_init(MigList *m);
void miglist_free(MigList *m);
void miglist_copy(MigList *dst, const MigList *src);
void miglist_add(MigList *m, const char *src, const char *dst);
void miglist_remove(MigList *m, int i);              /* 0-based */
int  miglist_find(const MigList *m, const char *src, const char *dst);

/* Parse "SRC->DST" entries separated by ',' or ';'. Syntax errors -> errs. */
void miglist_parse(MigList *m, const char *spec, DiagList *errs);

/* Validate every band against the resolved tree and annotate the nodes for
 * display (markers, and internal-label flags for clade endpoints). Invalid
 * bands -> errs. Returns 1 if all bands are valid. */
int  miglist_apply(const MigList *m, Resolution *r, DiagList *errs);

/* BPP control-file "migration = N" block (call after miglist_apply succeeds).
 * Returns NULL when there are no bands. Caller frees. */
char *migration_block(const MigList *m, Resolution *r);

/* Print a "M1:  A -> C" legend (colourised when `color`). */
void  migration_legend(const MigList *m, Resolution *r, FILE *fp, int color);

#endif /* BPP_TREE_MIGRATE_H */
