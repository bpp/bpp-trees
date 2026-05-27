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
