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
more than two operands) with a clear error. It also supports **MSC-M migration
bands** (see below). Polytomy support and MSC-I introgression are planned for
later phases.

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
| `-i, --interactive` | Start interactive mode (workspace of named trees) |
| `--version` | Show version and exit |
| `--json` | Emit structured JSON (for tooling / agents) |
| `--json-indent N` | JSON indentation width (default 2) |
| `--quiet` | Suppress warnings/progress on stderr |
| `--joins STRING` | Join formula as a `,`- or `;`-separated string |
| `--imap FILE` | Imap file; fills individual counts automatically |
| `--display` | Also print the tree as an indented branching diagram |
| `--ascii` | With `--display`, use ASCII connectors instead of Unicode |
| `--move LIST` | Prune-and-regraft moves `SRC->DST` (see below) |
| `--graft LIST` | Add new tips `NEW->DST` (see below) |
| `--prune LIST` | Remove tips/subtrees (see below) |
| `--migration LIST` | MSC-M migration bands `SRC->DST` (see below) |
| `--rotate LIST` | Reverse the children of each named clade (see below) |
| `--out PREFIX` | Write `PREFIX.nwk` and `PREFIX.stree` |
| `--newick-only` | Print only the Newick string |
| `--validate` | Validate only; exit 0 if valid, 1 if errors |

### Displaying the tree

`--display` adds an indented, root-at-left branching diagram to the human
output. Tips show their name; internal nodes show their explicit label, or
their implicit leaf-set label when unlabelled (so every ancestor is named).
Unicode box-drawing is used by default; `--ascii` switches to plain ASCII.

```
$ bpp-tree --display examples/primates.joins
...
Tree:
  ┬ bonobo_chimp_gibbon_gorilla_human_macaque_orang_siamang
  ├── macaque
  └─┬ apes
    ├─┬ hominidae
    │ ├── orang
    │ └─┬ homininae
    │   ├── gorilla
    │   └─┬ hominini
    │     ├─┬ pan
    │     │ ├── chimp
    │     │ └── bonobo
    │     └── human
    └─┬ hylobatidae
      ├── gibbon
      └── siamang
```

The diagram reflects any `--move`/`--rotate` transforms. It is human-output
only (not included in `--json`).

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

### Grafting new tips

`--graft 'NEW->DST'` adds a brand-new tip `NEW` as the sister of an existing
node `DST` (a tip or clade), subdividing `DST`'s branch. This is how you grow
a finished tree — a join can't, because joins use each tip once, so reusing an
existing tip is a `TAXON_JOINED_TWICE` error. `move` relocates existing clades;
`graft` adds new ones.

```
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D' --graft 'E->D'   # ((A,B),(C,(D,E)));
```

`NEW` must not already be in the tree (use `move` to relocate) and cannot
contain `_` (it's a species name). Like `move`/`rotate`, grafts are a
command-line/interactive transform, not part of the order-free `.joins` file.

To graft a whole **subtree**, give a parenthesised join-formula as the source:

```
$ bpp-tree --quiet --newick-only --joins 'E+G,A+E_G' --graft '(P+Q; R+S)->E_G'
(A,((E,G),((P,Q),(R,S))));
```

The subtree is built with the normal join rules and must resolve to a single
tree of **new, unique** tips (none already in the target tree); otherwise the
graft is rejected.

### Removing tips and subtrees

`--prune 'NAME'` (interactive: `prune`/`remove`) removes a tip or clade and
suppresses its now-unary parent — the inverse of `graft`. Name tips or clades
by leaf-set/explicit label, `,`/`;`-separated for several.

```
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D' --prune 'A'     # (B,(C,D));
$ bpp-tree --quiet --newick-only --joins 'A+B,C+D' --prune 'A_B'   # (C,D);
```

Removing the root (the whole tree) is an error. Note that `prune` removes a
clade *from* the tree, whereas the interactive `drop` deletes a whole *tree*
from the workspace.

### Migration bands (MSC-M)

`--migration 'SRC->DST'` (interactive: `migration SRC->DST`) adds an MSC-M
migration band — gene flow from branch `SRC` to branch `DST`. Endpoints are
branches (tips or clades, named as usual), and the band is **directional**.
A band is valid only between two **non-nested, contemporaneous** branches:
source ≠ target, and neither may be the other's ancestor or descendant (those
don't coexist in time). Invalid bands are reported and excluded from output.
The diagram marks the endpoints (`M1→` source, `→M1` destination), colourised
by band on a colour terminal, with a legend:

```
$ bpp-tree --display --joins 'A+B,C+D,A_B+C_D' --migration 'A->C, C->B'
Tree:
  ┬ A_B_C_D
  ├─┬ A_B
  │ ├── A M1→
  │ └── B →M2
  └─┬ C_D
    ├── C →M1 M2→
    └── D

  migration bands:
    M1:  A → C
    M2:  C → B
```

The BPP `migration = N` block is emitted alongside `species&tree` (by the CLI
human output, `--out`, JSON, and the interactive `block` / `block replace`).
A clade endpoint is given an internal label in the Newick (e.g. `((A,B)A_B,…)`)
so BPP can reference the ancestral population. Migration is only valid between
branches that overlap in time — this is a topology-only tool (no divergence
times), so temporal overlap is left to BPP/`bpp-lint` to check. (BPP also
requires `wprior` and `speciestree = 0` when migration is set.)

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

## Interactive mode

`bpp-tree -i` opens a workspace of named trees held in memory. Exactly one
tree is *active*; anything you type that isn't a command is read as a join and
added to it, and `move`/`rotate` edit it — all changes are kept in memory.
`bpp-tree -i FILE` (or `-i --joins '...'`) seeds the first tree.

At a terminal you can recall previous commands with the **up/down arrows** and
edit them inline (←/→, Home/End, backspace) before running — handy for fixing a
typo or tweaking a `move`/`graft`. **Tab** completion is context-aware:
command names and clade/tip/Imap-species names at the start of a line (and as
`move`/`graft`/… arguments); **file paths** after `imap` and `block`; and tree
names after `use`/`drop`/`save`. A second ambiguous Tab lists the candidates.
Paths may be relative, absolute, or start with `~/` (expanded to `$HOME`).
`history` lists the commands you've entered.

```
$ bpp-tree -i
bpp-tree interactive mode. Type 'help' for commands, 'quit' to exit.
bpp-tree> A+B
[main] 2 taxa, tree complete
bpp-tree> C+D
[main] 4 taxa, tree complete
bpp-tree> save balanced
bpp-tree> move A_B -> C_D
bpp-tree> new caterpillar
bpp-tree> A_B+C            # auto-creates the A_B pair
bpp-tree> A_B_C+D
bpp-tree> display
bpp-tree> use balanced
bpp-tree> quit
```

| Command | Action |
| --- | --- |
| `A+B` (any join) | add a join to the active tree |
| `move SRC -> DST` | prune `SRC`, regraft as sister of `DST` |
| `graft NEW -> DST` | add a new tip `NEW` as sister of `DST` |
| `prune LIST` / `remove LIST` | remove tips/subtrees |
| `rotate LIST` | reverse children of the named clade(s) |
| `undo` | undo the last change to the active tree |
| `display [ascii]` / `newick` / `status` | view the active tree |
| `imap FILE` | attach an Imap to the active tree (no arg: show; `clear`: detach) |
| `migration SRC->DST` | add an MSC-M migration band (no arg: list; `clear`; `rm N`) |
| `taxa` | list the tree's tips and the attached imap's species |
| `block [FILE]` | print the `species&tree` block (stdout, or write to `FILE`) |
| `block replace FILE` | replace the `species&tree` block inside a BPP control file |
| `trees` | list trees in memory (active marked `*`) |
| `save NAME` | save a copy of the active tree as `NAME` |
| `use NAME` | make `NAME` the active tree |
| `new NAME` | start a new empty tree and make it active |
| `drop NAME` | delete a tree |
| `session save\|load [FILE]` | save/restore named trees (see below) |
| `history` | list the commands entered this session |
| `help` / `quit` | help; leave (also `exit`, or EOF) |

At an interactive terminal the workspace is **persisted like R's `.RData`**: on
exit you're asked whether to save your **named** trees, and a saved session is
restored automatically next time. The scratch `main` tree is *not* saved — to
keep work, `save NAME` it first. The image is `.bpptree` in the current
directory (override with `$BPPTREE_SESSION`); `session save`/`session load
[FILE]` do it explicitly. Auto load/save only happens at a terminal, so
piped/scripted use always starts fresh.

Each tree is stored as its joins plus an ordered list of moves/rotations and
recomputed on demand, so the declarative join set stays order-free while edits
remain an explicit, ordered layer. `imap FILE` attaches an Imap to the active
tree so that `block` fills in the per-species counts:

```
bpp-tree> imap samples.imap
imap attached to 'main': samples.imap (4 species)
bpp-tree> block
species&tree = 4  AFR  EUR  EAS  AMR
   2  2  2  2
  (AFR,(EUR,(EAS,AMR)));
```

`block FILE` writes the block to a file, and `block replace FILE` edits an
existing BPP control file in place — locating its `species&tree` statement
(names, counts, and the Newick, even when they wrap across lines) and swapping
in the new one, leaving the rest of the file untouched (the original is saved
to `FILE.bak`).

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
