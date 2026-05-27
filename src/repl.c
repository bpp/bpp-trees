#define _POSIX_C_SOURCE 200809L

#include "repl.h"
#include "parser.h"
#include "resolver.h"
#include "tree.h"
#include "validate.h"
#include "diag.h"
#include "util.h"
#include "imap.h"
#include "block.h"
#include "lineedit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A tree is stored as the operations that build it: joins (declarative, an
 * order-free set) plus moves/rotations (applied in order afterwards). The
 * resolved tree is recomputed on demand. */
typedef enum { OP_JOIN, OP_MOVE, OP_ROTATE, OP_GRAFT, OP_PRUNE } OpKind;

typedef struct { OpKind kind; char *spec; } Op;

typedef struct {
    char *name;
    Op   *ops;
    int   n_ops, cap;
    char *imap_path;     /* attached Imap file, or NULL */
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
    t->imap_path = NULL;
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
    free(t->imap_path);
}

static void tree_copy(NamedTree *dst, const NamedTree *src, const char *name)
{
    tree_init(dst, name);
    for (int i = 0; i < src->n_ops; i++)
        tree_add_op(dst, src->ops[i].kind, src->ops[i].spec);
    if (src->imap_path) dst->imap_path = xstrdup(src->imap_path);
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
            else if (t->ops[i].kind == OP_GRAFT)  resolution_graft(r, t->ops[i].spec, errs, warns);
            else if (t->ops[i].kind == OP_PRUNE)  resolution_prune(r, t->ops[i].spec, errs, warns);
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
        /* line numbers are per-entry and meaningless across a session — omit */
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

/* Apply a transform only if it succeeds on the current tree, so a failed edit
 * never gets committed (which would re-error on every later recompute). */
static void try_transform(NamedTree *t, OpKind kind, const char *spec)
{
    JoinList joins; DiagList errs, warns;
    diag_init(&errs); diag_init(&warns);
    Resolution *r = tree_build(t, &joins, &errs, &warns);
    int ok = 0;

    if (!r->root) {
        printf("the active tree isn't complete yet — finish it before editing\n");
        print_diags(&errs, "error");
    } else {
        DiagList te; diag_init(&te);
        if      (kind == OP_MOVE)   resolution_move(r, spec, &te, &warns);
        else if (kind == OP_ROTATE) resolution_rotate(r, spec, &te, &warns);
        else if (kind == OP_GRAFT)  resolution_graft(r, spec, &te, &warns);
        else                        resolution_prune(r, spec, &te, &warns);
        if (te.count == 0) ok = 1;
        else { print_diags(&te, "error"); printf("(not applied)\n"); }
        diag_free(&te);
    }
    resolution_free(r); joinlist_free(&joins);
    diag_free(&errs); diag_free(&warns);

    if (ok) { tree_add_op(t, kind, spec); show_status(t); }
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

static char *read_file_all(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap); }
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* `arg`: empty -> stdout; "replace FILE" -> splice into a control file;
 * otherwise -> write the block to FILE. */
static void cmd_block(const NamedTree *t, const char *arg)
{
    if (t->n_ops == 0) { printf("(empty tree — add joins first)\n"); return; }
    DiagList errs, warns; JoinList joins;
    diag_init(&errs); diag_init(&warns);
    Resolution *r = tree_build(t, &joins, &errs, &warns);
    if (!r->root) {
        print_diags(&errs, "error");
        printf("(tree is incomplete — no block yet)\n");
        resolution_free(r); joinlist_free(&joins); diag_free(&errs); diag_free(&warns);
        return;
    }
    TreeNode **taxa = NULL; int nt = 0, tc = 0;
    treenode_collect_leaves(r->root, &taxa, &nt, &tc);
    char *body = treenode_to_newick(r->root);
    char *nwk = xasprintf("%s;", body);
    free(body);

    Imap *m = NULL;
    if (t->imap_path) {
        DiagList ie; diag_init(&ie);
        m = imap_read(t->imap_path, &ie);
        if (!m) print_diags(&ie, "error");
        diag_free(&ie);
    }
    int filled = 0, *counts = NULL;
    char *block = species_block(taxa, nt, nwk, m, &filled, &counts);

    while (*arg == ' ' || *arg == '\t') arg++;

    if (*arg == '\0') {                                 /* print to stdout */
        printf("%s\n", block);
        if (!filled) {
            if (!t->imap_path)
                printf("(counts are '?'; attach an Imap with 'imap FILE')\n");
            else {
                printf("(no count for:");
                for (int i = 0; i < nt; i++) if (counts[i] < 0) printf(" %s", taxa[i]->name);
                printf(")\n");
            }
        }
    } else if (strncmp(arg, "replace", 7) == 0 &&
               (arg[7] == ' ' || arg[7] == '\t' || arg[7] == '\0')) {
        const char *file = arg + 7;                     /* replace in a control file */
        while (*file == ' ' || *file == '\t') file++;
        if (!*file) {
            printf("usage: block replace FILE\n");
        } else {
            char *txt = read_file_all(file);
            if (!txt) {
                printf("cannot read '%s'\n", file);
            } else {
                const char *err = NULL;
                char *outc = control_replace_block(txt, block, &err);
                if (!outc) {
                    printf("%s\n", err);
                } else {
                    char *bak = xasprintf("%s.bak", file);
                    FILE *fb = fopen(bak, "w");
                    if (fb) { fputs(txt, fb); fclose(fb); }
                    FILE *fo = fopen(file, "w");
                    if (!fo) { printf("cannot write '%s'\n", file); }
                    else { fputs(outc, fo); fclose(fo);
                           printf("replaced species&tree in '%s' (backup: %s)\n", file, bak); }
                    free(bak); free(outc);
                }
                free(txt);
            }
        }
    } else {                                            /* write block to a file */
        FILE *fo = fopen(arg, "w");
        if (!fo) printf("cannot write '%s'\n", arg);
        else { fprintf(fo, "%s\n", block); fclose(fo);
               printf("wrote species&tree block to '%s'\n", arg); }
    }

    free(taxa); free(nwk); free(block); free(counts); imap_free(m);
    resolution_free(r); joinlist_free(&joins); diag_free(&errs); diag_free(&warns);
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
"  graft NEW -> DST   add a new tip NEW beside DST (or a subtree '(A+B;C+D)')\n"
"  prune LIST         remove tips/subtrees (also 'remove')\n"
"  rotate LIST        reverse the children of the named clade(s)\n"
"  undo               undo the last change to the active tree\n"
"  display [ascii]    show the active tree as a branching diagram\n"
"  newick             print the active tree's Newick string\n"
"  block [FILE]       print the species&tree block (to stdout, or write to FILE)\n"
"  block replace FILE replace the species&tree block in a BPP control file\n"
"  imap [FILE]        attach an Imap file (no arg: show; 'clear': detach)\n"
"  status             taxa count, completeness, and any guidance\n"
"  history            list the commands entered this session\n"
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

/* A contradiction (vs. a merely-incomplete tree): adding such a join must be
 * rejected. Incomplete/ambiguous states are fine while building, so that
 * joins can still be entered in any order. */
static int is_blocking_code(const char *code)
{
    return strcmp(code, DIAG_TAXON_JOINED_TWICE) == 0
        || strcmp(code, DIAG_CLADE_JOINED_TWICE) == 0
        || strcmp(code, DIAG_DUPLICATE_LABEL) == 0
        || strcmp(code, DIAG_LABEL_RESERVED_UNDERSCORE) == 0
        || strcmp(code, DIAG_POLYTOMY_UNSUPPORTED) == 0
        || strcmp(code, DIAG_CYCLE) == 0;
}

static void handle_line(Workspace *ws, History *hist, char *raw, int *quit)
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
    if (IS("history") || IS("hist")) {
        for (int i = 0; i < hist->n; i++) printf("  %d  %s\n", i + 1, hist->items[i]);
        return;
    }
    if (IS("status") || IS("st"))            { show_status(ws_active(ws)); return; }
    if (IS("newick") || IS("nwk"))           { cmd_newick(ws_active(ws)); return; }
    if (IS("block"))                         { cmd_block(ws_active(ws), arg); return; }
    if (IS("imap")) {
        NamedTree *t = ws_active(ws);
        if (!*arg) {
            if (t->imap_path) printf("imap: %s\n", t->imap_path);
            else              printf("no imap attached (use 'imap FILE')\n");
            return;
        }
        if (strcmp(arg, "clear") == 0 || strcmp(arg, "none") == 0) {
            free(t->imap_path); t->imap_path = NULL;
            printf("imap detached\n");
            return;
        }
        DiagList ie; diag_init(&ie);
        Imap *m = imap_read(arg, &ie);          /* validate it opens/parses */
        if (!m) { print_diags(&ie, "error"); diag_free(&ie); return; }
        free(t->imap_path); t->imap_path = xstrdup(arg);
        printf("imap attached to '%s': %s (%d species)\n", t->name, arg, m->count);
        imap_free(m); diag_free(&ie);
        return;
    }
    if (IS("display") || IS("show") || IS("d")) {
        cmd_display(ws_active(ws), strcmp(arg, "ascii") == 0);
        return;
    }
    if (IS("move")) {
        if (!*arg) { printf("usage: move SRC -> DST\n"); return; }
        try_transform(ws_active(ws), OP_MOVE, arg);
        return;
    }
    if (IS("rotate")) {
        if (!*arg) { printf("usage: rotate CLADE[,CLADE...]\n"); return; }
        try_transform(ws_active(ws), OP_ROTATE, arg);
        return;
    }
    if (IS("graft") || IS("add")) {
        if (!*arg) { printf("usage: graft NEW -> TARGET\n"); return; }
        try_transform(ws_active(ws), OP_GRAFT, arg);
        return;
    }
    if (IS("prune") || IS("remove") || IS("rm")) {
        if (!*arg) { printf("usage: prune CLADE[,CLADE...]\n"); return; }
        try_transform(ws_active(ws), OP_PRUNE, arg);
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

    /* Not a command — try it as a join, but only commit if it parses cleanly
     * into at least one real join. This keeps typos and unknown commands from
     * polluting the active tree. */
    JoinList tmp; DiagList terr;
    joinlist_init(&tmp); diag_init(&terr);
    if (strchr(s, '\n')) parse_joins_text(s, &tmp, &terr);
    else                 parse_joins_string(s, &tmp, &terr);
    int real = 0;
    for (int i = 0; i < tmp.count; i++)
        if (tmp.items[i].n_operands >= 2) real = 1;

    if (terr.count == 0 && real) {
        NamedTree *t = ws_active(ws);
        tree_add_op(t, OP_JOIN, s);
        /* trial-build: reject a join that creates a contradiction (a taxon or
         * clade used twice, etc.); an incomplete/ambiguous result is fine. */
        JoinList j2; DiagList e2, w2;
        diag_init(&e2); diag_init(&w2);
        Resolution *r2 = tree_build(t, &j2, &e2, &w2);
        int blocked = 0;
        for (int i = 0; i < e2.count; i++)
            if (is_blocking_code(e2.items[i].code)) blocked = 1;
        if (blocked) {
            print_diags(&e2, "error");
            printf("(not added)\n");
            free(t->ops[--t->n_ops].spec);          /* undo the commit */
        }
        resolution_free(r2); joinlist_free(&j2);
        diag_free(&e2); diag_free(&w2);
        if (!blocked) show_status(t);
    } else if (strchr(s, '+')) {
        print_diags(&terr, "error");
        printf("(not added — fix the join and retype it)\n");
    } else {
        printf("unknown command '%.*s' — type 'help' for the command list\n",
               (int)clen, cmd);
    }
    joinlist_free(&tmp); diag_free(&terr);
}

/* --- tab completion ----------------------------------------------------- */

static const char *const COMMANDS[] = {
    "help", "quit", "exit", "display", "newick", "block", "imap", "status",
    "trees", "history", "save", "use", "new", "drop", "move", "graft", "prune",
    "remove", "rotate",
    NULL
};

static void add_cand(char ***arr, int *n, int *cap, const char *s)
{
    for (int i = 0; i < *n; i++) if (strcmp((*arr)[i], s) == 0) return;  /* dedup */
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *arr = xrealloc(*arr, (size_t)*cap * sizeof(char *)); }
    (*arr)[(*n)++] = xstrdup(s);
}

/* clade/tip identifiers in the current tree: tip names, and each internal
 * node's implicit (leaf-set) label and explicit label. */
static void collect_nodes(const TreeNode *nd, char ***arr, int *n, int *cap)
{
    if (!nd) return;
    if (nd->is_leaf) { add_cand(arr, n, cap, nd->name); return; }
    add_cand(arr, n, cap, nd->implicit_label);
    if (nd->explicit_label) add_cand(arr, n, cap, nd->explicit_label);
    for (int i = 0; i < nd->n_children; i++) collect_nodes(nd->children[i], arr, n, cap);
}

static int repl_complete(const char *buf, int wstart, int wend, char ***out, void *ctx)
{
    Workspace *ws = ctx;
    int is_cmd = 1;
    for (int i = 0; i < wstart; i++)
        if (!isspace((unsigned char)buf[i])) { is_cmd = 0; break; }

    char **all = NULL; int n = 0, cap = 0;
    if (is_cmd) {
        for (int i = 0; COMMANDS[i]; i++) add_cand(&all, &n, &cap, COMMANDS[i]);
    } else {
        NamedTree *t = ws_active(ws);
        DiagList e, w; JoinList j;
        diag_init(&e); diag_init(&w);
        Resolution *r = tree_build(t, &j, &e, &w);
        if (r->root) collect_nodes(r->root, &all, &n, &cap);
        else for (int i = 0; i < r->n_leaves; i++) add_cand(&all, &n, &cap, r->leaves[i]->name);
        resolution_free(r); joinlist_free(&j); diag_free(&e); diag_free(&w);
    }

    int wlen = wend - wstart;
    const char *word = buf + wstart;
    char **match = NULL; int m = 0, mc = 0;
    for (int i = 0; i < n; i++) {
        if ((int)strlen(all[i]) >= wlen && strncmp(all[i], word, (size_t)wlen) == 0) {
            if (m == mc) { mc = mc ? mc * 2 : 8; match = xrealloc(match, (size_t)mc * sizeof(char *)); }
            match[m++] = all[i];                 /* transfer ownership */
        } else {
            free(all[i]);
        }
    }
    free(all);
    *out = match;
    return m;
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
        fputs("bpp-tree interactive mode. Type 'help' for commands, 'quit' to exit.\n"
              "Up/down arrows recall previous commands; Tab completes commands "
              "and clade names.\n",
              stdout);
    if (seed_joins && *seed_joins) show_status(ws_active(&ws));

    History hist = {0};
    int quit = 0;
    char *line;
    while (!quit && (line = line_edit("bpp-tree> ", &hist, repl_complete, &ws)) != NULL) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '\0' && *s != '#') {
            hist_push(&hist, s);
            handle_line(&ws, &hist, s, &quit);
        }
        free(line);
    }
    if (tty) fputc('\n', stdout);

    hist_free(&hist);
    for (int i = 0; i < ws.n; i++) tree_free(&ws.trees[i]);
    free(ws.trees);
    return 0;
}
