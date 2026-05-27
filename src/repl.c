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
#include "migrate.h"
#include "lineedit.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A tree is stored as the operations that build it: joins (declarative, an
 * order-free set) plus moves/rotations (applied in order afterwards). The
 * resolved tree is recomputed on demand. */
typedef enum { OP_JOIN, OP_MOVE, OP_ROTATE, OP_GRAFT, OP_PRUNE } OpKind;

typedef struct { OpKind kind; char *spec; } Op;

typedef struct {
    char   *name;
    Op     *ops;
    int     n_ops, cap;
    char   *imap_path;   /* attached Imap file, or NULL */
    MigList mig;         /* migration bands */
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
    miglist_init(&t->mig);
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
    miglist_free(&t->mig);
}

static void tree_copy(NamedTree *dst, const NamedTree *src, const char *name)
{
    tree_init(dst, name);
    for (int i = 0; i < src->n_ops; i++)
        tree_add_op(dst, src->ops[i].kind, src->ops[i].spec);
    if (src->imap_path) dst->imap_path = xstrdup(src->imap_path);
    miglist_copy(&dst->mig, &src->mig);
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
        DiagList me; diag_init(&me);
        miglist_apply(&t->mig, r, &me);          /* annotate nodes; collect errors */
        treenode_display(r->root, stdout, ascii, "");
        if (t->mig.count) {
            printf("\n");
            migration_legend(&t->mig, r, stdout, treenode_use_color(ascii, stdout));
        }
        print_diags(&me, "error");               /* any invalid bands */
        diag_free(&me);
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

/* Expand a leading '~' (or '~/') to $HOME; otherwise copy the path. */
static char *expand_tilde(const char *path)
{
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) return xasprintf("%s%s", home, path + 1);
    }
    return xstrdup(path);
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
    /* apply migration before the Newick so clade endpoints get labelled */
    DiagList me; diag_init(&me);
    int migok = miglist_apply(&t->mig, r, &me);

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
    char *migblk = (migok && t->mig.count) ? migration_block(&t->mig, r) : NULL;

    while (*arg == ' ' || *arg == '\t') arg++;

    if (*arg == '\0') {                                 /* print to stdout */
        printf("%s\n", block);
        if (migblk) printf("\n%s\n", migblk);
        print_diags(&me, "error");                      /* invalid bands, if any */
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
        const char *farg = arg + 7;                     /* replace in a control file */
        while (*farg == ' ' || *farg == '\t') farg++;
        if (!*farg) {
            printf("usage: block replace FILE\n");
        } else {
            char *file = expand_tilde(farg);
            char *txt = read_file_all(file);
            if (!txt) {
                printf("cannot read '%s'\n", file);
            } else {
                const char *err = NULL;
                char *outc = control_replace_block(txt, block, &err);
                if (!outc) {
                    printf("%s\n", err);
                } else {
                    char *outm = control_replace_migration(outc, migblk);
                    free(outc); outc = outm;
                    char *bak = xasprintf("%s.bak", file);
                    FILE *fb = fopen(bak, "w");
                    if (fb) { fputs(txt, fb); fclose(fb); }
                    FILE *fo = fopen(file, "w");
                    if (!fo) { printf("cannot write '%s'\n", file); }
                    else { fputs(outc, fo); fclose(fo);
                           printf("replaced species&tree%s in '%s' (backup: %s)\n",
                                  migblk ? " and migration" : "", file, bak); }
                    free(bak); free(outc);
                }
                free(txt);
            }
            free(file);
        }
    } else {                                            /* write block to a file */
        char *file = expand_tilde(arg);
        FILE *fo = fopen(file, "w");
        if (!fo) printf("cannot write '%s'\n", file);
        else { fprintf(fo, "%s\n", block);
               if (migblk) fprintf(fo, "\n%s\n", migblk);
               fclose(fo);
               printf("wrote species&tree block to '%s'\n", file); }
        free(file);
    }

    free(taxa); free(nwk); free(block); free(counts); free(migblk); imap_free(m);
    diag_free(&me);
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

/* List the taxa associated with a tree: the tips in it, and the species in the
 * attached imap (the pool to build from), flagging any not yet used. */
static void cmd_taxa(const NamedTree *t)
{
    DiagList e, w; JoinList j;
    diag_init(&e); diag_init(&w);
    Resolution *r = tree_build(t, &j, &e, &w);

    printf("tips in tree (%d):", r->n_leaves);
    for (int i = 0; i < r->n_leaves; i++) printf(" %s", r->leaves[i]->name);
    printf("\n");

    if (t->imap_path) {
        DiagList ie; diag_init(&ie);
        Imap *m = imap_read(t->imap_path, &ie);
        if (!m) {
            print_diags(&ie, "error");
        } else {
            printf("imap species (%d):", m->count);
            for (int i = 0; i < m->count; i++) printf(" %s", m->items[i].species);
            printf("\n");
            int any = 0;
            for (int i = 0; i < m->count; i++) {
                int in = 0;
                for (int k = 0; k < r->n_leaves; k++)
                    if (strcmp(m->items[i].species, r->leaves[k]->name) == 0) { in = 1; break; }
                if (!in) { if (!any) { printf("not yet in tree:"); any = 1; } printf(" %s", m->items[i].species); }
            }
            if (any) printf("\n");
            imap_free(m);
        }
        diag_free(&ie);
    } else {
        printf("(no imap attached — 'imap FILE' adds a taxon pool)\n");
    }
    resolution_free(r); joinlist_free(&j); diag_free(&e); diag_free(&w);
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
"  migration SRC->DST add an MSC-M migration band (no arg: list; 'clear';\n"
"                     'rm N': remove band N)\n"
"  status             taxa count, completeness, and any guidance\n"
"  taxa               list the tree's tips and the attached imap's species\n"
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
    if (IS("taxa") || IS("species"))         { cmd_taxa(ws_active(ws)); return; }
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
        char *full = expand_tilde(arg);
        Imap *m = imap_read(full, &ie);         /* validate it opens/parses */
        if (!m) { print_diags(&ie, "error"); free(full); diag_free(&ie); return; }
        free(t->imap_path); t->imap_path = full;   /* store the expanded path */
        printf("imap attached to '%s': %s (%d species)\n", t->name, full, m->count);
        imap_free(m); diag_free(&ie);
        return;
    }
    if (IS("migration") || IS("mig")) {
        NamedTree *t = ws_active(ws);
        if (!*arg) {                                   /* list */
            if (t->mig.count == 0) { printf("(no migration bands)\n"); return; }
            DiagList e, w; JoinList j; diag_init(&e); diag_init(&w);
            Resolution *r = tree_build(t, &j, &e, &w);
            migration_legend(&t->mig, r, stdout, treenode_use_color(0, stdout));
            resolution_free(r); joinlist_free(&j); diag_free(&e); diag_free(&w);
            return;
        }
        if (strcmp(arg, "clear") == 0) {
            miglist_free(&t->mig);
            printf("migration bands cleared\n");
            return;
        }
        if (strncmp(arg, "rm", 2) == 0 && (arg[2] == ' ' || arg[2] == '\t')) {
            int idx = atoi(arg + 2);
            if (idx < 1 || idx > t->mig.count) { printf("no migration band %d\n", idx); return; }
            miglist_remove(&t->mig, idx - 1);
            printf("removed migration band %d\n", idx);
            return;
        }
        /* add SRC->DST, validated against the current tree */
        MigList tmp; miglist_init(&tmp);
        DiagList pe; diag_init(&pe);
        miglist_parse(&tmp, arg, &pe);
        if (pe.count || tmp.count != 1) {
            if (pe.count) print_diags(&pe, "error");
            else printf("usage: migration SRC->DST\n");
        } else {
            DiagList e, w, me; JoinList j;
            diag_init(&e); diag_init(&w); diag_init(&me);
            Resolution *r = tree_build(t, &j, &e, &w);
            if (!r->root) {
                printf("the active tree isn't complete yet — finish it first\n");
            } else if (miglist_find(&t->mig, tmp.items[0].src, tmp.items[0].dst) >= 0) {
                printf("that migration band already exists\n");
            } else if (!miglist_apply(&tmp, r, &me)) {
                print_diags(&me, "error");
            } else {
                miglist_add(&t->mig, tmp.items[0].src, tmp.items[0].dst);
                printf("added migration M%d: %s \xe2\x86\x92 %s\n",
                       t->mig.count, tmp.items[0].src, tmp.items[0].dst);
            }
            resolution_free(r); joinlist_free(&j);
            diag_free(&e); diag_free(&w); diag_free(&me);
        }
        miglist_free(&tmp); diag_free(&pe);
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
    "help", "quit", "exit", "display", "newick", "block", "imap", "migration",
    "taxa", "status", "trees", "history", "save", "use", "new", "drop", "move",
    "graft", "prune", "remove", "rotate",
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

/* species names from an attached imap file (so they complete before use). */
static void add_imap_species(const char *path, char ***arr, int *n, int *cap)
{
    if (!path) return;
    DiagList e; diag_init(&e);
    Imap *m = imap_read(path, &e);
    if (m) {
        for (int i = 0; i < m->count; i++) add_cand(arr, n, cap, m->items[i].species);
        imap_free(m);
    }
    diag_free(&e);
}

/* clade/tip names in a tree (built from its ops). */
static void add_tree_names(const NamedTree *t, char ***arr, int *n, int *cap)
{
    DiagList e, w; JoinList j;
    diag_init(&e); diag_init(&w);
    Resolution *r = tree_build(t, &j, &e, &w);
    if (r->root) collect_nodes(r->root, arr, n, cap);
    else for (int i = 0; i < r->n_leaves; i++) add_cand(arr, n, cap, r->leaves[i]->name);
    resolution_free(r); joinlist_free(&j); diag_free(&e); diag_free(&w);
}

/* filesystem path completions for `word` (each candidate is a full path that
 * starts with `word`; directories get a trailing '/'). */
static void add_path_candidates(const char *word, char ***arr, int *n, int *cap)
{
    const char *slash = strrchr(word, '/');
    char *dirpart = slash ? xstrndup(word, (size_t)(slash - word) + 1) : xstrdup("");
    const char *base = slash ? slash + 1 : word;
    char *fsdir = expand_tilde(dirpart);     /* '~' expanded for the filesystem */

    DIR *d = opendir(*fsdir ? fsdir : ".");
    if (d) {
        size_t blen = strlen(base);
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (strncmp(e->d_name, base, blen) != 0) continue;
            char *cand = xasprintf("%s%s", dirpart, e->d_name);   /* keeps '~' */
            char *real = xasprintf("%s%s", fsdir, e->d_name);
            struct stat st;
            if (stat(real, &st) == 0 && S_ISDIR(st.st_mode)) {
                char *cd = xasprintf("%s/", cand);
                add_cand(arr, n, cap, cd);
                free(cd);
            } else {
                add_cand(arr, n, cap, cand);
            }
            free(cand); free(real);
        }
        closedir(d);
    }
    free(dirpart); free(fsdir);
}

static int repl_complete(const char *buf, int wstart, int wend, char ***out, void *ctx)
{
    Workspace *ws = ctx;
    int is_cmd = 1;
    for (int i = 0; i < wstart; i++)
        if (!isspace((unsigned char)buf[i])) { is_cmd = 0; break; }

    char **all = NULL; int n = 0, cap = 0;
    NamedTree *t = ws_active(ws);
    const char *word = buf + wstart;
    int wlen = wend - wstart;

    if (is_cmd) {
        /* start of a line: commands, plus taxa (a line may begin a join) */
        for (int i = 0; COMMANDS[i]; i++) add_cand(&all, &n, &cap, COMMANDS[i]);
        add_tree_names(t, &all, &n, &cap);
        add_imap_species(t->imap_path, &all, &n, &cap);
    } else {
        /* an argument: candidate kind depends on the command */
        int b = 0; while (buf[b] == ' ' || buf[b] == '\t') b++;
        int we = b; while (buf[we] && buf[we] != ' ' && buf[we] != '\t') we++;
        char *cmd = xstrndup(buf + b, (size_t)(we - b));
        if (strcmp(cmd, "imap") == 0 || strcmp(cmd, "block") == 0) {
            add_path_candidates(word, &all, &n, &cap);          /* file paths */
        } else if (strcmp(cmd, "use") == 0 || strcmp(cmd, "switch") == 0 ||
                   strcmp(cmd, "drop") == 0 || strcmp(cmd, "delete") == 0 ||
                   strcmp(cmd, "save") == 0) {
            for (int i = 0; i < ws->n; i++) add_cand(&all, &n, &cap, ws->trees[i].name);
        } else {
            add_tree_names(t, &all, &n, &cap);                  /* clades/tips */
            add_imap_species(t->imap_path, &all, &n, &cap);
        }
        free(cmd);
    }

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
