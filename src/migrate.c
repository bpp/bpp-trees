#include "migrate.h"
#include "tree.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

void miglist_init(MigList *m) { m->items = NULL; m->count = m->cap = 0; }

void miglist_free(MigList *m)
{
    for (int i = 0; i < m->count; i++) { free(m->items[i].src); free(m->items[i].dst); }
    free(m->items);
    miglist_init(m);
}

void miglist_add(MigList *m, const char *src, const char *dst)
{
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 4;
        m->items = xrealloc(m->items, (size_t)m->cap * sizeof(MigBand));
    }
    m->items[m->count].src = xstrdup(src);
    m->items[m->count].dst = xstrdup(dst);
    m->count++;
}

void miglist_copy(MigList *dst, const MigList *src)
{
    miglist_init(dst);
    for (int i = 0; i < src->count; i++) miglist_add(dst, src->items[i].src, src->items[i].dst);
}

int miglist_find(const MigList *m, const char *src, const char *dst)
{
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->items[i].src, src) == 0 && strcmp(m->items[i].dst, dst) == 0) return i;
    return -1;
}

void miglist_remove(MigList *m, int i)
{
    if (i < 0 || i >= m->count) return;
    free(m->items[i].src); free(m->items[i].dst);
    for (int k = i; k < m->count - 1; k++) m->items[k] = m->items[k + 1];
    m->count--;
}

void miglist_parse(MigList *m, const char *spec, DiagList *errs)
{
    const char *p = spec;
    while (*p) {
        size_t len = strcspn(p, ",;");
        char *piece = xstrndup(p, len);
        p += len; if (*p) p++;

        char *arrow = strstr(piece, "->");
        if (!arrow) {
            diag_add(errs, DIAG_MIGRATION_INVALID, -1,
                "migration '%s' is not of the form SRC->DST.", piece);
            free(piece); continue;
        }
        *arrow = '\0';
        char *s = piece, *d = arrow + 2;
        while (*s == ' ' || *s == '\t') s++;
        char *se = s + strlen(s); while (se > s && (se[-1]==' '||se[-1]=='\t')) *--se = '\0';
        while (*d == ' ' || *d == '\t') d++;
        char *de = d + strlen(d); while (de > d && (de[-1]==' '||de[-1]=='\t')) *--de = '\0';
        if (*s == '\0' || *d == '\0')
            diag_add(errs, DIAG_MIGRATION_INVALID, -1, "migration: missing source or target.");
        else
            miglist_add(m, s, d);
        free(piece);
    }
}

static int is_anc(const TreeNode *a, const TreeNode *b)
{
    for (const TreeNode *p = b; p; p = p->parent) if (p == a) return 1;
    return 0;
}

int miglist_apply(const MigList *m, Resolution *r, DiagList *errs)
{
    int ok = 1;
    for (int k = 0; k < m->count; k++) {
        const char *S = m->items[k].src, *D = m->items[k].dst;
        TreeNode *src = resolution_find(r, S), *dst = resolution_find(r, D);
        if (!src || !dst) {
            diag_add(errs, DIAG_MIGRATION_UNKNOWN, -1,
                "migration: %s '%s' is not in the tree.", src ? "target" : "source",
                src ? D : S);
            ok = 0; continue;
        }
        if (src == dst) {
            diag_add(errs, DIAG_MIGRATION_INVALID, -1,
                "migration: source and target are the same ('%s').", S);
            ok = 0; continue;
        }
        if (is_anc(src, dst) || is_anc(dst, src)) {
            Diagnostic *d = diag_add(errs, DIAG_MIGRATION_INVALID, -1,
                "migration: '%s' and '%s' are ancestor and descendant; they do not "
                "coexist in time.", S, D);
            diag_set_hint(d, "a band must connect two non-nested (contemporaneous) "
                             "branches.");
            ok = 0; continue;
        }
        treenode_add_mig(src, +(k + 1));      /* source of band k+1 */
        treenode_add_mig(dst, -(k + 1));      /* destination */
        if (!src->is_leaf) src->show_label = 1;
        if (!dst->is_leaf) dst->show_label = 1;
    }
    return ok;
}

char *migration_block(const MigList *m, Resolution *r)
{
    if (m->count == 0) return NULL;
    char *out = xasprintf("migration = %d", m->count);
    for (int k = 0; k < m->count; k++) {
        TreeNode *src = resolution_find(r, m->items[k].src);
        TreeNode *dst = resolution_find(r, m->items[k].dst);
        const char *sn = src ? treenode_bpp_name(src) : m->items[k].src;
        const char *dn = dst ? treenode_bpp_name(dst) : m->items[k].dst;
        char *tmp = xasprintf("%s\n  %s  %s", out, sn, dn);
        free(out); out = tmp;
    }
    return out;
}

void migration_legend(const MigList *m, Resolution *r, FILE *fp, int color)
{
    if (m->count == 0) return;
    fputs("migration bands:\n", fp);
    for (int k = 0; k < m->count; k++) {
        TreeNode *src = resolution_find(r, m->items[k].src);
        TreeNode *dst = resolution_find(r, m->items[k].dst);
        const char *sn = src ? treenode_bpp_name(src) : m->items[k].src;
        const char *dn = dst ? treenode_bpp_name(dst) : m->items[k].dst;
        fputs("  ", fp);
        if (color) fputs(treenode_mig_color(k + 1), fp);
        fprintf(fp, "M%d", k + 1);
        if (color) fputs(TREENODE_MIG_RESET, fp);
        fprintf(fp, ":  %s \xe2\x86\x92 %s\n", sn, dn);   /* → */
    }
}
