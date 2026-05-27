#define _POSIX_C_SOURCE 200809L

#include "parser.h"
#include "tree.h"
#include "resolver.h"
#include "validate.h"
#include "imap.h"
#include "diag.h"
#include "util.h"
#include "json_writer.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPP_TREE_VERSION "0.1.0"   /* phase 1: binary trees */

typedef struct {
    int   json;
    int   json_indent;
    int   quiet;
    int   newick_only;
    int   validate_only;
    char *joins_string;   /* --joins */
    char *imap_path;      /* --imap */
    char *out_prefix;     /* --out */
    char *rotate_spec;    /* --rotate */
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

/* Build the BPP species&tree block. Sets *filled to 1 if all counts came
 * from the Imap, and returns the per-taxon counts via *counts_out (caller
 * frees). taxa[] are in Newick left-to-right order. */
static char *species_block(TreeNode **taxa, int n_taxa, const char *newick,
                           const Imap *imap, int *filled, int **counts_out)
{
    int *counts = xmalloc((size_t)(n_taxa ? n_taxa : 1) * sizeof(int));
    int all = (imap != NULL);
    for (int i = 0; i < n_taxa; i++) {
        counts[i] = imap ? imap_count_for(imap, taxa[i]->name) : -1;
        if (counts[i] < 0) all = 0;
    }
    *filled = all;

    /* line 1: "species&tree = N  n1  n2 ..." */
    char *out = xasprintf("species&tree = %d", n_taxa);
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = xasprintf("%s  %s", out, taxa[i]->name);
        free(out); out = tmp;
    }
    /* line 2: counts */
    { char *tmp = xasprintf("%s\n ", out); free(out); out = tmp; }
    for (int i = 0; i < n_taxa; i++) {
        char *tmp = (counts[i] >= 0) ? xasprintf("%s  %d", out, counts[i])
                                     : xasprintf("%s  ?", out);
        free(out); out = tmp;
    }
    /* line 3: newick */
    { char *tmp = xasprintf("%s\n  %s", out, newick); free(out); out = tmp; }

    *counts_out = counts;
    return out;
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
                      int counts_filled, const int *counts,
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
"      --json            Output JSON instead of human-readable text\n"
"      --json-indent N   JSON indentation width [2]\n"
"      --quiet           Suppress warnings/progress on stderr\n"
"\n"
"Input:\n"
"      --joins STRING    Join formula as a ',' or ';' separated string\n"
"      --imap FILE       Imap file; fills individual counts automatically\n"
"\n"
"Output:\n"
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
           OPT_OUT, OPT_NEWICK, OPT_VALIDATE, OPT_QUIET, OPT_ROTATE };
    static struct option lo[] = {
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, OPT_VERSION},
        {"json",        no_argument,       0, OPT_JSON},
        {"json-indent", required_argument, 0, OPT_INDENT},
        {"quiet",       no_argument,       0, OPT_QUIET},
        {"joins",       required_argument, 0, OPT_JOINS},
        {"imap",        required_argument, 0, OPT_IMAP},
        {"out",         required_argument, 0, OPT_OUT},
        {"rotate",      required_argument, 0, OPT_ROTATE},
        {"newick-only", no_argument,       0, OPT_NEWICK},
        {"validate",    no_argument,       0, OPT_VALIDATE},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
            case 'h':          usage(stdout); return 0;
            case OPT_VERSION:  printf("bpp-tree %s\n", BPP_TREE_VERSION); return 0;
            case OPT_JSON:     o.json = 1; break;
            case OPT_INDENT:   o.json_indent = atoi(optarg); break;
            case OPT_QUIET:    o.quiet = 1; break;
            case OPT_JOINS:    o.joins_string = optarg; break;
            case OPT_IMAP:     o.imap_path = optarg; break;
            case OPT_OUT:      o.out_prefix = optarg; break;
            case OPT_ROTATE:   o.rotate_spec = optarg; break;
            case OPT_NEWICK:   o.newick_only = 1; break;
            case OPT_VALIDATE: o.validate_only = 1; break;
            default:           usage(stderr); return 2;
        }
    }
    if (optind < argc) o.joins_file = argv[optind];

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
    if (o.joins_string) {
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

    if (syntax_errs == 0) {
        r = resolve_tree(&joins, &errs);
        validate_joins(&joins, r, &errs);
        validate_tree(&joins, r, &errs, &warns);
        /* apply node rotations to the resolved tree (output reflects them) */
        if (o.rotate_spec && !errs.count)
            resolution_rotate(r, o.rotate_spec, &errs, &warns);
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
            newick = newick_string(r->root);
            block = species_block(taxa, n_taxa, newick, imap, &filled, &counts);
        }
        emit_json(stdout, &o, r, taxa, n_taxa, n_joins, newick, block,
                  filled, counts, &errs, &warns);
        free(taxa); free(newick); free(block); free(counts);
    } else {
        /* errors → stderr, no output */
        for (int i = 0; i < errs.count; i++) print_diag(stderr, &errs.items[i], "error");
        if (!o.quiet)
            for (int i = 0; i < warns.count; i++) print_diag(stderr, &warns.items[i], "warning");

        if (!errs.count && r && r->root) {
            TreeNode **taxa = NULL; int n_taxa = 0, tcap = 0;
            treenode_collect_leaves(r->root, &taxa, &n_taxa, &tcap);
            char *newick = newick_string(r->root);
            int filled = 0; int *counts = NULL;
            char *block = species_block(taxa, n_taxa, newick, imap, &filled, &counts);

            if (o.newick_only) {
                printf("%s\n", newick);
            } else if (o.validate_only) {
                if (!o.quiet)
                    printf("bpp-tree: valid (%d taxa, %d joins)\n", n_taxa, n_joins);
            } else {
                printf("bpp-tree: %d taxa, %d joins, valid\n\n", n_taxa, n_joins);
                printf("Newick:\n  %s\n\n", newick);
                printf("BPP species&tree block:\n");
                /* indent the block by 2 spaces on its first line for display */
                printf("  %s\n", block);
                if (!filled)
                    printf("\nNote: replace '?' with the number of sequences per "
                           "species from your Imap file.\n");
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
                }
                if (fn) fclose(fn);
                if (fs) fclose(fs);
                free(nwk); free(str);
            }

            free(taxa); free(newick); free(block); free(counts);
        }
    }

    resolution_free(r);
    joinlist_free(&joins);
    imap_free(imap);
    diag_free(&errs);
    diag_free(&warns);
    return rc;
}
