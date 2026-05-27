# bpp-tree: Specification for Claude Code Development

## Overview

`bpp-tree` is a standalone C command-line tool that compiles a simple
human-readable join-formula into BPP-compatible tree specifications.
It accepts a `.joins` file (or stdin) describing a binary (or polytomous)
tree as a set of join operations, validates the joins, resolves them
order-independently, and outputs the BPP `species&tree` block plus a
Newick string.

The tool is designed to be used two ways:
1. **Directly by a researcher** who writes a `.joins` file by hand
2. **By the bpp-agent** which builds the join set from a conversation
   and calls `bpp-tree --json` to get structured output

The `.joins` format is the primary artifact — it is human-readable,
editable, self-documenting, and fully describes the tree. No session
state is maintained; editing the file and re-running is the intended
workflow for corrections.

---

## The Join Formula Syntax

### Basic join

```
A+B
```

Joins two taxa or previously-named clades. The result is a new clade
whose members are the union of the operands' leaves.

### Explicit label

```
A+B = hominini
```

Names the resulting clade `hominini`. This label can be used in
subsequent joins instead of the implicit name.

### Implicit label

When no `= label` is given, the implicit label is constructed by
sorting the constituent leaf names alphabetically and joining them
with `_`:

```
chimp+bonobo     → implicit label: bonobo_chimp
```

The `_` character is the reserved clade-label separator. See
"Naming rules and `_`" below.

### Polytomy

More than two operands on one join line:

```
A+B+C
```

Produces a true polytomy `(A,B,C)` — an unresolved node. BPP accepts
polytomies in the species tree for A11 and some other analyses.

### Comments and blank lines

Lines beginning with `#` (after optional leading whitespace) are
comments. Blank lines are ignored.

```
# outgroup
E+ABCD

# ingroup
A+B
C+D
AB+CD
```

### Order independence

Joins may appear in any order. The parser resolves dependencies
before building the tree, so:

```
AB+C        ← references AB before it is defined
A+B = AB    ← defines AB
```

is valid. The parser performs a topological sort of joins by their
dependencies. If a cycle exists, it is reported as an error.

---

## Naming Rules and `_`

The `_` character is reserved as the constituent-leaf separator in
implicit clade labels. This creates a potential ambiguity: if a
species is named `A_B`, it looks the same as the implicit label for
a clade joining `A` and `B`.

### Rules

1. A token is a **leaf** (species) if it never appears as the result
   of any join (either explicitly via `= label` or implicitly).

2. A token is a **clade label** if it appears as the result of some
   join.

3. If a species name contains `_`, the parser checks whether the
   name could be misinterpreted as an implicit clade label. If so,
   the parser emits an error and requires an explicit label for any
   join that would produce that implicit name.

### Example — conflict

```
# Species: A, B, A_B
A+B           ← implicit label would be A_B — conflicts with species A_B
A_B+C         ← ambiguous: is A_B the species or the clade?
```

Error:
```
Error: implicit label 'A_B' for join 'A+B' conflicts with species
       name 'A_B'.
  Fix: add an explicit label, e.g.:
         A+B = AB_clade
         AB_clade+C
```

### Recommendation

For datasets with species names containing `_`, use explicit labels
for all joins. The implicit label is a convenience for simple cases.

---

## Complete Example Files

### Small tree, implicit labels

```
C+D
A+B
AB+CD
E+AB_CD
```

Newick: `(E,((A,B),(C,D)));`

### Medium tree, explicit labels

```
# great apes
chimp+bonobo        = pan
pan+human           = hominini
gorilla+hominini    = homininae
orang+homininae     = hominidae

# lesser apes
gibbon+siamang      = hylobatidae

# all apes
hominidae+hylobatidae = apes

# with macaque outgroup
macaque+apes
```

Newick: `(macaque,(((orang,((gorilla,((chimp,bonobo),human)))),
         (gibbon,siamang))));`

### Polytomy

```
A+B+C = trichotomy
D+trichotomy
```

Newick: `(D,(A,B,C));`

---

## Validation: Error and Warning Conditions

The parser must detect and report all of the following. Every error
message must include the line number (or "inferred from context" for
errors that span lines) and a suggested fix.

### Errors (prevent output)

**UNDEFINED_OPERAND**
An operand in a join is not a known leaf and is not produced by any
other join.
```
A+X
# X is never defined
```
```
Error line 1: operand 'X' is not a known taxon and is not produced
by any join in this file.
  Hint: check the spelling, or add a join that produces 'X'.
```

**TAXON_JOINED_TWICE**
A leaf appears as an operand in more than one join.
```
A+B
A+C
```
```
Error: taxon 'A' appears in multiple joins (lines 1 and 2).
  Each taxon may only be joined once.
```

**CLADE_JOINED_TWICE**
A clade label (explicit or implicit) appears as an operand in more
than one join.
```
A+B = AB
AB+C
AB+D
```
```
Error: clade 'AB' appears in multiple joins (lines 2 and 3).
  Each clade may only be joined once.
```

**CYCLE**
The dependency graph of joins contains a cycle. This should not be
possible with valid join syntax but is checked defensively.
```
Error: dependency cycle detected involving clades: X, Y, Z.
  This is a parser error — please report it.
```

**DISCONNECTED**
After all joins are resolved, more than one clade remains unjoined.
These are the "roots" — a valid tree has exactly one.
```
A+B
C+D
# AB and CD are never joined
```
```
Error: tree is disconnected. 2 unjoined clades remain after all joins:
  'A_B'  (from line 1, leaves: A B)
  'C_D'  (from line 2, leaves: C D)
  Hint: add a join connecting these, e.g.:
         A_B+C_D
```
When more than two clades are disconnected, list all of them and
suggest a sequence of joins that would connect them.

**IMPLICIT_LABEL_CONFLICT**
The implicit label for a join conflicts with a species name.
(See "Naming rules and `_`" above.)

**DUPLICATE_LABEL**
Two joins assign the same explicit label.
```
A+B = clade1
C+D = clade1
```
```
Error: label 'clade1' is defined by more than one join (lines 1 and 2).
  Each label must be unique.
```

**EMPTY_FILE**
No join statements found (only comments/blanks).
```
Error: no join statements found. The file must contain at least one
join, e.g.:  A+B
```

**SINGLE_TAXON_TREE**
Only one taxon and no joins — not a valid species tree for BPP.
```
Error: a BPP species tree requires at least 2 taxa.
```

### Warnings (produce output but flag)

**SINGLE_JOIN_REMAINING**
After resolving all joins, the root clade is valid but there is only
one join total — the "tree" is a single node with two children. Valid
but worth confirming.

**POLYTOMY_PRESENT**
One or more joins have more than two operands. BPP accepts polytomies
for some analyses but not all.
```
Warning: polytomy detected at join on line N (operands: A B C).
  BPP accepts polytomies for A11 analysis. For A00/A01/A10, the
  tree must be fully resolved.
```

**LARGE_TREE**
More than 50 taxa. Valid but BPP analyses may be slow.
```
Warning: 73 taxa specified. BPP analyses with large species trees
  may require long MCMC runs.
```

**UNUSED_LABEL**
An explicit label is assigned but the clade is the root (never used
as an operand). Harmless but potentially a typo.
```
Warning line 4: label 'my_root' assigned to the root clade.
  Root labels are valid but not referenced by any further join.
```

---

## Resolution Algorithm

```
1. Parse all lines into a list of JoinStatement records:
     { operands: [string], label: string|null, line_no: int }

2. Identify leaves:
   A token is a leaf if it does not appear as the label (explicit
   or implicit) of any join.
   
   Compute implicit labels first:
     implicit_label(operands) = sort(leaves(operands)).join("_")
   where leaves(op) recursively collects all leaf descendants of op.
   But at parse time we don't know the full leaf set yet, so:
   
   Two-pass approach:
   Pass 1: collect all tokens that appear anywhere (operands + labels)
   Pass 2: tokens that appear only as operands (never as a result) 
           are leaves.

3. Build a dependency graph:
   Each join J depends on the joins that produce its operands
   (if an operand is a leaf, it has no dependency).

4. Topological sort:
   Use Kahn's algorithm (or DFS with cycle detection).
   If a cycle is found → CYCLE error.
   If an operand has no producing join → UNDEFINED_OPERAND error.

5. Validate:
   - Check for TAXON_JOINED_TWICE, CLADE_JOINED_TWICE
   - Check for DUPLICATE_LABEL
   - Check for IMPLICIT_LABEL_CONFLICT

6. Build tree bottom-up (in topological order):
   For each join (in resolved order):
     Create an internal node with children = resolved operands
     Register the result under its label

7. Identify root:
   The root is the unique clade that is never used as an operand.
   If zero such clades → cycle (caught in step 4).
   If more than one → DISCONNECTED error.

8. Convert root to Newick (recursive):
   node_to_newick(node):
     if node is leaf: return node.name
     return "(" + ",".join(node_to_newick(c) for c in children) + ")"
   Append ";" to the root.
```

---

## Output

### Human-readable (default)

```
bpp-tree: 8 taxa, 7 joins, valid

Newick:
  (macaque,(((orang,(gorilla,(chimp,bonobo,human))),
  (gibbon,siamang))));

BPP species&tree block:
  species&tree = 8  bonobo  chimp  gibbon  gorilla  human
                    macaque  orang  siamang
                    ?  ?  ?  ?  ?  ?  ?  ?
                   (macaque,(((orang,(gorilla,(((chimp,bonobo),human)))),
                   (gibbon,siamang))));

Note: replace '?' with the number of sequences per species from your
Imap file.
```

The taxa in the `species&tree` header are listed alphabetically.
Individual counts are left as `?` — these come from the Imap file
and are filled in by the agent or user. If an Imap file is provided
via `--imap`, the counts are filled in automatically.

### JSON output (--json)

```json
{
  "bpp_tree_version": "1.0.0",
  "status": "ok",
  "n_taxa": 8,
  "n_joins": 7,
  "taxa": ["bonobo","chimp","gibbon","gorilla","human",
           "macaque","orang","siamang"],
  "newick": "(macaque,(((orang,(gorilla,(chimp,bonobo,human))),(gibbon,siamang))));",
  "species_and_tree_block": "species&tree = 8  bonobo  chimp ...\n  ?  ?  ...\n  (macaque,...);",
  "individual_counts_filled": false,
  "warnings": [],
  "errors": []
}
```

When `--imap` is provided and all species are matched:

```json
{
  "individual_counts_filled": true,
  "species_counts": {
    "bonobo": 3, "chimp": 4, "gorilla": 2,
    "human": 6, "macaque": 2, "orang": 3,
    "gibbon": 2, "siamang": 1
  },
  "species_and_tree_block": "species&tree = 8  bonobo  chimp ...\n  3  4  2  6  2  3  2  1\n  (macaque,...);",
}
```

### Error output (JSON)

When errors are present, `status` is `"error"` and no Newick is
produced:

```json
{
  "status": "error",
  "errors": [
    {
      "code": "DISCONNECTED",
      "line": null,
      "message": "Tree is disconnected. 2 unjoined clades remain: 'A_B', 'C_D'.",
      "hint": "Add a join connecting these, e.g.: A_B+C_D"
    }
  ],
  "warnings": []
}
```

---

## Invocation

```
bpp-tree [options] [JOINS_FILE]
```

If `JOINS_FILE` is omitted, read from stdin.

### Options

```
General:
  -h, --help            Show help and exit
  --version             Show version and exit
  --json                Output JSON instead of human-readable text
  --json-indent N       JSON indentation width [2]
  --quiet               Suppress progress messages on stderr

Input:
  --joins STRING        Join formula as a comma-separated string,
                        e.g. --joins "A+B=AB,C+D,AB+C_D,E+AB_C_D"
                        Semicolons also accepted as separators.
                        Alternative to providing a file.
  --imap FILE           Imap file (sample → species mapping).
                        If provided, individual counts in the
                        species&tree block are filled in automatically.

Output:
  --out PREFIX          Write Newick to PREFIX.nwk and the
                        species&tree block to PREFIX.stree
                        (default: stdout only)
  --newick-only         Print only the Newick string, no BPP block
  --validate            Validate only; do not produce output files
                        Exit 0 if valid, 1 if errors present.
```

### Exit codes

```
0   Valid tree, output produced (or --validate with valid input)
1   Errors in the join formula (output suppressed)
2   System error (file not found, out of memory, etc.)
```

---

## File Format: `.joins`

The recommended extension is `.joins`. The format is plain text,
UTF-8, Unix or Windows line endings both accepted.

### Grammar (informal)

```
file      := line*
line      := blank | comment | join
blank     := whitespace* newline
comment   := whitespace* '#' anything newline
join      := operands ('=' label)? whitespace* comment? newline
operands  := token ('+' token)+
label     := token
token     := [A-Za-z0-9_-]+    (no spaces, no '+', no '=', no '#')
```

Token characters: letters, digits, underscore, hyphen. No spaces
within a token. The `+` and `=` characters are operators, `#` begins
a comment.

Note: `_` within a token is allowed — it is only special when it
appears as the separator in an **implicit label** (i.e., a label
derived by the parser from leaf names). Species names containing `_`
are valid; the parser detects and reports conflicts between such
names and implicit clade labels.

---

## Implementation Notes

### Language and dependencies

- C11, no external dependencies
- Link against nothing beyond libc and libm
- POSIX.1-2008 interfaces (`getopt_long`, etc.)
- Build with: `cc -std=c11 -Wall -Wextra -O2 -o bpp-tree main.c ...`

### Source structure

```
bpp-tree/
├── Makefile
├── README.md
└── src/
    ├── main.c          # CLI, dispatch, output formatting
    ├── parser.c / .h   # tokeniser, join statement parser
    ├── resolver.c / .h # dependency graph, topological sort
    ├── tree.c / .h     # tree node data structure, Newick serialiser
    ├── validate.c / .h # all error/warning checks
    ├── imap.c / .h     # optional Imap reader for individual counts
    └── json_writer.c/.h # minimal JSON output (same as bpp-seqs)
```

### Data structures

```c
/* A single parsed join statement */
typedef struct {
    char   **operands;   /* NULL-terminated array of operand tokens */
    int      n_operands;
    char    *label;      /* explicit label, or NULL */
    int      line_no;    /* source line number for error messages */
} JoinStmt;

/* A node in the resolved tree */
typedef struct TreeNode {
    char          *name;        /* leaf name, or clade label */
    char          *implicit_label; /* computed from leaf set */
    struct TreeNode **children;
    int             n_children;
    int             is_leaf;
    int             join_line;  /* line number that produced this node */
} TreeNode;

/* Resolved tree */
typedef struct {
    TreeNode  *root;
    TreeNode **leaves;     /* pointer array, alphabetical order */
    int        n_leaves;
    char     **all_labels; /* all label strings for dedup checking */
    int        n_labels;
} Tree;

/* Error / warning record */
typedef struct {
    char *code;
    int   line_no;    /* -1 if not line-specific */
    char *message;
    char *hint;       /* suggested fix, may be NULL */
} Diagnostic;

/* Full parse result */
typedef struct {
    Tree       *tree;          /* NULL if errors present */
    Diagnostic *errors;
    int         n_errors;
    Diagnostic *warnings;
    int         n_warnings;
} ParseResult;
```

### Topological sort

Use Kahn's algorithm:

```c
/* Build adjacency list: join J depends on join K if K produces
 * an operand of J. Leaves have no dependencies. */

/* In-degree array: n_deps[j] = number of joins j depends on */
/* Queue: joins with n_deps == 0 */
/* While queue not empty:
 *   pop join J
 *   add J to resolved order
 *   for each join K that depends on J:
 *     n_deps[K]--
 *     if n_deps[K] == 0: enqueue K
 * If resolved order != total joins: cycle exists
 */
```

### Newick serialisation

```c
/* Recursive, produces standard unrooted Newick.
 * Leaf names are quoted if they contain special characters
 * (though the grammar forbids them — quote defensively). */
static void node_to_newick(TreeNode *node, FILE *fp)
{
    if (node->is_leaf) {
        fprintf(fp, "%s", node->name);
        return;
    }
    fputc('(', fp);
    for (int i = 0; i < node->n_children; i++) {
        if (i > 0) fputc(',', fp);
        node_to_newick(node->children[i], fp);
    }
    fputc(')', fp);
}
/* Call: node_to_newick(root, fp); fprintf(fp, ";"); */
```

### BPP species&tree block

```
species&tree = N  sp1  sp2  sp3  ...
               c1  c2   c3  ...
               (newick);
```

- First line: `N` = number of species (taxa), then species names in
  the same order as they appear in the Newick (post-order left-to-right
  traversal of leaves). BPP requires this order to match.
- Second line: individual counts per species, same order. Filled from
  Imap if provided; otherwise `?` placeholders.
- Third line: Newick string followed by `;`.

Species name order in the block header follows the order they appear
as leaves in a post-order left-to-right traversal of the tree — this
matches BPP's expectation.

---

## Test Suite

`tests/run_tests.sh` using `set -uo pipefail` and `mktemp`/`trap`
cleanup, consistent with bpp-seqs tests.

### Required test cases

**Correctness (tests 1–15)**

t1: 2-taxon tree `A+B` → Newick `(A,B);` ✓

t2: 4-taxon balanced `A+B, C+D, A_B+C_D` → `((A,B),(C,D));` ✓

t3: 4-taxon pectinate `A+B, A_B+C, A_B_C+D` → `(((A,B),C),D);` ✓

t4: Order independence — joins given in reverse dependency order
    produce the same tree as forward order ✓

t5: Explicit labels — `A+B=AB, C+D=CD, AB+CD` → `((A,B),(C,D));` ✓

t6: Mixed implicit/explicit — `A+B=myAB, myAB+C` → `((A,B),C);` ✓

t7: Polytomy `A+B+C, A_B_C+D` → `((A,B,C),D);` ✓

t8: 8-taxon labelled tree (primate example) → correct Newick ✓

t9: `--joins` CLI string produces same result as file ✓

t10: `--imap` provided → individual counts filled correctly ✓

t11: Comments and blank lines ignored correctly ✓

t12: `--validate` exits 0 on valid input ✓

t13: `--validate` exits 1 on invalid input ✓

t14: `--json` output is valid JSON with correct fields ✓

t15: `--newick-only` prints only the Newick string ✓

**Error detection (tests 16–30)**

t16: UNDEFINED_OPERAND → correct error message and hint ✓

t17: TAXON_JOINED_TWICE → correct error with both line numbers ✓

t18: CLADE_JOINED_TWICE → correct error ✓

t19: DISCONNECTED (2 clades) → lists both clades, suggests join ✓

t20: DISCONNECTED (3 clades) → lists all three ✓

t21: DUPLICATE_LABEL → reports both lines ✓

t22: IMPLICIT_LABEL_CONFLICT with underscore species name → error
     with fix suggestion ✓

t23: EMPTY_FILE → correct error ✓

t24: SINGLE_TAXON_TREE → correct error ✓

t25: Species name with `-` (hyphen) is valid ✓

t26: Species name with `_` but no conflict → valid ✓

t27: Species name with `_` causing conflict → IMPLICIT_LABEL_CONFLICT ✓

t28: Stdin input (`echo "A+B" | bpp-tree`) → correct output ✓

t29: JSON error output has correct structure when errors present ✓

t30: Multiple errors in one file — all reported, not just first ✓

**Warning detection (tests 31–35)**

t31: POLYTOMY_PRESENT → warning in output ✓

t32: LARGE_TREE (>50 taxa, synthetic) → warning ✓

t33: Warning in JSON output has correct structure ✓

t34: Warnings do not prevent output production ✓

t35: `--validate` with warnings exits 0 (warnings are not errors) ✓

**Integration (tests 36–40)**

t36: Output Newick is parseable by bpp-tools newick.c ✓
     (pipe bpp-tree output into a minimal newick-parse check)

t37: species&tree block format matches what BPP expects
     (validate against the fixture bpp.ctl from bpp-seqs realdata) ✓

t38: `--imap` with samples.imap from bpp-seqs realdata →
     correct counts (4 4 4 4) for the 4-population fixture ✓

t39: bpp-tree + bpp-lint round-trip:
     build a control file using bpp-tree output, run bpp-lint,
     verify bpp-lint reports valid ✓

t40: Large synthetic tree (20 taxa) correct topology ✓

---

## Development Sequence

Build and test in this order:

1. `src/parser.c` — tokeniser and join statement parser
   Test: parse each example file into JoinStmt records

2. `src/tree.c` — TreeNode data structure and Newick serialiser
   Test: hand-build a 4-node tree, verify Newick output

3. `src/resolver.c` — dependency graph and topological sort
   Test: resolve the 4-taxon and 8-taxon examples correctly

4. `src/validate.c` — all error and warning checks
   Test: each error condition from tests 16–30

5. `src/imap.c` — Imap reader for individual counts
   Test: counts filled correctly for the realdata fixture

6. `src/json_writer.c` — copy from bpp-seqs, no changes needed

7. `src/main.c` — CLI, option parsing, output formatting
   Test: end-to-end on all 40 test cases

The implementation is complete when:
- `make` builds without warnings
- `make debug` (with ASan/UBSan) builds cleanly
- `make test` passes all 40 tests
- The species&tree block produced for the bpp-seqs realdata fixture
  (4 populations, 4 individuals each) is accepted by bpp-lint without
  errors
