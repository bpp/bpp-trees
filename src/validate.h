/* validate.h — error and warning checks over parsed joins and the resolved
 * tree. All applicable diagnostics are collected, not just the first. */
#ifndef BPP_TREE_VALIDATE_H
#define BPP_TREE_VALIDATE_H

#include "parser.h"
#include "resolver.h"
#include "diag.h"

/* Checks that need only the parsed joins:
 *   EMPTY_FILE, SINGLE_TAXON_TREE, POLYTOMY_UNSUPPORTED (phase 1),
 *   DUPLICATE_LABEL, TAXON_JOINED_TWICE, CLADE_JOINED_TWICE. */
void validate_joins(const JoinList *joins, const Resolution *r, DiagList *errs);

/* Checks that need the resolved tree:
 *   IMPLICIT_LABEL_CONFLICT (errors),
 *   SINGLE_JOIN_REMAINING, LARGE_TREE, UNUSED_LABEL (warnings). */
void validate_tree(const JoinList *joins, const Resolution *r,
                   DiagList *errs, DiagList *warnings);

#endif /* BPP_TREE_VALIDATE_H */
