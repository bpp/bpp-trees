/* diag.h — diagnostics (errors and warnings) collected during a run.
 *
 * Every diagnostic carries a stable string code, an optional source line
 * number, a human-readable message, and an optional suggested-fix hint.
 * All diagnostics are collected and reported together, not just the first.
 */
#ifndef BPP_TREE_DIAG_H
#define BPP_TREE_DIAG_H

/* Error codes (output suppressed). */
/* A multi-member clade reference (3+ leaves) that no join builds — its
 * branching order cannot be inferred from the name. */
#define DIAG_AMBIGUOUS_CLADE        "AMBIGUOUS_CLADE"
#define DIAG_TAXON_JOINED_TWICE     "TAXON_JOINED_TWICE"
#define DIAG_CLADE_JOINED_TWICE     "CLADE_JOINED_TWICE"
#define DIAG_CYCLE                  "CYCLE"
#define DIAG_DISCONNECTED           "DISCONNECTED"
#define DIAG_DUPLICATE_LABEL        "DUPLICATE_LABEL"
/* '_' is reserved for implicit clade labels; an explicit label must not use it. */
#define DIAG_LABEL_RESERVED_UNDERSCORE "LABEL_RESERVED_UNDERSCORE"
/* A --rotate target that names no clade in the tree. */
#define DIAG_ROTATE_UNKNOWN         "ROTATE_UNKNOWN"
/* --move: a source/target that names nothing, or an illegal move. */
#define DIAG_MOVE_UNKNOWN           "MOVE_UNKNOWN"
#define DIAG_MOVE_INVALID           "MOVE_INVALID"
/* graft: add a new tip as the sister of an existing node. */
#define DIAG_GRAFT_UNKNOWN          "GRAFT_UNKNOWN"
#define DIAG_GRAFT_INVALID          "GRAFT_INVALID"
/* prune: remove a tip or subtree from the tree. */
#define DIAG_PRUNE_UNKNOWN          "PRUNE_UNKNOWN"
#define DIAG_PRUNE_INVALID          "PRUNE_INVALID"
/* migration (MSC-M) bands. */
#define DIAG_MIGRATION_UNKNOWN      "MIGRATION_UNKNOWN"
#define DIAG_MIGRATION_INVALID      "MIGRATION_INVALID"
#define DIAG_INTROGRESSION_UNKNOWN  "INTROGRESSION_UNKNOWN"
#define DIAG_INTROGRESSION_INVALID  "INTROGRESSION_INVALID"
#define DIAG_MODEL_CONFLICT         "MODEL_CONFLICT"   /* migration XOR introgression */
#define DIAG_EMPTY_FILE             "EMPTY_FILE"
#define DIAG_SINGLE_TAXON_TREE      "SINGLE_TAXON_TREE"
#define DIAG_SYNTAX                 "SYNTAX"
/* Phase 1: polytomies (joins with more than two operands) are not yet
 * supported; they are planned for a later phase. */
#define DIAG_POLYTOMY_UNSUPPORTED   "POLYTOMY_UNSUPPORTED"

/* Warning / note codes (output still produced). */
#define DIAG_PAIR_AUTO_CREATED      "PAIR_AUTO_CREATED"
#define DIAG_ROOT_AUTO_JOINED       "ROOT_AUTO_JOINED"
#define DIAG_ROTATE_IGNORED_TIP     "ROTATE_IGNORED_TIP"
#define DIAG_MOVE_NOOP              "MOVE_NOOP"
#define DIAG_MIGRATION_NOOP         "MIGRATION_NOOP"
#define DIAG_LARGE_TREE             "LARGE_TREE"
#define DIAG_UNUSED_LABEL           "UNUSED_LABEL"

typedef struct {
    const char *code;   /* stable, static string */
    int   line_no;      /* -1 if not line-specific */
    char *message;      /* owned */
    char *hint;         /* owned, may be NULL */
} Diagnostic;

typedef struct {
    Diagnostic *items;
    int count;
    int cap;
} DiagList;

void diag_init(DiagList *d);
void diag_free(DiagList *d);

/* Append a diagnostic; message is printf-formatted. Returns the new
 * diagnostic so a hint can be attached with diag_set_hint(). */
Diagnostic *diag_add(DiagList *d, const char *code, int line_no,
                     const char *fmt, ...);

/* Attach (or replace) a printf-formatted hint on an existing diagnostic. */
void diag_set_hint(Diagnostic *dg, const char *fmt, ...);

#endif /* BPP_TREE_DIAG_H */
