#define _POSIX_C_SOURCE 200809L

#include "parser.h"
#include "tree.h"
#include "resolver.h"
#include "validate.h"
#include "imap.h"
#include "diag.h"
#include "util.h"
#include "json_writer.h"
#include "block.h"
#include "migrate.h"
#include "introgress.h"
#include "import.h"
#include "repl.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BPP_TREE_VERSION "0.1.0"   /* phase 1: binary trees */

typedef struct {
    int   json;
    int   json_indent;
    int   quiet;
    int   newick_only;
    int   validate_only;
    int   display;        /* --display */
    int   ascii;          /* --ascii */
    int   interactive;    /* -i / --interactive */
    char *joins_string;   /* --joins */
    char *imap_path;      /* --imap */
    char *out_prefix;     /* --out */
    char *rotate_spec;    /* --rotate */
    char *move_spec;      /* --move */
    char *graft_spec;     /* --graft */
    char *prune_spec;     /* --prune */
    char *migration_spec; /* --migration */
    char *introgression_spec; /* --introgression */
    char *read_file;      /* --read FILE: Newick / block / control file */
    char *joins_file;     /* positional */
} Options;

/* --- input ------------------------------------------------------------- */

static char *read_stream(FILE *fp)
{
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += r;
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap); }
    }
    buf[len] = '\0';
    return buf;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char *buf = read_stream(fp);
    fclose(fp);
    return buf;
}

/* --- output helpers ---------------------------------------------------- */

static char *newick_string(const TreeNode *root)
{
    char *body = treenode_to_newick(root);
    char *s = xasprintf("%s;", body);
    free(body);
    return s;
}

/* The species&tree Newick: an extended-Newick network if there are
 * introgression events, otherwise the plain tree. */
static char *species_newick(Resolution *r, const IntroList *intro)
{
    if (intro->count) {
        char *body = introgress_newick(intro, r);
        char *s = xasprintf("%s;", body);
        free(body);
        return s;
    }
    return newick_string(r->root);
}

/* The re-emitted species&tree Newick. A stacked MSC-I network carried as a
 * graph (imported, or constructed from join+introgression input -- the flat
 * event list cannot hold it) is serialised directly from the graph: faithful
 * and idempotent by construction. Everything else is built from the resolved
 * tree and its event list. */
static char *result_newick(const Graph *netgraph, Resolution *r, const IntroList *intro)
{
    if (netgraph) {
        char *body = graph_to_newick(netgraph);
        char *s = xasprintf("%s;", body);
        free(body);
        return s;
    }
    return species_newick(r, intro);
}

static void print_diag(FILE *fp, const Diagnostic *d, const char *kind)
{
    if (d->line_no >= 0)
        fprintf(fp, "bpp-tree: %s [%s] (line %d): %s\n", kind, d->code, d->line_no, d->message);
    else
        fprintf(fp, "bpp-tree: %s [%s]: %s\n", kind, d->code, d->message);
    if (d->hint)
        fprintf(fp, "    hint: %s\n", d->hint);
}

static void emit_json(FILE *fp, const Options *o, const Resolution *r,
                      TreeNode **taxa, int n_taxa, int n_joins,
                      const char *newick, const char *block,
                      int counts_filled, const int *counts, const MigList *mig,
                      const IntroList *intro,
                      const DiagList *errs, const DiagList *warns)
{
    JsonWriter w;
    jw_init(&w, fp, o->json_indent);
    jw_obj_open(&w);
    jw_kv_str(&w, "bpp_tree_version", BPP_TREE_VERSION);
    jw_kv_str(&w, "status", errs->count ? "error" : "ok");

    if (!errs->count) {
        jw_kv_int(&w, "n_taxa", n_taxa);
        jw_kv_int(&w, "n_joins", n_joins);
        jw_key(&w, "taxa");
        jw_arr_open(&w);
        for (int i = 0; i < r->n_leaves; i++) jw_str(&w, r->leaves[i]->name);
        jw_arr_close(&w);
        jw_kv_str(&w, "newick", newick);
        jw_kv_str(&w, "species_and_tree_block", block);
        jw_kv_bool(&w, "individual_counts_filled", counts_filled);
        if (counts_filled) {
            jw_key(&w, "species_counts");
            jw_obj_open(&w);
            for (int i = 0; i < n_taxa; i++) jw_kv_int(&w, taxa[i]->name, counts[i]);
            jw_obj_close(&w);
        }
        jw_key(&w, "migration");
        jw_arr_open(&w);
        for (int i = 0; i < mig->count; i++) {
            TreeNode *s = resolution_find(r, mig->items[i].src);
            TreeNode *d = resolution_find(r, mig->items[i].dst);
            jw_obj_open(&w);
            jw_kv_str(&w, "source", s ? treenode_bpp_name(s) : mig->items[i].src);
            jw_kv_str(&w, "target", d ? treenode_bpp_name(d) : mig->items[i].dst);
            jw_obj_close(&w);
        }
        jw_arr_close(&w);
        jw_key(&w, "introgression");
        jw_arr_open(&w);
        for (int i = 0; i < intro->count; i++) {
            const IntroEvent *e = &intro->items[i];
            TreeNode *D = resolution_find(r, e->donor);
            TreeNode *R = resolution_find(r, e->recip);
            jw_obj_open(&w);
            jw_kv_str(&w, "label", e->label ? e->label : "");
            jw_kv_str(&w, "donor", D ? treenode_bpp_name(D) : e->donor);
            jw_kv_str(&w, "recipient", R ? treenode_bpp_name(R) : e->recip);
            jw_kv_dbl(&w, "phi", e->phi);
            jw_kv_str(&w, "src", e->src == TAU_NODE ? "node" : "branch");
            jw_kv_str(&w, "dst", e->dst == TAU_NODE ? "node" : "branch");
            jw_obj_close(&w);
        }
        jw_arr_close(&w);
    }

    jw_key(&w, "warnings");
    jw_arr_open(&w);
    for (int i = 0; i < warns->count; i++) {
        jw_obj_open(&w);
        jw_kv_str(&w, "code", warns->items[i].code);
        if (warns->items[i].line_no >= 0) jw_kv_int(&w, "line", warns->items[i].line_no);
        else jw_kv_null(&w, "line");
        jw_kv_str(&w, "message", warns->items[i].message);
        jw_kv_str(&w, "hint", warns->items[i].hint);
        jw_obj_close(&w);
    }
    jw_arr_close(&w);

    jw_key(&w, "errors");
    jw_arr_open(&w);
    for (int i = 0; i < errs->count; i++) {
        jw_obj_open(&w);
        jw_kv_str(&w, "code", errs->items[i].code);
        if (errs->items[i].line_no >= 0) jw_kv_int(&w, "line", errs->items[i].line_no);
        else jw_kv_null(&w, "line");
        jw_kv_str(&w, "message", errs->items[i].message);
        jw_kv_str(&w, "hint", errs->items[i].hint);
        jw_obj_close(&w);
    }
    jw_arr_close(&w);

    jw_obj_close(&w);
    jw_finish(&w);
}

static void usage(FILE *fp)
{
    fputs(
"Usage: bpp-tree [options] [JOINS_FILE]\n"
"\n"
"Compile a join-formula into a BPP species tree. Reads JOINS_FILE, or\n"
"stdin if omitted, or the --joins string.\n"
"\n"
"General:\n"
"  -h, --help            Show this help and exit\n"
"      --version         Show version and exit\n"
"  -i, --interactive     Start interactive mode (a workspace of named trees)\n"
"      --json            Output JSON instead of human-readable text\n"
"      --json-indent N   JSON indentation width [2]\n"
"      --quiet           Suppress warnings/progress on stderr\n"
"\n"
"Input:\n"
"      --joins STRING    Join formula as a ',' or ';' separated string\n"
"      --read FILE       Read a tree from FILE: a Newick (or extended-Newick\n"
"                        for MSC-I), a BPP species&tree block, or a control\n"
"                        file containing one. Migration and introgression\n"
"                        are recovered from the file too.\n"
"      --imap FILE       Imap file; fills individual counts automatically\n"
"\n"
"Output:\n"
"      --display         Also print the tree as an indented branching diagram\n"
"      --ascii           With --display, use ASCII connectors (no Unicode)\n"
"      --move LIST       Prune-and-regraft moves 'SRC->DST' (',' or ';'\n"
"                        separated, applied in order): detach clade SRC and\n"
"                        regraft it as the sister of DST.\n"
"      --graft LIST      Add 'NEW->DST': a new tip NEW, or a subtree built from\n"
"                        a parenthesised join-formula '(A+B; C+D)->DST', as\n"
"                        the sister of DST.\n"
"      --prune LIST      Remove tips/subtrees (',' or ';' separated clade or\n"
"                        tip names); the parent is suppressed.\n"
"      --migration LIST  MSC-M migration bands 'SRC->DST' (',' or ';' separated):\n"
"                        gene flow from branch SRC to branch DST.\n"
"      --introgression LIST  MSC-I introgression events (',' or ';' separated),\n"
"                        each 'DONOR->RECIP [= NAME] [phi=P] [src=branch|node]\n"
"                        [dst=branch|node]'. Emits an extended-Newick network.\n"
"                        Repeating a recipient stacks pulses on one lineage;\n"
"                        an endpoint may name a prior event. '= NAME' names the\n"
"                        event (auto H1,H2,..). Mutually exclusive with --migration.\n"
"      --rotate LIST     Reverse the children of each named clade (',' or ';'\n"
"                        separated; a leaf-set label like 'A_B' or an explicit\n"
"                        label). Tips are ignored. Changes order, not topology.\n"
"      --out PREFIX      Write PREFIX.nwk and PREFIX.stree\n"
"      --newick-only     Print only the Newick string\n"
"      --validate        Validate only; exit 0 if valid, 1 if errors\n"
"\n"
"Exit codes: 0 valid, 1 join-formula errors, 2 system error.\n"
"Phase 1: binary trees only (polytomies are reported as errors).\n",
        fp);
}

int main(int argc, char **argv)
{
    Options o = { .json_indent = 2 };

    enum { OPT_VERSION = 1000, OPT_JSON, OPT_INDENT, OPT_JOINS, OPT_IMAP,
           OPT_OUT, OPT_NEWICK, OPT_VALIDATE, OPT_QUIET, OPT_ROTATE, OPT_MOVE,
           OPT_GRAFT, OPT_PRUNE, OPT_MIGRATION, OPT_INTROGRESSION,
           OPT_READ, OPT_DISPLAY, OPT_ASCII };
    static struct option lo[] = {
        {"help",        no_argument,       0, 'h'},
        {"interactive", no_argument,       0, 'i'},
        {"version",     no_argument,       0, OPT_VERSION},
        {"json",        no_argument,       0, OPT_JSON},
        {"json-indent", required_argument, 0, OPT_INDENT},
        {"quiet",       no_argument,       0, OPT_QUIET},
        {"joins",       required_argument, 0, OPT_JOINS},
        {"imap",        required_argument, 0, OPT_IMAP},
        {"out",         required_argument, 0, OPT_OUT},
        {"move",        required_argument, 0, OPT_MOVE},
        {"graft",       required_argument, 0, OPT_GRAFT},
        {"prune",       required_argument, 0, OPT_PRUNE},
        {"migration",   required_argument, 0, OPT_MIGRATION},
        {"introgression", required_argument, 0, OPT_INTROGRESSION},
        {"read",        required_argument, 0, OPT_READ},
        {"rotate",      required_argument, 0, OPT_ROTATE},
        {"display",     no_argument,       0, OPT_DISPLAY},
        {"ascii",       no_argument,       0, OPT_ASCII},
        {"newick-only", no_argument,       0, OPT_NEWICK},
        {"validate",    no_argument,       0, OPT_VALIDATE},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "hi", lo, NULL)) != -1) {
        switch (c) {
            case 'h':          usage(stdout); return 0;
            case 'i':          o.interactive = 1; break;
            case OPT_VERSION:  printf("bpp-tree %s\n", BPP_TREE_VERSION); return 0;
            case OPT_JSON:     o.json = 1; break;
            case OPT_INDENT:   o.json_indent = atoi(optarg); break;
            case OPT_QUIET:    o.quiet = 1; break;
            case OPT_JOINS:    o.joins_string = optarg; break;
            case OPT_IMAP:     o.imap_path = optarg; break;
            case OPT_OUT:      o.out_prefix = optarg; break;
            case OPT_MOVE:     o.move_spec = optarg; break;
            case OPT_GRAFT:    o.graft_spec = optarg; break;
            case OPT_PRUNE:    o.prune_spec = optarg; break;
            case OPT_MIGRATION: o.migration_spec = optarg; break;
            case OPT_INTROGRESSION: o.introgression_spec = optarg; break;
            case OPT_READ:     o.read_file = optarg; break;
            case OPT_ROTATE:   o.rotate_spec = optarg; break;
            case OPT_DISPLAY:  o.display = 1; break;
            case OPT_ASCII:    o.ascii = 1; break;
            case OPT_NEWICK:   o.newick_only = 1; break;
            case OPT_VALIDATE: o.validate_only = 1; break;
            default:           usage(stderr); return 2;
        }
    }
    if (optind < argc) o.joins_file = argv[optind];

    /* Interactive mode: optionally seed the first tree from a file or string. */
    if (o.interactive) {
        char *seed = NULL, *owned = NULL;
        if (o.joins_string) {
            seed = o.joins_string;
        } else if (o.joins_file) {
            owned = read_file(o.joins_file);
            if (!owned) { fprintf(stderr, "bpp-tree: cannot open '%s'\n", o.joins_file); return 2; }
            seed = owned;
        }
        int rc = repl_run(seed);
        free(owned);
        return rc;
    }

    /* Catch the common mistake of an unquoted --joins string: the shell
     * splits 'A+B, A_B+C' into a --joins value plus stray arguments. */
    if (o.joins_string && o.joins_file) {
        fprintf(stderr,
            "bpp-tree: both --joins and a separate argument ('%s') were given; "
            "only one input is allowed.\n", o.joins_file);
        fprintf(stderr,
            "  hint: quote the --joins string so spaces don't split it, e.g.\n"
            "          --joins 'A+B, A_B+C'   (or remove the spaces: A+B,A_B+C)\n");
        return 2;
    }
    if (!o.joins_string && argc - optind > 1) {
        fprintf(stderr,
            "bpp-tree: too many arguments; expected a single JOINS_FILE.\n"
            "  hint: to give joins on the command line, use a quoted --joins "
            "string, e.g.  --joins 'A+B,C+D'\n");
        return 2;
    }

    /* --- gather input ------------------------------------------------- */
    DiagList errs, warns;
    diag_init(&errs);
    diag_init(&warns);

    JoinList joins;
    joinlist_init(&joins);

    int syntax_errs = 0;
    Import imp; import_init(&imp);
    if (o.read_file) {
        if (!import_read(o.read_file, &imp, &errs)) syntax_errs = 1;
        else {
            for (int i = 0; i < imp.n_joins; i++)
                parse_joins_string(imp.joins[i], &joins, &errs);
        }
    } else if (o.joins_string) {
        syntax_errs = parse_joins_string(o.joins_string, &joins, &errs);
    } else {
        char *text;
        if (o.joins_file) {
            text = read_file(o.joins_file);
            if (!text) {
                fprintf(stderr, "bpp-tree: cannot open '%s'\n", o.joins_file);
                joinlist_free(&joins); diag_free(&errs); diag_free(&warns);
                return 2;
            }
        } else {
            /* No input source given. Read piped/redirected stdin, but at an
             * interactive terminal show usage instead of blocking silently. */
            if (isatty(STDIN_FILENO)) {
                usage(stderr);
                joinlist_free(&joins); diag_free(&errs); diag_free(&warns);
                return 2;
            }
            text = read_stream(stdin);
        }
        syntax_errs = parse_joins_text(text, &joins, &errs);
        free(text);
    }

    Imap *imap = NULL;
    if (o.imap_path) {
        imap = imap_read(o.imap_path, &errs);
        if (!imap) {  /* system error */
            for (int i = 0; i < errs.count; i++) print_diag(stderr, &errs.items[i], "error");
            joinlist_free(&joins); diag_free(&errs); diag_free(&warns);
            return 2;
        }
    }

    /* --- analyse ------------------------------------------------------ */
    Resolution *r = NULL;
    int n_joins = 0;
    MigList mig; miglist_init(&mig);
    IntroList intro; introlist_init(&intro);
    Graph *cgraph = NULL;          /* network built from join+introgression input */
    /* --read seeds migration and introgression from the file's own blocks */
    if (o.read_file) {
        miglist_copy(&mig, &imp.mig);
        introlist_copy(&intro, &imp.intro);
    }

    if (syntax_errs == 0) {
        r = resolve_tree(&joins, &errs);
        validate_joins(&joins, r, &errs);
        validate_tree(&joins, r, &errs, &warns);
        /* transforms apply to the built tree: moves, grafts, then rotations */
        if (o.move_spec && !errs.count)
            resolution_move(r, o.move_spec, &errs, &warns);
        if (o.graft_spec && !errs.count)
            resolution_graft(r, o.graft_spec, &errs, &warns);
        if (o.prune_spec && !errs.count)
            resolution_prune(r, o.prune_spec, &errs, &warns);
        if (o.rotate_spec && !errs.count)
            resolution_rotate(r, o.rotate_spec, &errs, &warns);
        /* a tree carries migration OR introgression, never both */
        if (o.migration_spec && o.introgression_spec && !errs.count)
            diag_add(&errs, DIAG_MODEL_CONFLICT, -1,
                "a tree may have migration (MSC-M) or introgression (MSC-I), not both.");
        /* migration bands annotate the (final) tree for display and output */
        if (o.migration_spec && !errs.count && r->root) {
            miglist_parse(&mig, o.migration_spec, &errs);
        }
        if (mig.count && !errs.count && r->root)
            miglist_apply(&mig, r, &errs);
        /* introgression events turn the species tree into an eNewick network */
        if (o.introgression_spec && !errs.count && r->root) {
            introlist_parse(&intro, o.introgression_spec, &errs);
        }
        /* A spec that stacks (a recipient used twice, or an endpoint naming a
         * prior event) is built as a graph; the flat introlist_apply cannot
         * represent it. The display list is then re-derived from the graph. */
        if (o.introgression_spec && !errs.count && r->root && introlist_needs_graph(&intro)) {
            cgraph = graph_construct(r, &intro, 1, &errs);
            if (cgraph) {
                introlist_free(&intro); introlist_init(&intro);
                introlist_from_graph(&intro, cgraph, r);
            }
        } else if (intro.count && !errs.count && r->root && !imp.graph_only) {
            introlist_apply(&intro, r, &errs);
        }
        /* An imported MSC-I network is carried as a graph: its events are already
         * in `intro` (copied from imp.intro). Re-emission comes from the graph,
         * so here we only mark the base tree for display -- and, if the tree was
         * edited, re-pin the events by rebuilding the graph on the edited tree. */
        if (imp.graph_only && !errs.count && r && r->root) {
            int edited = o.move_spec || o.graft_spec || o.prune_spec || o.rotate_spec;
            if (edited) {
                /* an edit that removed an endpoint orphans its event: drop it
                 * with a warning so the edit goes through (the read is not
                 * refused; the introgression on the missing branch is gone). */
                IntroList ev; introlist_init(&ev);
                introlist_events(&ev, imp.graph);
                introlist_drop_orphans(&ev, r, &warns);
                cgraph = ev.count ? graph_construct(r, &ev, 0, &errs) : NULL;
                introlist_free(&ev);
                introlist_free(&intro); introlist_init(&intro);
                if (cgraph) introlist_from_graph(&intro, cgraph, r);
                else        imp.graph_only = 0;   /* no events left -- plain tree */
            } else {
                introlist_mark(&intro, r);   /* intro already holds the imported events */
            }
        }
        /* internal nodes of the resulting tree (includes auto-created ones) */
        if (r->root) n_joins = treenode_count_internal(r->root);
    }

    /* Tree-shape warnings are moot once the formula has errors. */
    if (errs.count) { diag_free(&warns); diag_init(&warns); }

    int rc = errs.count ? 1 : 0;

    /* --- output ------------------------------------------------------- */
    if (o.json) {
        TreeNode **taxa = NULL; int n_taxa = 0, tcap = 0;
        char *newick = NULL, *block = NULL; int filled = 0; int *counts = NULL;
        if (!errs.count && r && r->root) {
            treenode_collect_leaves(r->root, &taxa, &n_taxa, &tcap);
            newick = result_newick(cgraph ? cgraph : (imp.graph_only ? imp.graph : NULL), r, &intro);
            block = species_block(taxa, n_taxa, newick, imap, &filled, &counts);
        }
        emit_json(stdout, &o, r, taxa, n_taxa, n_joins, newick, block,
                  filled, counts, &mig, &intro, &errs, &warns);
        free(taxa); free(newick); free(block); free(counts);
    } else {
        /* errors → stderr, no output */
        for (int i = 0; i < errs.count; i++) print_diag(stderr, &errs.items[i], "error");
        if (!o.quiet)
            for (int i = 0; i < warns.count; i++) print_diag(stderr, &warns.items[i], "warning");

        if (!errs.count && r && r->root) {
            TreeNode **taxa = NULL; int n_taxa = 0, tcap = 0;
            treenode_collect_leaves(r->root, &taxa, &n_taxa, &tcap);
            char *newick = result_newick(cgraph ? cgraph : (imp.graph_only ? imp.graph : NULL), r, &intro);
            int filled = 0; int *counts = NULL;
            char *block = species_block(taxa, n_taxa, newick, imap, &filled, &counts);
            char *migblk = mig.count ? migration_block(&mig, r) : NULL;
            int color = treenode_use_color(o.ascii, stdout);

            if (o.newick_only) {
                printf("%s\n", newick);
            } else if (o.validate_only) {
                if (!o.quiet)
                    printf("bpp-tree: valid (%d taxa, %d joins)\n", n_taxa, n_joins);
            } else {
                printf("bpp-tree: %d taxa, %d joins, valid\n\n", n_taxa, n_joins);
                printf("Newick:\n  %s\n\n", newick);
                if (o.display) {
                    printf("Tree:\n");
                    treenode_display(r->root, stdout, o.ascii, "  ");
                    if (mig.count) { printf("\n"); migration_legend(&mig, r, stdout, color); }
                    if (intro.count) { printf("\n"); introgress_legend(&intro, r, stdout, color); }
                    printf("\n");
                }
                printf("BPP species&tree block:\n");
                /* indent the block by 2 spaces on its first line for display */
                printf("  %s\n", block);
                if (migblk) printf("\n%s\n", migblk);
                if (intro.count)
                    printf("\nNote: this is an MSC-I network; add 'phiprior = a b' "
                           "(Beta prior) to the control file.\n");
                if (!filled)
                    printf("\nNote: replace '?' with the number of sequences per "
                           "species from your Imap file.\n");
            }

            /* --display alongside --newick-only / --validate: append the diagram */
            if (o.display && (o.newick_only || o.validate_only)) {
                printf("Tree:\n");
                treenode_display(r->root, stdout, o.ascii, "  ");
                if (mig.count) { printf("\n"); migration_legend(&mig, r, stdout, color); }
                if (intro.count) { printf("\n"); introgress_legend(&intro, r, stdout, color); }
            }

            if (o.out_prefix && !o.validate_only) {
                char *nwk = xasprintf("%s.nwk", o.out_prefix);
                char *str = xasprintf("%s.stree", o.out_prefix);
                FILE *fn = fopen(nwk, "w");
                FILE *fs = fopen(str, "w");
                if (!fn || !fs) {
                    fprintf(stderr, "bpp-tree: cannot write output files with prefix '%s'\n",
                            o.out_prefix);
                    rc = 2;
                } else {
                    fprintf(fn, "%s\n", newick);
                    fprintf(fs, "%s\n", block);
                    if (migblk) fprintf(fs, "\n%s\n", migblk);
                }
                if (fn) fclose(fn);
                if (fs) fclose(fs);
                free(nwk); free(str);
            }

            free(taxa); free(newick); free(block); free(counts); free(migblk);
        }
    }

    miglist_free(&mig);
    introlist_free(&intro);
    graph_free(cgraph);
    import_free(&imp);
    resolution_free(r);
    joinlist_free(&joins);
    imap_free(imap);
    diag_free(&errs);
    diag_free(&warns);
    return rc;
}
