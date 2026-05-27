/* lineedit.h — a small dependency-free line editor with history recall.
 *
 * At an interactive terminal it puts the terminal in raw mode and supports
 * inline editing (left/right, Home/End, backspace/delete, Ctrl-A/E/U) and
 * recalling previous commands with the up/down arrows, so a past command can
 * be pulled up and edited before running. When stdin is not a TTY it falls
 * back to a plain line read, so piped/scripted input works unchanged.
 */
#ifndef BPP_TREE_LINEEDIT_H
#define BPP_TREE_LINEEDIT_H

typedef struct {
    char **items;
    int    n, cap;
} History;

/* Append a command to the history (consecutive duplicates are skipped). */
void  hist_push(History *h, const char *line);
void  hist_free(History *h);

/* Read one line, showing `prompt`. Returns a malloc'd line without its
 * trailing newline (caller frees), or NULL on end-of-input. */
char *line_edit(const char *prompt, History *h);

#endif /* BPP_TREE_LINEEDIT_H */
