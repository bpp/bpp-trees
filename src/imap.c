#define _POSIX_C_SOURCE 200809L

#include "imap.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void imap_bump(Imap *m, const char *species)
{
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->items[i].species, species) == 0) {
            m->items[i].count++;
            return;
        }
    }
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->items = xrealloc(m->items, (size_t)m->cap * sizeof(*m->items));
    }
    m->items[m->count].species = xstrdup(species);
    m->items[m->count].count = 1;
    m->count++;
}

Imap *imap_read(const char *path, DiagList *errs)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        diag_add(errs, "SYSTEM", -1, "cannot open Imap file '%s'.", path);
        return NULL;
    }

    Imap *m = xcalloc(1, sizeof(*m));

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = bt_getline(&line, &cap, fp)) != -1) {
        /* tokenise: first field = sample, second field = species */
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0' || *p == '#') continue;

        char *s1 = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;          /* only one field — skip */
        *p++ = '\0';
        (void)s1;                          /* sample name, not needed */

        while (*p == ' ' || *p == '\t') p++;
        char *s2 = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        *p = '\0';
        if (*s2 == '\0') continue;

        imap_bump(m, s2);
    }
    free(line);
    fclose(fp);
    return m;
}

void imap_free(Imap *m)
{
    if (!m) return;
    for (int i = 0; i < m->count; i++) free(m->items[i].species);
    free(m->items);
    free(m);
}

int imap_count_for(const Imap *m, const char *species)
{
    if (!m) return -1;
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->items[i].species, species) == 0)
            return m->items[i].count;
    return -1;
}
