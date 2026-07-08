#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void)
{
    fputs("bpp-tree: out of memory\n", stderr);
    exit(2);
}

long bt_getline(char **lineptr, size_t *n, FILE *fp)
{
    if (!lineptr || !n || !fp) return -1;
    char  *buf = *lineptr;
    size_t cap = *n;
    if (!buf || cap == 0) {
        cap = 128;
        buf = (char *)realloc(buf, cap);
        if (!buf) return -1;
    }
    size_t len = 0;
    int c;
    while ((c = getc(fp)) != EOF) {
        if (len + 1 >= cap) {                 /* keep room for the NUL */
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) { *lineptr = buf; *n = cap; return -1; }
            buf = nb; cap = ncap;
        }
        buf[len++] = (char)c;
        if (c == '\n') break;
    }
    *lineptr = buf;
    *n = cap;
    if (len == 0 && c == EOF) return -1;      /* nothing read */
    buf[len] = '\0';
    return (long)len;
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) oom();

    char *buf = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

int cmp_str(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}
