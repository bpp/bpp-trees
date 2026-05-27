# bpp-tree

Compile a simple, human-readable **join formula** into a
[BPP](https://github.com/bpp/bpp)-compatible species tree: a Newick string
and a `species&tree` control-file block.

```
$ bpp-tree examples/primates.joins
bpp-tree: 8 taxa, 7 joins, valid

Newick:
  (macaque,((orang,(gorilla,((chimp,bonobo),human))),(gibbon,siamang)));

BPP species&tree block:
  species&tree = 8  macaque  orang  gorilla  chimp  bonobo  human  gibbon  siamang
     ?  ?  ?  ?  ?  ?  ?  ?
  (macaque,((orang,(gorilla,((chimp,bonobo),human))),(gibbon,siamang)));

Note: replace '?' with the number of sequences per species from your Imap file.
```

## Status — Phase 1

This is **Phase 1: binary trees only**. It covers the full pipeline —
parsing, order-independent resolution, validation, the `species&tree` block,
Imap count-filling, and JSON output — but **rejects polytomies** (joins with
more than two operands) with a clear error. Polytomy support, migration, and
introgression are planned for later phases.

## Build

```sh
make            # release build -> ./bpp-tree   (warning-free, -O2)
make debug      # ASan/UBSan build for testing
make test       # run the test suite
```

C11, no dependencies beyond libc/libm.

## The join formula

A `.joins` file (or stdin, or a `--joins` string) describes the tree as a set
of join operations, one per line:

```
chimp+bonobo      = pan      # name the new clade 'pan'
pan+human                    # implicit label: bonobo_chimp_human
```

- **`A+B`** joins two taxa or previously-named clades into a new clade.
- **`A+B = label`** names the resulting clade so later joins can reference it.
- Without `= label`, the **implicit label** is the clade's leaf names sorted
  alphabetically and joined with `_` (e.g. `chimp+bonobo` → `bonobo_chimp`).
  Reference it by that implicit label, e.g. `A+B` then `A_B+C`.
- Joins may appear in **any order**; dependencies are resolved automatically.
- Lines beginning with `#` are comments; blank lines are ignored.
- Tokens may contain letters, digits, `_` and `-`.

### `_` is reserved for joined clades

The `_` character is reserved syntax: a token containing `_` **always** means
"the clade joining these species" and is **never** a tip. This makes the
formula unambiguous:

- A **tip (species) name must not contain `_`** — `homo_sapiens` is read as the
  clade of `homo` and `sapiens`.
- **`A_B` denotes the clade of `A` and `B`** by its members. It is a valid
  **synonym** for that clade even if you also gave it an explicit label: after
  `A+B=abanc`, both `abanc` and `A_B` reference the same clade.
- An **explicit label cannot contain `_`** — `C+D=A_B` is rejected.

A token is a **leaf (species)** if it is never produced as the label of any
join (and contains no `_`); a **clade** if some join produces it. This is
usage-based — writing `A+B` defines `A` and `B` as taxa.

### Forced choices are auto-resolved

The tool fills in any grouping that has only one possible binary outcome, so
you write only the joins that carry a real topology decision — in any order:

- **A 2-member reference is auto-created.** `A_B+C_D` alone yields
  `((A,B),(C,D))` — no need to write `A+B` and `C+D` first. (A 3+-member
  reference like `A_B_C` still must be built by a join, since its branching
  order isn't implied by the name — otherwise you get an `AMBIGUOUS_CLADE`
  error with guidance.)
- **The final two clades are auto-joined.** When your joins leave exactly two
  separate clades, there is only one rooted tree containing both, so they are
  joined automatically. `A+B` / `C+D` → `((A,B),(C,D))`.

Both are reported as notes (`PAIR_AUTO_CREATED`, `ROOT_AUTO_JOINED`) so nothing
is hidden. You only have to make a decision when the input is genuinely
ambiguous: a 3+-member reference you haven't built, or **three or more**
separate clades remaining (`DISCONNECTED`) — the diagnostic then tells you what
to connect.

## Usage

```
bpp-tree [options] [JOINS_FILE]
```

| Option | Description |
| --- | --- |
| `-h, --help` | Show help and exit |
| `--version` | Show version and exit |
| `--json` | Emit structured JSON (for tooling / agents) |
| `--json-indent N` | JSON indentation width (default 2) |
| `--quiet` | Suppress warnings/progress on stderr |
| `--joins STRING` | Join formula as a `,`- or `;`-separated string |
| `--imap FILE` | Imap file; fills individual counts automatically |
| `--move LIST` | Prune-and-regraft moves `SRC->DST` (see below) |
| `--rotate LIST` | Reverse the children of each named clade (see below) |
| `--out PREFIX` | Write `PREFIX.nwk` and `PREFIX.stree` |
| `--newick-only` | Print only the Newick string |
| `--validate` | Validate only; exit 0 if valid, 1 if errors |

### Moving clades (prune-and-regraft)

`--move 'SRC->DST'` detaches clade `SRC` and regrafts it as the **sister** of
`DST` — a subtree prune-and-regraft (SPR). Name clades by leaf-set label
(`A_B`), explicit label, or (for `SRC`/`DST` that are single taxa) a tip name.
Multiple moves, separated by `,`/`;`, apply **in order**:

```
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D_E,A_B+C_D_E'                 # ((A,B),(C,(D,E)));
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D_E,A_B+C_D_E' --move 'A_B->D_E'  # (C,((D,E),(A,B)));
```

Moves are a transform applied to the finished tree — they live only on the
command line, so the `.joins` file stays order-free. A move is rejected (with
guidance) when the source is the root, the target lies inside the source
(can't regraft onto your own subtree), the target is the source's parent, or
either name is unknown; a move that is already in place is reported as a no-op.

When combined with `--rotate`, moves are applied first (topology), then
rotations (ordering).

### Rotating nodes

`--rotate` reverses the child order of one or more clades — it changes the
Newick string but not the topology (a "rotation"). Name each clade by its
leaf-set label (`A_B`) or its explicit label, separated by `,` or `;`:

```
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D'                  # ((A,B),(C,D));
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D' --rotate 'A_B'   # ((B,A),(C,D));
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D' --rotate 'A_B_C_D'  # ((C,D),(A,B));  (the root)
```

A tip in the list is ignored (it has no children to rotate); an identifier
that names no clade is an error.

**Exit codes:** `0` valid · `1` errors in the join formula · `2` system error.

## Validation

Errors (suppress output): `AMBIGUOUS_CLADE` (a 3+-member reference no join
builds), `TAXON_JOINED_TWICE`, `CLADE_JOINED_TWICE`, `CYCLE`, `DISCONNECTED`
(3+ clades remain), `DUPLICATE_LABEL`, `LABEL_RESERVED_UNDERSCORE`,
`EMPTY_FILE`, `SINGLE_TAXON_TREE`, `POLYTOMY_UNSUPPORTED` (Phase 1).

Notes (output still produced): `PAIR_AUTO_CREATED`, `ROOT_AUTO_JOINED`,
`LARGE_TREE`, `UNUSED_LABEL`.

All applicable diagnostics are reported together, each with a line number
(where applicable) and a suggested fix.

## Notes on the spec

Several points in `bpp-tree-spec.md` were resolved in favour of the spec's
authoritative rules, the realdata fixture, and design decisions made for this
project:

- The "small tree, implicit labels" prose example writes `AB+CD`, but the
  implicit-label rule (and test t2) make `A+B` produce `A_B`. The underscore
  forms are used.
- The spec's "Naming Rules and `_`" section treats species names with `_` as
  valid but potentially conflicting (`IMPLICIT_LABEL_CONFLICT`). Instead, `_`
  is **reserved**: never part of a tip name or an explicit label, so the
  conflict cannot arise and that error is removed.
- `UNDEFINED_OPERAND` is replaced by `AMBIGUOUS_CLADE`: 2-member references are
  auto-created, and only a 3+-member reference whose shape can't be inferred is
  an error. (The spec's `A+X` example contradicts the usage-based leaf rule.)
- `DISCONNECTED` fires at **3+** remaining clades, not 2: exactly two clades
  have a unique rooted join and are completed automatically (`ROOT_AUTO_JOINED`).
- The `species&tree` header lists taxa in Newick left-to-right order to match
  the provided `bpp.ctl` fixture (which BPP accepts via `bpp-lint`).
