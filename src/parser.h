/* parser.h — tokeniser and join-statement parser for the .joins format.
 *
 * Grammar (informal):
 *   line     := blank | comment | join
 *   comment  := ws* '#' ...
 *   join     := operands ('=' label)? ws* comment?
 *   operands := token ('+' token)+
 *   token    := [A-Za-z0-9_-]+
 */
#ifndef BPP_TREE_PARSER_H
#define BPP_TREE_PARSER_H

#include "diag.h"

typedef struct {
    char **operands;   /* owned array of owned token strings */
    int    n_operands;
    char  *label;      /* explicit label (owned), or NULL */
    int    line_no;    /* 1-based source line for diagnostics */
} JoinStmt;

typedef struct {
    JoinStmt *items;
    int count;
    int cap;
} JoinList;

void joinlist_init(JoinList *j);
void joinlist_free(JoinList *j);

/* Parse newline-separated join text. Syntax errors are appended to errs.
 * Returns the number of syntax errors added. Even on error, any joins that
 * parsed cleanly are present in `out`. */
int parse_joins_text(const char *text, JoinList *out, DiagList *errs);

/* Parse a --joins string: statements separated by ',' or ';'. Each becomes
 * one join, numbered by its position (line_no = 1-based index). */
int parse_joins_string(const char *str, JoinList *out, DiagList *errs);

#endif /* BPP_TREE_PARSER_H */
