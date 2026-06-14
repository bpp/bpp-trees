# bpp-tree: introgression graph model and insertion semantics

Status: **implemented and shipped.** The single network graph is the one
representation for parse, re-emit, display, construction and editing across
every BPP model (A, B, C, and D); the old flat event list and its heuristic
importer (`recover_introgressions`) are gone. This document is the design
rationale; `src/graph.h` and the "graph model" section of `CLAUDE.md` are the
as-built reference.

Two refinements the implementation added to the design below — read these before
the rest:

- **Native-edge normalisation.** "The base species tree is recovered by deleting
  every secondary edge and suppressing each hybrid node" (below) is true only
  once `primary_parent` is the **native** (own-τ, `tau-parent=yes`) side. In a
  Model-B *swap* the eNewick writes the recipient subtree on the *donor* side, so
  `graph_from_newick` swaps the two parents (and `phi → 1−phi`) at parse time.
  After that the donor is **always** the secondary side and `phi` is always its
  contribution there — no per-model φ complement anywhere downstream.
- **Donor bare ref emits first.** On a donor attachment the bare hybrid reference
  is the *first* child: BPP pairs stacked-hybrid φ by child order and rejects a
  trailing bare ref with "phi do not sum to 1".
- **Model D** is two hybrids that are each other's secondary parent. It is built
  and re-emitted directly (φ but no `tau-parent`), and `graph_events` collapses
  the pair into one legend line. (Model-D *construction* via `A<->B` still uses
  the legacy `IntroList` emitter; importing/displaying it uses the graph.)

## Why

The current model stores a fixed base binary tree plus a flat list of events,
each pinned to a *named branch* (`donor = MOD`, `recipient = NEAN`). It cannot
represent networks where two introgression edges **stack on the same lineage**
(e.g. two modern→Neanderthal pulses at different depths): stacking subdivides a
branch into ordered intermediate nodes that are neither base-tree clades nor
expressible as independent branch-keyed events. The importer's heuristic
recovery into that model is also where every recent bug lived (phi orientation,
use-after-free, implicit-label leak, the M3 collapse).

The fix is one **graph** model, shared by the importer and the construction
language: anything that can be read can also be written, and vice versa.

## The graph

A tree whose branches may carry an **ordered chain of hybrid nodes**.

```
Node:
  children[]            subtree(s) below
  parents[1..2]         one parent normally; TWO for a hybrid node
  label                 tip name, clade label, implicit leaf-set, or event name
  is_hybrid             two parents → a reticulation
  // hybrid-only:
  primary_parent        the parent on this node's own lineage (vertical edge)
  secondary_parent      the donor end (the introgression / horizontal edge)
  phi                   donor's contribution = weight on the secondary edge
  tau_primary           own-τ flag on the primary (recipient) edge   → dst
  tau_secondary         own-τ flag on the secondary (donor)  edge    → src
```

A **branch** is the edge between a node and its (primary) parent. Every node has
exactly one branch above it, so a branch is named unambiguously by its **lower
(child) node**. For a hybrid node, "the branch above it" always means the
**primary** edge, never the introgression edge.

A single introgression event is realized as a pair of inserted nodes:

- `H` — the **hybrid node** on the recipient lineage (carries the event's name).
- `D` — the **donor attachment** on the donor lineage.
- a secondary edge `D → H` carrying `phi` (the donor's contribution; the
  recipient keeps `1 − phi` from its primary parent).

The base species tree is recovered by deleting every secondary edge and
suppressing each now-unary hybrid node (`H` → its child). The event list is read
back off the secondary edges. So the persisted form stays exactly as today —
**base joins + an ordered list of `introgress` lines** — and replaying the lines
in order rebuilds the graph.

## Endpoints

An endpoint resolves to a **node**; the event attaches to the branch
immediately above it. Endpoints may be:

- a **tip** (`CEU`),
- a **clade** by leaf-set label (`VINDIJA_CHAG`) or explicit label (`MOD`),
- a **prior event's name** (`hn1`) — meaning that event's hybrid node `H`.

The last form is what lets a later event attach above a specific earlier one
(see *Overriding order*).

## Insertion semantics

`DONOR -> RECIP [= NAME] [phi=P] [src=branch|node] [dst=branch|node]`

1. Resolve `RECIP` to node `r`; let `p_r = primary_parent(r)`.
2. Resolve `DONOR` to node `d`; let `p_d = primary_parent(d)`.
3. **Insert `H` on the recipient branch** (split `r`'s branch): replace `r` by
   `H` in `p_r`'s children; set `H.child = r`, `H.primary_parent = p_r`.
4. **Insert `D` on the donor branch**: replace `d` by `D` in `p_d`'s children;
   set `D.children = { d, H }`, `D.primary_parent = p_d`; set
   `H.secondary_parent = D`.
5. Set `H.phi = P` (default 0.5), `H.tau_secondary` from `src`, `H.tau_primary`
   from `dst` (defaults `branch`/`branch` → model A). Name `H = NAME`.

Because steps 3–4 always insert **immediately above the named node**, a later
event that names the same `r`/`d` lands *below* the previous one — the new node
goes into what is now the lowest segment. **The latest event takes the bottom
(innermost) slot.** That single rule is the whole of stacking; there is no
`above`/`below` keyword.

### Worked example — the akey M3 network

```
MOD -> NEAN = hn1 phi=0.05
MOD -> NEAN = hn2 phi=0.005
VC  -> CEU  = nh  phi=0.02
```

Recipient (NEAN) lineage, applied in order:
- `hn1`: insert above NEAN → `NEAN → hn1 → HN`
- `hn2`: "above NEAN" is now the `NEAN→hn1` segment → `NEAN → hn2 → hn1 → HN`

Donor (MOD) lineage, same rule:
- `hn1`: insert above MOD → `MOD → d1 → HN`
- `hn2`: "above MOD" is now `MOD→d1` → `MOD → d2 → d1 → HN`

Pairing the secondary edges (`d2→hn2`, `d1→hn1`) gives `hn2`/`d2` (φ=0.005)
innermost and `hn1`/`d1` (φ=0.05) outside them on **both** lineages — exactly
M3, produced by three arrows in order with no extra syntax.

### Overriding order

The default (latest = innermost) covers the common case. To stack in a
different order, name the node to attach above by referencing a prior event:

```
MOD -> NEAN = hn2 phi=0.005     # inner
MOD -> hn2  = hn1 phi=0.05      # attach hn1 above the hn2 hybrid node
```

`hn2` as a donor/recipient endpoint denotes that event's hybrid node, so `hn1`
is inserted on the branch immediately above it. (Addressing the *donor*-side
attachment explicitly is rarely needed because creation order already stacks it;
if it ever is, a `define NAME = <clade>` form — mirroring BPP's `define ... as`
— can name an arbitrary point. Deferred until a case needs it.)

## Naming (`=`)

Every introgression carries a **unique name**, mirroring the join syntax
(`A+B = pan`):

- `DONOR -> RECIP = NAME` names the event (and its hybrid node `H`).
- The name must be unique among events and must not collide with a tip or clade
  label. `_` is forbidden (it is the implicit-clade separator), as for explicit
  clade labels.
- If `= NAME` is omitted, a name is auto-generated (`H1`, `H2`, …) as today.
- The existing `label=NAME` option remains an accepted synonym for `= NAME`.

Names are what make events **addressable** (for ordering overrides) and make
re-emission **stable** (the eNewick hybrid label is the event name).

Bidirectional (Model D) events name both coupled nodes:
`A <-> B = Ha/Hb phi=P phi2=Q` (or auto-named `H1/H2`).

## Re-emission (extended Newick)

Serialize the graph directly — no heuristic recovery:

- Traverse the primary spanning tree. Each hybrid node `H` is emitted **twice**:
  once with its subtree at its primary-parent position, `(child)NAME[&tau-parent=<dst>]`;
  once as a bare `NAME[&phi=<P>,&tau-parent=<src>]` at its secondary-parent
  (donor) position.
- `phi` is written on the **donor-side** bare reference and equals the donor's
  contribution (the convention the emitter already uses).

Because the structure is explicit, write→read→write is idempotent **by
construction** for every network, stacked or not.

## Display

Derived from the graph, never re-derived heuristically:

- Render the primary spanning tree (follow `primary_parent`); show each hybrid
  node under its primary parent.
- Mark the secondary edge: `NAME⇝` on the donor node, `⇝NAME(.P)` on the
  recipient hybrid. Stacked hybrids are simply several marked nodes along a
  lineage.
- Legend per event: `NAME:  donor ⇝ recipient   phi=P  [model A|B|C|D]`, with the
  model letter derived from `tau_primary`/`tau_secondary`.

## Import (the inverse)

Parsing eNewick builds the graph directly (`graph_from_newick`):

- Each label occurring twice is a hybrid node. The occurrence carrying a subtree
  gives `H.child`; the bare occurrence gives the donor end and `phi`. `tau-parent`
  flags map to `tau_primary`/`tau_secondary`. Then **native-edge normalisation**
  (see the top of this doc) makes `primary_parent` the own-τ side.
- No tree rewriting, no "definition occurrence" guessing, no donor/recipient name
  reconstruction. The graph *is* the parsed structure, so stacked networks (M3)
  and bidirectional (model D) networks parse with no special handling. The
  importer is a faithful parse; the old heuristic `recover_introgressions` has
  been deleted.

Editing (`move`/`graft`/`prune`/`rotate`) operates on the recovered base tree;
events re-pin to nodes after each edit by deriving the event list from the graph,
applying the edit to the base tree, and rebuilding with `graph_construct`. An
edit that deletes an event endpoint makes the rebuild fail — the edit is refused
with a message, the read is not.

## Validation

- `phi ∈ (0, 1)` (and `phi2` for Model D).
- donor ≠ recipient; neither endpoint is the other's ancestor/descendant at
  insertion time (contemporaneity beyond topology is left to BPP/bpp-lint, since
  bpp-tree carries no divergence times).
- each hybrid node has exactly two parents (a node is a recipient once); a
  *lineage* may host many.
- names unique; no `_`; no collision with tips/clades.
- the result must remain a DAG (the non-ancestor rule prevents secondary-edge
  cycles).

## Backward compatibility

- Every current `.joins` + `introgress` input is unchanged: a single event per
  branch is just "no stacking," and `= NAME` / references are optional.
- The persisted representation is still base joins + an ordered `introgress`
  list; only the *application* changes (insert-above-node, multiple hybrids per
  branch, node-valued endpoints).
