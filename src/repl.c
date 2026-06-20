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
#include "introgress.h"
#include "import.h"
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
    char     *imap_path;   /* attached Imap file, or NULL */
    MigList   mig;         /* migration bands (MSC-M) */
    IntroList intro;       /* introgression events (MSC-I); mutually exclusive */
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
    introlist_init(&t->intro);
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
    introlist_free(&t->intro);
}

static void tree_copy(NamedTree *dst, const NamedTree *src, const char *name)
{
    tree_init(dst, name);
    for (int i = 0; i < src->n_ops; i++)
        tree_add_op(dst, src->ops[i].kind, src->ops[i].spec);
    if (src->imap_path) dst->imap_path = xstrdup(src->imap_path);
    miglist_copy(&dst->mig, &src->mig);
    introlist_copy(&dst->intro, &src->intro);
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
        if (te.count == 0) {
            ok = 1;
            /* The edit went through; any introgression events whose endpoint
             * the edit removed are dropped (with a note). Mostly fires on
             * prune, but a move can also break a clade-label endpoint. */
            if (t->intro.count) {
                DiagList drops; diag_init(&drops);
                if (introlist_drop_orphans(&t->intro, r, &drops))
                    print_diags(&drops, "note");
                diag_free(&drops);
            }
        }
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
        IntroList intro_copy; introlist_copy(&intro_copy, &t->intro);
        introlist_apply(&intro_copy, r, &me);
        treenode_display(r->root, stdout, ascii, "");
        int color = treenode_use_color(ascii, stdout);
        if (t->mig.count)   { printf("\n"); migration_legend(&t->mig, r, stdout, color); }
        if (t->intro.count) { printf("\n"); introgress_legend(&intro_copy, r, stdout, color); }
        introlist_free(&intro_copy);
        print_diags(&me, "error");               /* any invalid bands/events */
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
        char *body;
        if (t->intro.count) {
            IntroList ic; introlist_copy(&ic, &t->intro);
            DiagList ie; diag_init(&ie);
            introlist_apply(&ic, r, &ie);            /* set labels, show_label */
            body = introgress_newick(&ic, r);
            introlist_free(&ic); diag_free(&ie);
        } else {
            body = treenode_to_newick(r->root);
        }
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

/* --- session persistence ------------------------------------------------ */

static char op_kind_char(OpKind k)
{
    switch (k) {
        case OP_JOIN:   return 'j';
        case OP_MOVE:   return 'm';
        case OP_GRAFT:  return 'g';
        case OP_PRUNE:  return 'p';
        default:        return 'r';   /* OP_ROTATE */
    }
}

/* Number of named (non-"main") trees — "main" is a scratch buffer. */
static int workspace_named(const Workspace *ws)
{
    int n = 0;
    for (int i = 0; i < ws->n; i++) if (strcmp(ws->trees[i].name, "main") != 0) n++;
    return n;
}

/* Save the named trees (not the scratch "main") as a small text image.
 * Returns the number written, or -1 if the file can't be opened. */
static int workspace_save(const Workspace *ws, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs("# bpp-tree session\n", f);
    int written = 0;
    for (int i = 0; i < ws->n; i++) {
        const NamedTree *t = &ws->trees[i];
        if (strcmp(t->name, "main") == 0) continue;       /* scratch: not saved */
        fprintf(f, "tree %s\n", t->name);
        for (int o = 0; o < t->n_ops; o++)
            fprintf(f, "%c %s\n", op_kind_char(t->ops[o].kind), t->ops[o].spec);
        if (t->imap_path) fprintf(f, "imap %s\n", t->imap_path);
        for (int b = 0; b < t->mig.count; b++)
            fprintf(f, "mig %s %s\n", t->mig.items[b].src, t->mig.items[b].dst);
        for (int k = 0; k < t->intro.count; k++) {
            const IntroEvent *e = &t->intro.items[k];
            fprintf(f, "intro %s %s %g %s %s %s\n",
                    e->donor, e->recip, e->phi,
                    e->src == TAU_NODE ? "node" : "branch",
                    e->dst == TAU_NODE ? "node" : "branch",
                    e->label ? e->label : "-");
        }
        written++;
    }
    fprintf(f, "active %s\n", ws->trees[ws->active].name);
    fclose(f);
    return written;
}

/* Replace the workspace with the image in `path`. Returns the number of trees
 * loaded, or -1 if the file can't be read. */
static int workspace_load(Workspace *ws, const char *path)
{
    char *txt = read_file_all(path);
    if (!txt) return -1;

    for (int i = 0; i < ws->n; i++) tree_free(&ws->trees[i]);
    ws->n = 0; ws->active = 0;

    char *active = NULL;
    NamedTree *cur = NULL;
    char *line = txt, *nl;
    while (*line) {
        nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char *s = xstrndup(line, len);
        char *p = s;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            char *kw = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            size_t kl = (size_t)(p - kw);
            while (*p == ' ' || *p == '\t') p++;          /* p = rest of line */
            char *re = p + strlen(p);
            while (re > p && (re[-1]==' '||re[-1]=='\t'||re[-1]=='\r')) *--re = '\0';
            #define KW(w) (kl == strlen(w) && strncmp(kw, w, kl) == 0)
            if (KW("tree")) {
                NamedTree t; tree_init(&t, p);
                cur = &ws->trees[ws_add(ws, t)];
            } else if (KW("active")) {
                free(active); active = xstrdup(p);
            } else if (KW("imap") && cur) {
                free(cur->imap_path); cur->imap_path = *p ? xstrdup(p) : NULL;
            } else if (KW("mig") && cur) {
                char *q = p; while (*q && *q != ' ' && *q != '\t') q++;
                if (*q) { *q++ = '\0'; while (*q==' '||*q=='\t') q++;
                          if (*q) miglist_add(&cur->mig, p, q); }
            } else if (KW("intro") && cur) {
                /* intro DONOR RECIP PHI SRC DST LABEL */
                char *tok[6] = {0}; int n = 0;
                char *q = p;
                while (n < 6 && *q) {
                    while (*q == ' ' || *q == '\t') q++;
                    if (!*q) break;
                    tok[n++] = q;
                    while (*q && *q != ' ' && *q != '\t') q++;
                    if (*q) { *q = '\0'; q++; }
                }
                if (n >= 5) {
                    IntroEvent ev; memset(&ev, 0, sizeof ev);
                    ev.donor = xstrdup(tok[0]); ev.recip = xstrdup(tok[1]);
                    ev.phi = atof(tok[2]); ev.phi2 = -1.0;
                    ev.src = strcmp(tok[3], "node") == 0 ? TAU_NODE : TAU_BRANCH;
                    ev.dst = strcmp(tok[4], "node") == 0 ? TAU_NODE : TAU_BRANCH;
                    ev.label = (n >= 6 && strcmp(tok[5], "-") != 0) ? xstrdup(tok[5]) : NULL;
                    if (cur->intro.count == cur->intro.cap) {
                        cur->intro.cap = cur->intro.cap ? cur->intro.cap * 2 : 4;
                        cur->intro.items = xrealloc(cur->intro.items,
                                                    (size_t)cur->intro.cap * sizeof(IntroEvent));
                    }
                    cur->intro.items[cur->intro.count++] = ev;
                }
            } else if (kl == 1 && cur && *p) {
                OpKind k; int ok = 1;
                switch (kw[0]) {
                    case 'j': k = OP_JOIN; break;  case 'm': k = OP_MOVE; break;
                    case 'g': k = OP_GRAFT; break; case 'p': k = OP_PRUNE; break;
                    case 'r': k = OP_ROTATE; break; default: ok = 0; k = OP_JOIN;
                }
                if (ok) tree_add_op(cur, k, p);
            }
            #undef KW
        }
        free(s);
        if (!nl) break;
        line = nl + 1;
    }
    free(txt);
    /* always keep a scratch "main" available */
    if (ws_find(ws, "main") < 0) { NamedTree t; tree_init(&t, "main"); ws_add(ws, t); }
    if (active) { int idx = ws_find(ws, active); if (idx >= 0) ws->active = idx; free(active); }
    return ws->n;
}

static const char *session_file(void)
{
    const char *p = getenv("BPPTREE_SESSION");
    return p ? p : ".bpptree";
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
    /* apply migration/introgression before the Newick so clade endpoints get
     * labelled and the eNewick form is emitted when there are introgressions */
    DiagList me; diag_init(&me);
    int migok = miglist_apply(&t->mig, r, &me);
    IntroList ic; introlist_copy(&ic, &t->intro);
    int introok = introlist_apply(&ic, r, &me);

    TreeNode **taxa = NULL; int nt = 0, tc = 0;
    treenode_collect_leaves(r->root, &taxa, &nt, &tc);
    char *body = introok && ic.count ? introgress_newick(&ic, r)
                                     : treenode_to_newick(r->root);
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
        if (ic.count)
            printf("\n(MSC-I network: add 'phiprior = a b' to the control file)\n");
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
    introlist_free(&ic);
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
"  read FILE [as NAME]  read a Newick / block / control file as a tree (NAME\n"
"                     creates or replaces a named tree; else replaces active)\n"
"  imap [FILE]        attach an Imap file (no arg: show; 'clear': detach)\n"
"  migration SRC->DST add an MSC-M migration band (no arg: list; 'clear';\n"
"                     'rm N': remove band N)\n"
"  introgress DONOR->RECIP [phi=P] [src=branch|node] [dst=branch|node]\n"
"                     add an MSC-I introgression event (no arg: list;\n"
"                     'clear'; 'rm N': remove event N). Mutually exclusive\n"
"                     with 'migration' on a given tree.\n"
"  hybrid H : A, C [phi=P] [src=...] [dst=...]\n"
"                     add a new hybrid species H with primary parent A and\n"
"                     secondary parent C; sugar for 'graft H->A' + introgression.\n"
"  status             taxa count, completeness, and any guidance\n"
"  taxa               list the tree's tips and the attached imap's species\n"
"  history            list the commands entered this session\n"
"  trees              list trees in memory (active marked '*')\n"
"  save NAME          save a copy of the active tree as NAME\n"
"  use NAME           make NAME the active tree\n"
"  new NAME           start a new empty tree named NAME and make it active\n"
"  drop NAME          delete the named tree\n"
"  session save|load [FILE]  save/load named trees (auto on a terminal)\n"
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
    if (IS("session")) {
        int save = strncmp(arg, "save", 4) == 0 && (arg[4]==' '||arg[4]=='\t'||arg[4]=='\0');
        int load = strncmp(arg, "load", 4) == 0 && (arg[4]==' '||arg[4]=='\t'||arg[4]=='\0');
        if (!save && !load) { printf("usage: session save|load [FILE]\n"); return; }
        const char *fa = arg + 4; while (*fa == ' ' || *fa == '\t') fa++;
        char *full = expand_tilde(*fa ? fa : session_file());
        if (save) {
            int nw = workspace_save(ws, full);
            if (nw < 0)       printf("cannot write '%s'\n", full);
            else if (nw == 0) printf("nothing to save (only 'main'; use 'save NAME' to keep a tree)\n");
            else              printf("saved %d tree(s) to '%s'\n", nw, full);
        } else {
            int n = workspace_load(ws, full);
            if (n < 0) printf("cannot read '%s'\n", full);
            else       printf("loaded session from '%s' (active: %s)\n", full, ws_active(ws)->name);
        }
        free(full);
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
        if (t->intro.count) {
            printf("error: this tree has introgression events (MSC-I); migration and "
                   "introgression are mutually exclusive — clear them first\n");
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
    if (IS("introgression") || IS("introgress") || IS("intro")) {
        NamedTree *t = ws_active(ws);
        if (!*arg) {                                       /* list */
            if (t->intro.count == 0) { printf("(no introgression events)\n"); return; }
            DiagList e, w, ie; JoinList j;
            diag_init(&e); diag_init(&w); diag_init(&ie);
            Resolution *r = tree_build(t, &j, &e, &w);
            IntroList ic; introlist_copy(&ic, &t->intro);
            introlist_apply(&ic, r, &ie);
            introgress_legend(&ic, r, stdout, treenode_use_color(0, stdout));
            introlist_free(&ic);
            resolution_free(r); joinlist_free(&j);
            diag_free(&e); diag_free(&w); diag_free(&ie);
            return;
        }
        if (strcmp(arg, "clear") == 0) {
            introlist_free(&t->intro);
            printf("introgression events cleared\n");
            return;
        }
        if (strncmp(arg, "rm", 2) == 0 && (arg[2] == ' ' || arg[2] == '\t')) {
            int idx = atoi(arg + 2);
            if (idx < 1 || idx > t->intro.count) { printf("no introgression event %d\n", idx); return; }
            free(t->intro.items[idx - 1].donor);
            free(t->intro.items[idx - 1].recip);
            free(t->intro.items[idx - 1].label);
            for (int k = idx - 1; k < t->intro.count - 1; k++)
                t->intro.items[k] = t->intro.items[k + 1];
            t->intro.count--;
            printf("removed introgression event %d\n", idx);
            return;
        }
        if (t->mig.count) {
            printf("error: this tree has migration bands (MSC-M); migration and "
                   "introgression are mutually exclusive — clear them first\n");
            return;
        }
        /* parse and validate against the current tree */
        IntroList tmp; introlist_init(&tmp);
        DiagList pe; diag_init(&pe);
        introlist_parse(&tmp, arg, &pe);
        if (pe.count || tmp.count != 1) {
            if (pe.count) print_diags(&pe, "error");
            else printf("usage: introgress DONOR->RECIP [phi=P] [src=branch|node] [dst=branch|node]\n");
        } else {
            DiagList e, w, ae; JoinList j;
            diag_init(&e); diag_init(&w); diag_init(&ae);
            Resolution *r = tree_build(t, &j, &e, &w);
            if (!r->root) {
                printf("the active tree isn't complete yet — finish it first\n");
            } else if (introlist_find_pair(&t->intro, tmp.items[0].donor, tmp.items[0].recip) >= 0) {
                printf("an introgression event already exists between '%s' and '%s'\n",
                       tmp.items[0].donor, tmp.items[0].recip);
            } else {
                /* validate against a merged list so the recipient-once rule fires */
                IntroList merged; introlist_copy(&merged, &t->intro);
                IntroEvent *src = &tmp.items[0];
                IntroEvent ev; memset(&ev, 0, sizeof ev);
                ev.donor = xstrdup(src->donor); ev.recip = xstrdup(src->recip);
                ev.phi = src->phi; ev.phi2 = src->phi2; ev.bidir = src->bidir;
                ev.src = src->src; ev.dst = src->dst;
                ev.label = src->label ? xstrdup(src->label) : NULL;
                if (merged.count == merged.cap) {
                    merged.cap = merged.cap ? merged.cap * 2 : 4;
                    merged.items = xrealloc(merged.items, (size_t)merged.cap * sizeof(IntroEvent));
                }
                merged.items[merged.count++] = ev;
                if (!introlist_apply(&merged, r, &ae)) {
                    print_diags(&ae, "error");
                } else {
                    /* commit a fresh copy of the validated new event into the tree */
                    if (t->intro.count == t->intro.cap) {
                        t->intro.cap = t->intro.cap ? t->intro.cap * 2 : 4;
                        t->intro.items = xrealloc(t->intro.items,
                                                  (size_t)t->intro.cap * sizeof(IntroEvent));
                    }
                    IntroEvent *commit = &t->intro.items[t->intro.count++];
                    memset(commit, 0, sizeof *commit);
                    commit->donor = xstrdup(src->donor);
                    commit->recip = xstrdup(src->recip);
                    commit->phi = src->phi; commit->phi2 = src->phi2;
                    commit->bidir = src->bidir;
                    commit->src = src->src; commit->dst = src->dst;
                    commit->label = src->label ? xstrdup(src->label) : NULL;
                    /* re-apply on the live tree to set labels and show_label */
                    DiagList junk; diag_init(&junk);
                    IntroList c2; introlist_copy(&c2, &t->intro);
                    introlist_apply(&c2, r, &junk);
                    printf("added introgression %s: %s \xe2\x87\x9d %s   phi=%g\n",
                           c2.items[c2.count - 1].label, src->donor, src->recip, src->phi);
                    introlist_free(&c2); diag_free(&junk);
                }
                introlist_free(&merged);
            }
            resolution_free(r); joinlist_free(&j);
            diag_free(&e); diag_free(&w); diag_free(&ae);
        }
        introlist_free(&tmp); diag_free(&pe);
        return;
    }
    if (IS("hybrid")) {
        /* hybrid H : A, C  phi=...  [src=...] [dst=...]
         * sugar for: graft H->A ; introgress C->H phi=... */
        if (!*arg) {
            printf("usage: hybrid H : PRIMARY, SECONDARY [phi=P] [src=...] [dst=...]\n");
            return;
        }
        char *spec = xstrdup(arg);
        char *colon = strchr(spec, ':');
        if (!colon) {
            printf("usage: hybrid H : PRIMARY, SECONDARY [phi=P] ...\n");
            free(spec); return;
        }
        *colon = '\0';
        char *H = trim(spec);
        char *rest = colon + 1;
        char *comma = strchr(rest, ',');
        if (!H || !*H || !comma) {
            printf("usage: hybrid H : PRIMARY, SECONDARY [phi=P] ...\n");
            free(spec); return;
        }
        *comma = '\0';
        char *primary = trim(rest);
        char *after = comma + 1;
        while (*after == ' ' || *after == '\t') after++;  /* start of secondary */
        char *end = after;
        while (*end && *end != ' ' && *end != '\t') end++;
        char saved = *end; *end = '\0';
        char *secondary = after;                          /* now NUL-terminated */
        char *opts = end + (saved ? 1 : 0);
        while (*opts == ' ' || *opts == '\t') opts++;
        if (!*primary || !*secondary) {
            printf("usage: hybrid H : PRIMARY, SECONDARY [phi=P] ...\n");
            free(spec); return;
        }
        /* graft H->primary, then introgress secondary->H opts */
        char *gspec = xasprintf("%s->%s", H, primary);
        char *ispec = xasprintf("%s->%s%s%s", secondary, H, *opts ? " " : "", opts);
        char gcmd[256], icmd[512];
        snprintf(gcmd, sizeof gcmd, "graft %s", gspec);
        snprintf(icmd, sizeof icmd, "introgress %s", ispec);
        free(gspec); free(ispec); free(spec);
        /* drive each via handle_line so the same validation paths apply */
        char gbuf[256]; snprintf(gbuf, sizeof gbuf, "%s", gcmd);
        char ibuf[512]; snprintf(ibuf, sizeof ibuf, "%s", icmd);
        handle_line(ws, hist, gbuf, quit);
        handle_line(ws, hist, ibuf, quit);
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
    if (IS("read") || IS("load")) {
        if (!*arg) { printf("usage: read FILE [as NAME]\n"); return; }
        /* split off an optional 'as NAME' suffix */
        char *spec = xstrdup(arg);
        char *asname = NULL;
        char *asw = strstr(spec, " as ");
        if (asw) { *asw = '\0'; asname = trim(asw + 4); }
        char *path = expand_tilde(trim(spec));
        DiagList ie; diag_init(&ie);
        Import imp; import_init(&imp);
        if (!import_read(path, &imp, &ie)) {
            print_diags(&ie, "error");
            import_free(&imp); diag_free(&ie); free(path); free(spec);
            return;
        }
        /* destination: a brand-new tree if 'as NAME' given (or 'main' is the
         * active and not empty), else replace the active scratch tree. */
        NamedTree *t = ws_active(ws);
        const char *dst_name = asname && *asname ? asname : t->name;
        if (asname && *asname) {
            int idx = ws_find(ws, dst_name);
            NamedTree nt; tree_init(&nt, dst_name);
            if (idx >= 0) { tree_free(&ws->trees[idx]); ws->trees[idx] = nt; ws->active = idx; }
            else          ws->active = ws_add(ws, nt);
            t = ws_active(ws);
        } else {
            for (int i = 0; i < t->n_ops; i++) free(t->ops[i].spec);
            t->n_ops = 0;
            miglist_free(&t->mig);
            introlist_free(&t->intro);
        }
        for (int i = 0; i < imp.n_joins; i++) tree_add_op(t, OP_JOIN, imp.joins[i]);
        miglist_copy(&t->mig, &imp.mig);
        introlist_copy(&t->intro, &imp.intro);
        printf("read '%s' into '%s': %d joins", path, dst_name, imp.n_joins);
        if (imp.mig.count)   printf(", %d migration band%s",
                                    imp.mig.count, imp.mig.count == 1 ? "" : "s");
        if (imp.intro.count) printf(", %d introgression event%s",
                                    imp.intro.count, imp.intro.count == 1 ? "" : "s");
        printf("\n");
        print_diags(&ie, "warning");
        import_free(&imp); diag_free(&ie); free(path); free(spec);
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
    "help", "quit", "exit", "display", "newick", "block", "read", "imap",
    "migration", "introgress", "hybrid", "taxa", "status", "trees", "history",
    "session", "save", "use", "new", "drop", "move", "graft", "prune",
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
        if (strcmp(cmd, "imap") == 0 || strcmp(cmd, "block") == 0 ||
            strcmp(cmd, "read") == 0 || strcmp(cmd, "load") == 0 ||
            strcmp(cmd, "session") == 0) {
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

    /* Restore a saved session at an interactive prompt, unless a tree was
     * supplied to seed the first tree. */
    if (tty && !(seed_joins && *seed_joins)) {
        FILE *sf = fopen(session_file(), "r");
        if (sf) {
            fclose(sf);
            int n = workspace_load(&ws, session_file());
            if (n >= 0)
                printf("restored %d tree(s) from '%s'\n", workspace_named(&ws), session_file());
        }
    }
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

    /* Offer to save named trees on exit (R-style), if any exist. */
    if (tty && workspace_named(&ws) > 0) {
        printf("Save %d named tree(s) to '%s'? [y/N] ", workspace_named(&ws), session_file());
        fflush(stdout);
        char *ans = NULL; size_t ac = 0;
        if (getline(&ans, &ac, stdin) > 0 && (ans[0] == 'y' || ans[0] == 'Y')) {
            if (workspace_save(&ws, session_file()) >= 0) printf("session saved.\n");
            else printf("could not write '%s'.\n", session_file());
        }
        free(ans);
    }

    hist_free(&hist);
    for (int i = 0; i < ws.n; i++) tree_free(&ws.trees[i]);
    free(ws.trees);
    return 0;
}
