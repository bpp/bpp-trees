#define _POSIX_C_SOURCE 200809L

#include "repl.h"
#include "parser.h"
#include "resolver.h"
#include "tree.h"
#include "validate.h"
#include "diag.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A tree is stored as the operations that build it: joins (declarative, an
 * order-free set) plus moves/rotations (applied in order afterwards). The
 * resolved tree is recomputed on demand. */
typedef enum { OP_JOIN, OP_MOVE, OP_ROTATE } OpKind;

typedef struct { OpKind kind; char *spec; } Op;

typedef struct {
    char *name;
    Op   *ops;
    int   n_ops, cap;
} NamedTree;

typedef struct {
    NamedTree *trees;
    int        n, cap;
    int        active;
} Workspace;

/* --- tree ops ----------------------------------------------------------- */

static void tree_init(NamedTree *t, const char *name)
{
    t->name = xstrdup(name);
    t->ops = NULL;
    t->n_ops = t->cap = 0;
}

static void tree_add_op(NamedTree *t, OpKind kind, const char *spec)
{
    if (t->n_ops == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->ops = xrealloc(t->ops, (size_t)t->cap * sizeof(Op));
    }
    t->ops[t->n_ops].kind = kind;
    t->ops[t->n_ops].spec = xstrdup(spec);
    t->n_ops++;
}

static void tree_free(NamedTree *t)
{
    for (int i = 0; i < t->n_ops; i++) free(t->ops[i].spec);
    free(t->ops);
    free(t->name);
}

static void tree_copy(NamedTree *dst, const NamedTree *src, const char *name)
{
    tree_init(dst, name);
    for (int i = 0; i < src->n_ops; i++)
        tree_add_op(dst, src->ops[i].kind, src->ops[i].spec);
}

/* Build the resolved tree from the op list: accumulate all joins, resolve,
 * then replay moves/rotations in order. Caller frees *joins and the result. */
static Resolution *tree_build(const NamedTree *t, JoinList *joins,
                              DiagList *errs, DiagList *warns)
{
    joinlist_init(joins);
    for (int i = 0; i < t->n_ops; i++) {
        if (t->ops[i].kind != OP_JOIN) continue;
        const char *s = t->ops[i].spec;
        if (strchr(s, '\n')) parse_joins_text(s, joins, errs);
        else                 parse_joins_string(s, joins, errs);
    }
    Resolution *r = resolve_tree(joins, errs);
    validate_joins(joins, r, errs);
    validate_tree(joins, r, errs, warns);
    if (!errs->count) {
        for (int i = 0; i < t->n_ops; i++) {
            if (t->ops[i].kind == OP_MOVE)        resolution_move(r, t->ops[i].spec, errs, warns);
            else if (t->ops[i].kind == OP_ROTATE) resolution_rotate(r, t->ops[i].spec, errs, warns);
            if (errs->count) break;
        }
    }
    return r;
}

/* --- workspace ---------------------------------------------------------- */

static int ws_find(const Workspace *ws, const char *name)
{
    for (int i = 0; i < ws->n; i++)
        if (strcmp(ws->trees[i].name, name) == 0) return i;
    return -1;
}

static int ws_add(Workspace *ws, NamedTree t)
{
    if (ws->n == ws->cap) {
        ws->cap = ws->cap ? ws->cap * 2 : 4;
        ws->trees = xrealloc(ws->trees, (size_t)ws->cap * sizeof(NamedTree));
    }
    ws->trees[ws->n] = t;
    return ws->n++;
}

static NamedTree *ws_active(Workspace *ws) { return &ws->trees[ws->active]; }

/* --- printing ----------------------------------------------------------- */

static void print_diags(const DiagList *d, const char *kind)
{
    for (int i = 0; i < d->count; i++) {
        const Diagnostic *dg = &d->items[i];
        if (dg->line_no >= 0)
            printf("  %s [%s] (line %d): %s\n", kind, dg->code, dg->line_no, dg->message);
        else
            printf("  %s [%s]: %s\n", kind, dg->code, dg->message);
        if (dg->hint) printf("      hint: %s\n", dg->hint);
    }
}

static void show_status(const NamedTree *t)
{
    if (t->n_ops == 0) {
        printf("[%s] empty — add joins, e.g.  A+B\n", t->name);
        return;
    }
    DiagList errs, warns; JoinList joins;
    diag_init(&errs); diag_init(&warns);
    Resolution *r = tree_build(t, &joins, &errs, &warns);

    print_diags(&warns, "note");
    print_diags(&errs, "error");
    printf("[%s] %d taxa, %s\n", t->name, r->n_leaves,
           r->root ? "tree complete" : "tree incomplete");

    resolution_free(r); joinlist_free(&joins);
    diag_free(&errs); diag_free(&warns);
}

static void cmd_display(const NamedTree *t, int ascii)
{
    if (t->n_ops == 0) { printf("(empty tree — add joins first)\n"); return; }
    DiagList errs, warns; JoinList joins;
    diag_init(&errs); diag_init(&warns);
    Resolution *r = tree_build(t, &joins, &errs, &warns);

    if (r->root) {
        treenode_display(r->root, stdout, ascii, "");
    } else {
        print_diags(&errs, "error");
        printf("(tree is incomplete — nothing to display yet)\n");
    }
    resolution_free(r); joinlist_free(&joins);
    diag_free(&errs); diag_free(&warns);
}

static void cmd_newick(const NamedTree *t)
{
    if (t->n_ops == 0) { printf("(empty tree — add joins first)\n"); return; }
    DiagList errs, warns; JoinList joins;
    diag_init(&errs); diag_init(&warns);
    Resolution *r = tree_build(t, &joins, &errs, &warns);

    if (r->root) {
        char *body = treenode_to_newick(r->root);
        printf("%s;\n", body);
        free(body);
    } else {
        print_diags(&errs, "error");
        printf("(tree is incomplete — no Newick yet)\n");
    }
    resolution_free(r); joinlist_free(&joins);
    diag_free(&errs); diag_free(&warns);
}

static void cmd_list(const Workspace *ws)
{
    for (int i = 0; i < ws->n; i++) {
        const NamedTree *t = &ws->trees[i];
        DiagList errs, warns; JoinList joins;
        diag_init(&errs); diag_init(&warns);
        Resolution *r = tree_build(t, &joins, &errs, &warns);
        const char *state = t->n_ops == 0 ? "empty"
                          : r->root        ? "complete" : "incomplete";
        printf("  %c %-16s %3d taxa  (%s)\n", i == ws->active ? '*' : ' ',
               t->name, r->n_leaves, state);
        resolution_free(r); joinlist_free(&joins);
        diag_free(&errs); diag_free(&warns);
    }
}

static void print_help(void)
{
    fputs(
"Commands (anything else is read as a join formula and added to the tree):\n"
"  A+B [= label]      add a join ('+' to join; ',' or ';' separate several)\n"
"  move SRC -> DST    prune clade SRC and regraft it as the sister of DST\n"
"  rotate LIST        reverse the children of the named clade(s)\n"
"  undo               undo the last change to the active tree\n"
"  display [ascii]    show the active tree as a branching diagram\n"
"  newick             print the active tree's Newick string\n"
"  status             taxa count, completeness, and any guidance\n"
"  trees              list trees in memory (active marked '*')\n"
"  save NAME          save a copy of the active tree as NAME\n"
"  use NAME           make NAME the active tree\n"
"  new NAME           start a new empty tree named NAME and make it active\n"
"  drop NAME          delete the named tree\n"
"  help               show this help\n"
"  quit               leave (also: exit, or end-of-file)\n",
        stdout);
}

/* --- command handling --------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void handle_line(Workspace *ws, char *raw, int *quit)
{
    char *s = trim(raw);
    if (*s == '\0' || *s == '#') return;

    /* split off the first word */
    char *cmd = s, *arg = s;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    size_t clen = (size_t)(arg - cmd);
    while (*arg && isspace((unsigned char)*arg)) arg++;   /* arg = rest */

    #define IS(w) (clen == strlen(w) && strncmp(cmd, w, clen) == 0)

    if (IS("quit") || IS("exit") || IS("q")) { *quit = 1; return; }
    if (IS("help") || IS("h") || IS("?"))    { print_help(); return; }
    if (IS("trees") || IS("list") || IS("ls")) { cmd_list(ws); return; }
    if (IS("status") || IS("st"))            { show_status(ws_active(ws)); return; }
    if (IS("newick") || IS("nwk"))           { cmd_newick(ws_active(ws)); return; }
    if (IS("display") || IS("show") || IS("d")) {
        cmd_display(ws_active(ws), strcmp(arg, "ascii") == 0);
        return;
    }
    if (IS("move")) {
        if (!*arg) { printf("usage: move SRC -> DST\n"); return; }
        tree_add_op(ws_active(ws), OP_MOVE, arg);
        show_status(ws_active(ws));
        return;
    }
    if (IS("rotate")) {
        if (!*arg) { printf("usage: rotate CLADE[,CLADE...]\n"); return; }
        tree_add_op(ws_active(ws), OP_ROTATE, arg);
        show_status(ws_active(ws));
        return;
    }
    if (IS("undo")) {
        NamedTree *t = ws_active(ws);
        if (t->n_ops == 0) { printf("[%s] nothing to undo\n", t->name); return; }
        free(t->ops[--t->n_ops].spec);
        show_status(t);
        return;
    }
    if (IS("save")) {
        if (!*arg) { printf("usage: save NAME\n"); return; }
        int idx = ws_find(ws, arg);
        NamedTree copy;
        tree_copy(&copy, ws_active(ws), arg);
        if (idx >= 0) { tree_free(&ws->trees[idx]); ws->trees[idx] = copy; }
        else          ws_add(ws, copy);
        printf("saved active tree as '%s'\n", arg);
        return;
    }
    if (IS("use") || IS("switch")) {
        if (!*arg) { printf("usage: use NAME\n"); return; }
        int idx = ws_find(ws, arg);
        if (idx < 0) { printf("no tree named '%s' (try 'trees')\n", arg); return; }
        ws->active = idx;
        show_status(ws_active(ws));
        return;
    }
    if (IS("new")) {
        if (!*arg) { printf("usage: new NAME\n"); return; }
        if (ws_find(ws, arg) >= 0) { printf("a tree named '%s' already exists\n", arg); return; }
        NamedTree t; tree_init(&t, arg);
        ws->active = ws_add(ws, t);
        show_status(ws_active(ws));
        return;
    }
    if (IS("drop") || IS("delete")) {
        if (!*arg) { printf("usage: drop NAME\n"); return; }
        int idx = ws_find(ws, arg);
        if (idx < 0) { printf("no tree named '%s'\n", arg); return; }
        tree_free(&ws->trees[idx]);
        for (int i = idx; i < ws->n - 1; i++) ws->trees[i] = ws->trees[i + 1];
        ws->n--;
        if (ws->n == 0) { NamedTree t; tree_init(&t, "main"); ws_add(ws, t); ws->active = 0; }
        else if (ws->active == idx) ws->active = 0;
        else if (ws->active > idx)  ws->active--;
        printf("dropped '%s'; active is now '%s'\n", arg, ws_active(ws)->name);
        return;
    }

    #undef IS

    /* not a command — treat the whole line as a join formula */
    tree_add_op(ws_active(ws), OP_JOIN, s);
    show_status(ws_active(ws));
}

/* --- entry point -------------------------------------------------------- */

int repl_run(const char *seed_joins)
{
    Workspace ws = {0};
    NamedTree main_tree;
    tree_init(&main_tree, "main");
    if (seed_joins && *seed_joins) tree_add_op(&main_tree, OP_JOIN, seed_joins);
    ws_add(&ws, main_tree);
    ws.active = 0;

    int tty = isatty(STDIN_FILENO);
    if (tty)
        fputs("bpp-tree interactive mode. Type 'help' for commands, 'quit' to exit.\n",
              stdout);
    if (seed_joins && *seed_joins) show_status(ws_active(&ws));

    char *line = NULL;
    size_t cap = 0;
    int quit = 0;
    while (!quit) {
        if (tty) { fputs("bpp-tree> ", stdout); fflush(stdout); }
        if (getline(&line, &cap, stdin) < 0) break;   /* EOF */
        handle_line(&ws, line, &quit);
    }
    free(line);
    if (tty) fputc('\n', stdout);

    for (int i = 0; i < ws.n; i++) tree_free(&ws.trees[i]);
    free(ws.trees);
    return 0;
}
