# bpp-tree examples

Three inputs showing the two ways to define a tree: a **join formula** (build a
tree bottom-up from pairwise joins) and **reading an existing tree**. Each
prints the BPP `species&tree` block; add `--json` for machine-readable output,
`--display` for an indented diagram.

## primates.joins — join formula with clade labels

Great apes plus a macaque outgroup, joined by named clades.

```
bpp-tree examples/primates.joins
# → (macaque,((orang,(gorilla,((chimp,bonobo),human))),(gibbon,siamang)));
```

## four_pop.joins — join formula, counts from an Imap

The `(AFR,(EUR,(EAS,AMR)))` topology. Pass an Imap to fill per-species
individual counts automatically:

```
bpp-tree examples/four_pop.joins
bpp-tree --imap ../tests/fixtures/samples.imap examples/four_pop.joins
# → (((AMR,EAS),EUR),AFR);
```

## chr22_fig2c.nwk — read an MSC-I network (extended Newick)

An introgression model in extended-Newick form (`&phi`, hybrid nodes). Read it
back to recover the full species&tree block with its introgression events:

```
bpp-tree --read examples/chr22_fig2c.nwk
bpp-tree --read examples/chr22_fig2c.nwk --display   # indented diagram
```
