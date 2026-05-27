#define _POSIX_C_SOURCE 200809L

#include "lineedit.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define LE_MAX 4096

void hist_push(History *h, const char *line)
{
    if (h->n > 0 && strcmp(h->items[h->n - 1], line) == 0) return;  /* no dup */
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 32;
        h->items = xrealloc(h->items, (size_t)h->cap * sizeof(char *));
    }
    h->items[h->n++] = xstrdup(line);
}

void hist_free(History *h)
{
    for (int i = 0; i < h->n; i++) free(h->items[i]);
    free(h->items);
    h->items = NULL;
    h->n = h->cap = 0;
}

/* Plain read for non-interactive input. */
static char *read_cooked(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) { free(line); return NULL; }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    return line;
}

/* Redraw the single edit line and place the cursor at `pos`. */
static void refresh(const char *prompt, const char *buf, size_t pos)
{
    char seq[64];
    size_t plen = strlen(prompt);
    write(STDOUT_FILENO, "\r", 1);
    write(STDOUT_FILENO, prompt, plen);
    write(STDOUT_FILENO, buf, strlen(buf));
    write(STDOUT_FILENO, "\x1b[0K", 4);                 /* clear to end of line */
    size_t col = plen + pos;
    if (col > 0) { int n = snprintf(seq, sizeof seq, "\r\x1b[%zuC", col); write(STDOUT_FILENO, seq, (size_t)n); }
    else         write(STDOUT_FILENO, "\r", 1);
}

static void buf_set(char *buf, size_t *pos, size_t *len, const char *s)
{
    strncpy(buf, s, LE_MAX - 1);
    buf[LE_MAX - 1] = '\0';
    *len = *pos = strlen(buf);
}

static int is_token_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Replace the word [wstart,*pos) with `full`, optionally adding a space. */
static void apply_completion(char *buf, size_t *pos, size_t *len,
                             int wstart, const char *full, int add_space)
{
    size_t flen = strlen(full);
    size_t tail = *len - *pos;
    size_t add = flen + (add_space ? 1 : 0);
    if ((size_t)wstart + add + tail >= LE_MAX) return;   /* would overflow */
    memmove(buf + wstart + add, buf + *pos, tail);
    memcpy(buf + wstart, full, flen);
    if (add_space) buf[wstart + flen] = ' ';
    *len = (size_t)wstart + add + tail;
    *pos = (size_t)wstart + add;
    buf[*len] = '\0';
}

/* The interactive edit loop (terminal already in raw mode). */
static char *edit_raw(const char *prompt, History *h,
                      le_complete_fn complete, void *ctx)
{
    char buf[LE_MAX] = {0};
    char stash[LE_MAX] = {0};   /* the in-progress line, saved while browsing history */
    size_t pos = 0, len = 0;
    int hidx = h->n;            /* h->n means "the current (new) line" */

    refresh(prompt, buf, pos);

    for (;;) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) return NULL;            /* EOF / error */

        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\n", 1);
            return xstrdup(buf);
        } else if (c == 3) {            /* Ctrl-C: cancel the current line */
            write(STDOUT_FILENO, "^C\n", 3);
            return xstrdup("");
        } else if (c == 4) {            /* Ctrl-D: EOF if empty, else delete-forward */
            if (len == 0) { write(STDOUT_FILENO, "\n", 1); return NULL; }
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); buf[--len] = '\0'; }
        } else if (c == 127 || c == 8) {   /* Backspace */
            if (pos > 0) { memmove(buf + pos - 1, buf + pos, len - pos + 1); pos--; len--; }
        } else if (c == 1) {  pos = 0;                 /* Ctrl-A */
        } else if (c == 5) {  pos = len;               /* Ctrl-E */
        } else if (c == 2) {  if (pos > 0) pos--;      /* Ctrl-B */
        } else if (c == 6) {  if (pos < len) pos++;    /* Ctrl-F */
        } else if (c == 21) { buf[0] = '\0'; pos = len = 0;   /* Ctrl-U */
        } else if (c == 11) { buf[pos] = '\0'; len = pos;     /* Ctrl-K: kill to end */
        } else if (c == 9) {            /* Tab: completion */
            if (complete) {
                int wstart = (int)pos;
                while (wstart > 0 && is_token_char((unsigned char)buf[wstart - 1])) wstart--;
                char **cand = NULL;
                int nc = complete(buf, wstart, (int)pos, &cand, ctx);
                if (nc == 1) {
                    apply_completion(buf, &pos, &len, wstart, cand[0], 1);
                } else if (nc > 1) {
                    size_t lcp = strlen(cand[0]);          /* longest common prefix */
                    for (int i = 1; i < nc; i++) {
                        size_t j = 0;
                        while (j < lcp && cand[i][j] == cand[0][j]) j++;
                        lcp = j;
                    }
                    if (lcp > pos - (size_t)wstart) {
                        char *pref = xstrndup(cand[0], lcp);
                        apply_completion(buf, &pos, &len, wstart, pref, 0);
                        free(pref);
                    } else {                               /* list the candidates */
                        write(STDOUT_FILENO, "\n", 1);
                        for (int i = 0; i < nc; i++) {
                            write(STDOUT_FILENO, cand[i], strlen(cand[i]));
                            write(STDOUT_FILENO, "  ", 2);
                        }
                        write(STDOUT_FILENO, "\n", 1);
                    }
                }
                for (int i = 0; i < nc; i++) free(cand[i]);
                free(cand);
            }
        } else if (c == 27) {           /* escape sequence */
            char a, b;
            if (read(STDIN_FILENO, &a, 1) <= 0) continue;
            if (read(STDIN_FILENO, &b, 1) <= 0) continue;
            if (a == '[') {
                if (b == 'A') {                 /* Up: older history */
                    if (hidx > 0) {
                        if (hidx == h->n) { strncpy(stash, buf, LE_MAX - 1); stash[LE_MAX-1] = '\0'; }
                        hidx--;
                        buf_set(buf, &pos, &len, h->items[hidx]);
                    }
                } else if (b == 'B') {          /* Down: newer history */
                    if (hidx < h->n) {
                        hidx++;
                        buf_set(buf, &pos, &len, hidx == h->n ? stash : h->items[hidx]);
                    }
                } else if (b == 'C') { if (pos < len) pos++;   /* Right */
                } else if (b == 'D') { if (pos > 0) pos--;     /* Left */
                } else if (b == 'H') { pos = 0;                /* Home */
                } else if (b == 'F') { pos = len;              /* End */
                } else if (b >= '0' && b <= '9') {             /* e.g. 1~ 3~ 4~ */
                    char t; if (read(STDIN_FILENO, &t, 1) <= 0) { /* ignore */ }
                    if (b == '1') pos = 0;
                    else if (b == '4') pos = len;
                    else if (b == '3' && pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); buf[--len] = '\0'; }
                }
            }
        } else if ((unsigned char)c >= 32) {       /* printable: insert at cursor */
            if (len < LE_MAX - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos++] = c;
                len++;
            }
        }
        refresh(prompt, buf, pos);
    }
}

char *line_edit(const char *prompt, History *h, le_complete_fn complete, void *ctx)
{
    if (!isatty(STDIN_FILENO)) return read_cooked();

    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) < 0) return read_cooked();
    raw = orig;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return read_cooked();

    fflush(stdout);
    char *res = edit_raw(prompt, h, complete, ctx);
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    return res;
}
