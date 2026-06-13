# bpp-tree: Project Briefing for Claude Code

## What you are building

A standalone C command-line tool called `bpp-tree` that compiles a simple
human-readable join-formula into a BPP-compatible species tree specification.
Full specification is in `bpp-tree-spec.md`. Read that file completely before
writing any code.

## Files provided

- `bpp-tree-spec.md`      — complete specification (read this first)
- `json_writer.c / .h`    — copy from bpp-seqs, use as-is
- `imap.c / .h`           — copy from bpp-seqs workflow.c Imap logic,
                            or implement fresh per spec
- `samples.imap`          — bpp-seqs realdata Imap (4 populations × 4 samples)
- `bpp.ctl`               — bpp-seqs realdata BPP control file (reference)

## What it does

Reads a `.joins` file like:

    chimp+bonobo      = pan
    pan+human         = hominini
    gorilla+hominini  = homininae
    orang+homininae   = hominidae
    gibbon+siamang    = hylobatidae
    hominidae+hylobatidae = apes
    macaque+apes

And produces:
- A Newick tree string
- A BPP `species&tree` block ready to paste into a control file
- JSON output when --json is given (for agent use)

## Key design points

- Order independent: joins may appear in any order; resolver sorts them
- `_` is the reserved separator in implicit clade labels
- Species names with `_` are valid but may conflict — detect and report
- All errors reported together, not just the first one
- Exit 0 = valid, 1 = errors, 2 = system error
- No external dependencies beyond libc

## Implementation order (follow this)

1. src/parser.c       tokeniser + join statement parser
2. src/tree.c         TreeNode struct + Newick serialiser
3. src/resolver.c     dependency graph + topological sort (Kahn's algorithm)
4. src/validate.c     all 9 error conditions + 4 warning conditions
5. src/imap.c         Imap reader for individual counts
6. src/json_writer.c  copy from bpp-seqs
7. src/main.c         CLI, option parsing, human + JSON output

## Done when

- make builds without warnings
- make debug builds cleanly under ASan/UBSan
- make test passes all 40 tests
- species&tree block for the 4-population realdata fixture is accepted
  by bpp-lint without errors

---

## MSC-I introgression: graph-model redesign (IN PROGRESS)

The tool has grown well beyond the original spec above (MSC-M migration, MSC-I
introgression with extended-Newick I/O, an interactive REPL, `--read`). The
current work is a **redesign of MSC-I onto a single graph model**. If you are
touching `src/import.c`, `src/introgress.c`, the `--introgression` flag, or the
interactive `introgress` command, **read `docs/graph-model.md` in full first —
it is the authoritative spec.** This section is just the orientation.

### Why
MSC-I is currently stored as a *fixed base binary tree + a flat list of events,
each pinned to a named branch*. That model **cannot represent stacked
introgressions** (two reticulation edges on the same lineage — e.g. the Akey
"M3" Neanderthal trees), and the importer's heuristic recovery into it has been
the source of a run of bugs. The redesign makes the importer and the
construction language share **one network graph**: anything readable is
writable, and the importer becomes a faithful parse instead of a heuristic.

### Baseline already landed (do not re-fix)
- **phi orientation on import** (commit ed259d3): BPP's `&phi=X` is the weight of
  the edge `parent(annotated-occurrence) → hybrid`; `ev.phi` is the *donor's*
  contribution, so it is complemented (`1−X`) when the annotation sits on the
  recipient's primary edge. Confirmed against bpp source.
- **multi-event import** (commit 2b40bfb): `labels[]` owns string copies (fixed a
  use-after-free); recovery-time leaf-name collection skips bare hybrid refs
  (fixed an implicit-label leak like `ALTAI_CHAG_VINDIJA_nh_hyb`). M1/M2 read and
  round-trip; M3 still fails with a clean `INTROGRESSION_UNKNOWN`, which the
  redesign fixes.

### Design decisions (full detail in docs/graph-model.md)
- A branch is named by its **lower node**; an event attaches to the branch
  **immediately above** the named endpoint. Endpoints may be a tip, a clade, or
  a **prior event's name** (its hybrid node).
- **Stacking = creation order, latest event innermost.** No `above`/`below`
  keyword. Reference a prior event's name to override the default order.
- **Names via `= NAME`** (like `A+B = pan`): unique, no `_`, no tip/clade
  collision; `label=` is a synonym; auto-named `H1, H2, …` if omitted.
- `phi` is always the donor's contribution, emitted on the donor-side bare ref.

### Implementation order
1. Graph data structure + faithful eNewick parse, replacing
   `recover_introgressions`.
2. Re-emit by direct serialization of the graph (idempotent by construction).
3. Derived display/legend from the graph.
4. Construction-side insertion semantics (insert-above-node; multiple hybrids per
   branch; node-valued endpoints; `= NAME`).
5. Base-tree/join recovery for editing (`move`/`graft`/`rotate`).

### Testing
- Keep every existing test green (`make test`, currently **197/0**; also clean
  under `make debug` ASan/UBSan).
- Portable real-network fixtures: `tests/fixtures/bpp/*.stree`
  (yeast, anopheles, ghost, neander-m1, neander-m2). Add an **M3 stacked**
  fixture once it reads, with the construction-language form from graph-model.md.
- Semantic oracle: where a `bpp` binary exists (it was at `/usr/local/bin/bpp`
  on the original dev box; not guaranteed elsewhere), `bpp --cfile FILE` prints
  the parsed hybridization summary (per-edge phi, tau flags,
  `phi_X : parent -> X` direction). Require bpp's reading of bpp-tree's
  *re-emitted* network to match its reading of the original — that is the real
  definition of "robust to every eNewick." `bpp --msci-create DEFS` builds graphs
  from a definitions file and is prior art for the construction language
  (`tree` / `define ... as` / `hybridization ... as ... tau ... phi ...`).
- Larger reference corpora (not committed, original machine only):
  `~/repos/akey_reanalysis/bpp_ctl` (M1/M2/M3 across many files),
  `~/repos/ARGmigrationROC` (ghost), `~/repos/bpp/examples` (yeast, anopheles).
