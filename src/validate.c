#include "validate.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* line list helper for "appears in lines a, b, c" messages */
static char *lines_str(const int *lines, int n)
{
    char *s = NULL;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        char *piece = xasprintf("%d", lines[i]);
        const char *sep = (i == 0) ? "" : (i == n - 1 ? " and " : ", ");
        s = xrealloc(s, len + strlen(sep) + strlen(piece) + 1);
        memcpy(s + len, sep, strlen(sep)); len += strlen(sep);
        memcpy(s + len, piece, strlen(piece) + 1); len += strlen(piece);
        free(piece);
    }
    return s ? s : xstrdup("");
}

void validate_joins(const JoinList *joins, const Resolution *r, DiagList *errs)
{
    if (joins->count == 0) {
        Diagnostic *d = diag_add(errs, DIAG_EMPTY_FILE, -1,
            "no join statements found.");
        diag_set_hint(d, "the file must contain at least one join, e.g.:  A+B");
        return;
    }

    int real_joins = 0;
    for (int j = 0; j < joins->count; j++)
        if (joins->items[j].n_operands >= 2) real_joins++;

    if (real_joins == 0) {
        diag_add(errs, DIAG_SINGLE_TAXON_TREE, joins->items[0].line_no,
            "a BPP species tree requires at least two taxa joined together; "
            "no joins were found.");
        return;
    }

    /* POLYTOMY_UNSUPPORTED — phase 1 handles binary trees only */
    for (int j = 0; j < joins->count; j++) {
        if (joins->items[j].n_operands > 2) {
            Diagnostic *d = diag_add(errs, DIAG_POLYTOMY_UNSUPPORTED,
                joins->items[j].line_no,
                "join has %d operands (a polytomy). Phase 1 supports binary "
                "trees only.", joins->items[j].n_operands);
            diag_set_hint(d, "split it into binary joins, e.g. 'A+B+C' becomes "
                             "'A+B = AB' then 'AB+C'. Polytomy support is planned "
                             "for a later phase.");
        }
    }

    /* LABEL_RESERVED_UNDERSCORE — '_' is reserved for implicit clade labels */
    for (int j = 0; j < joins->count; j++) {
        const char *lbl = joins->items[j].label;
        if (lbl && strchr(lbl, '_')) {
            Diagnostic *d = diag_add(errs, DIAG_LABEL_RESERVED_UNDERSCORE,
                joins->items[j].line_no,
                "explicit label '%s' contains '_', which is reserved for the "
                "implicit labels of joined clades.", lbl);
            diag_set_hint(d, "choose a label without '_', e.g. '%.*s'.",
                          (int)(strchr(lbl, '_') - lbl), lbl);
        }
    }

    /* DUPLICATE_LABEL */
    for (int j = 0; j < joins->count; j++) {
        const char *lbl = joins->items[j].label;
        if (!lbl) continue;
        int lines[64]; int nl = 0;
        int first = 1;
        for (int k = 0; k < joins->count; k++) {
            if (joins->items[k].label && strcmp(joins->items[k].label, lbl) == 0) {
                if (k < j) { first = 0; break; }  /* report once, at first def */
                if (nl < 64) lines[nl++] = joins->items[k].line_no;
            }
        }
        if (first && nl >= 2) {
            char *ls = lines_str(lines, nl);
            Diagnostic *d = diag_add(errs, DIAG_DUPLICATE_LABEL, -1,
                "label '%s' is defined by more than one join (lines %s).", lbl, ls);
            diag_set_hint(d, "each label must be unique.");
            free(ls);
        }
    }

    /* TAXON_JOINED_TWICE / CLADE_JOINED_TWICE are detected in the resolver
     * via per-node reference counts (which also catches a clade joined under
     * both its explicit and implicit names). */
    (void)r;
}

void validate_tree(const JoinList *joins, const Resolution *r,
                   DiagList *errs, DiagList *warnings)
{
    (void)errs;
    if (!r->root) return;  /* incomplete: skip tree-shape notes */

    /* PAIR_AUTO_CREATED: a 2-member clade we built from its members. */
    for (int a = 0; a < r->n_auto; a++) {
        TreeNode *nd = r->auto_nodes[a];
        diag_add(warnings, DIAG_PAIR_AUTO_CREATED, nd->join_line,
            "clade '%s' was created implicitly from taxa %s and %s. Write "
            "'%s+%s' to make it explicit.", nd->implicit_label,
            nd->children[0]->name, nd->children[1]->name,
            nd->children[0]->name, nd->children[1]->name);
    }

    /* ROOT_AUTO_JOINED: the two remaining clades were joined to finish. */
    if (r->synth_root)
        diag_add(warnings, DIAG_ROOT_AUTO_JOINED, -1,
            "the two remaining clades ('%s', '%s') were joined automatically "
            "to complete the tree.",
            r->synth_root->children[0]->name, r->synth_root->children[1]->name);

    if (r->n_leaves > 50)
        diag_add(warnings, DIAG_LARGE_TREE, -1,
            "%d taxa specified. BPP analyses with large species trees may "
            "require long MCMC runs.", r->n_leaves);

    /* UNUSED_LABEL: explicit label assigned to the root (never referenced) */
    for (int j = 0; j < r->n_joins; j++) {
        if (r->resolved[j] && joins->items[j].label && !r->referenced[j]) {
            diag_add(warnings, DIAG_UNUSED_LABEL, joins->items[j].line_no,
                "label '%s' is assigned to the root clade and is not referenced "
                "by any further join.", joins->items[j].label);
        }
    }
}
