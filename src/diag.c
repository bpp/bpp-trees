#include "diag.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void diag_init(DiagList *d)
{
    d->items = NULL;
    d->count = 0;
    d->cap = 0;
}

void diag_free(DiagList *d)
{
    for (int i = 0; i < d->count; i++) {
        free(d->items[i].message);
        free(d->items[i].hint);
    }
    free(d->items);
    diag_init(d);
}

Diagnostic *diag_add(DiagList *d, const char *code, int line_no,
                     const char *fmt, ...)
{
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->items = xrealloc(d->items, (size_t)d->cap * sizeof(*d->items));
    }
    Diagnostic *dg = &d->items[d->count++];
    dg->code = code;
    dg->line_no = line_no;
    dg->hint = NULL;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    dg->message = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(dg->message, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return dg;
}

void diag_set_hint(Diagnostic *dg, const char *fmt, ...)
{
    free(dg->hint);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    dg->hint = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(dg->hint, (size_t)n + 1, fmt, ap);
    va_end(ap);
}
