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

## MSC-I introgression: the graph model (LANDED)

MSC-I is built on a **single network graph** that is the one representation for
parse, re-emit, display, construction and editing across **every** BPP model
(A, B, C, and D / bidirectional). The old "fixed base tree + flat list of
branch-keyed events" model and its heuristic importer (`recover_introgressions`)
are **gone**. `docs/graph-model.md` is the authoritative design spec; this is
the orientation. If you touch the network code, read `src/graph.h` first.

### Architecture (where things live)
- **`src/newick.{h,c}`** — the shared (extended-)Newick parser. A hybrid label
  simply appears twice; no MSC-I interpretation here.
- **`src/graph.{h,c}`** — the network graph and everything structural:
  `graph_from_newick` (faithful build), `graph_to_newick` (re-emit),
  `graph_base_newick` (the species tree), `graph_events` (display/legend events),
  and `graph_alloc`/`graph_new_node`/`graph_add_child` (build primitives).
- **`src/introgress.c`** — bridges graph ↔ resolver: `graph_construct` (build a
  network from a base tree + ordered events), `introlist_events` (graph → flat
  event list), `introlist_mark` (display markers), `introlist_from_graph`
  (= events + mark), plus the legacy `IntroList` parse/apply/legend still used
  for **simple, non-stacked construction**.
- **`src/import.c`** — builds the graph and, for any network with hybrids, sets
  `Import.graph` + `graph_only`; the flat `Import.intro` is also filled (from the
  graph) for the REPL/JSON. Plain trees take the normal join path.

### Invariants you must not break
- **Native-edge normalisation.** After `graph_from_newick`, a hybrid's
  `primary_parent` is always the **native** (own-τ, `tau-parent=yes`) side and
  `secondary_parent` is the **donor**. So the base tree follows native edges
  (BPP's species tree — yeast's Sbay basal), re-emission is the swapped form BPP
  reads identically, and the donor is **always** the secondary side with
  `H->phi` its contribution (no per-model φ complement — getting this wrong
  re-reads a model-A φ as `1−φ`).
- **Bidirectional = mutual secondary.** `gn_is_bidir(H)` ⇔ `H` and its donor are
  each other's secondary parent. Bidir nodes emit φ but **no `tau-parent`** (BPP
  rejects τ there); `graph_events` collapses the pair into one model-D event.
- **Stacking = insert immediately above the named node, latest innermost.** The
  donor bare ref is the attachment's **first** child (BPP pairs stacked-hybrid φ
  by child order and rejects a trailing bare ref with "phi do not sum to 1").
- **Editing** re-pins: derive events (`introlist_events`), apply the edit to the
  base tree, rebuild with `graph_construct(..., check_names=0, ...)`. An edit
  that deletes an endpoint makes the rebuild fail → the edit is refused, the read
  is not.

### Construction language (`--introgression`)
`DONOR -> RECIP [= NAME] [phi=P] [src=branch|node] [dst=branch|node]`,
`','`/`';'`-separated. Repeating a recipient **stacks** pulses; an endpoint may
name a prior event (order override). `= NAME` names the event (`label=` synonym;
auto `H1,H2,…`; unique, no `_`, no tip/clade collision). `A <-> B` is model D.

### Testing
- `make test` is **254/0**; `make debug` is clean under ASan/UBSan (3 pre-existing
  `null format string` warnings from system FORTIFY headers are not ours).
- Portable fixtures `tests/fixtures/bpp/*.stree`: yeast, anopheles, ghost,
  neander-m1/m2, and **neander-m3** (the real Akey stacked network).
- `tests/graph_roundtrip` (a Makefile target wired into `make test`) asserts
  `parse → graph → string` is byte-stable for every fixture.
- **Semantic oracle** (present on this machine at `/usr/local/bin/bpp`, not
  guaranteed elsewhere): `bpp --cfile FILE` prints the parsed hybridisation
  summary (`phi_X : parent -> X`, τ flags, bidirection count). The standard is
  that bpp's reading of bpp-tree's **re-emitted** network matches its reading of
  the original — verified for every fixture incl. M3 and model D. Needs a minimal
  runnable ctl (`jobname=`, `finetune = 1`, a tiny seqfile+Imap, `usedata = 0`).
- Larger corpora (not committed): `~/repos/akey_reanalysis/bpp_ctl` (M1/M2/M3),
  `~/repos/ARGmigrationROC` (ghost), `~/repos/bpp/examples` (yeast, anopheles).

### Remaining optional tidy-ups (not correctness gaps)
- **Model-D *construction*** (`A<->B`) still emits via the legacy `IntroList`
  path (`introlist_apply` + `introgress_newick`); import and display of model D
  go through the graph. Routing construction through `graph_construct` too would
  let that legacy emit retire.
- **ROOT-label round-trip**: `extract_newick_and_blocks` truncates the outermost
  label; the graph re-emit therefore drops a root label like `ROOT`
  (oracle-confirmed harmless). A proper fix routes plain trees through the graph
  as well, so the legacy join `walk` no longer needs to drop the root label.
