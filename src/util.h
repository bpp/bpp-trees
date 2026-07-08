/* util.h — small allocation and string helpers (abort on OOM). */
#ifndef BPP_TREE_UTIL_H
#define BPP_TREE_UTIL_H

#include <stddef.h>
#include <stdio.h>

/* Portable getline() replacement (POSIX getline is hidden under -std=c11 on
 * MinGW). Same contract: grows *lineptr (malloc/realloc, caller frees), keeps
 * the trailing '\n', NUL-terminates, returns line length or -1 at EOF. */
long bt_getline(char **lineptr, size_t *n, FILE *fp);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* printf into a freshly malloc'd string (caller frees). Aborts on OOM. */
char *xasprintf(const char *fmt, ...);

/* qsort comparator for arrays of `char *` (alphabetical). */
int cmp_str(const void *a, const void *b);

#endif /* BPP_TREE_UTIL_H */
