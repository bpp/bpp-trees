#!/usr/bin/env bash
# bpp-tree test suite (phase 1: binary trees).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bpp-tree"
FIX="$ROOT/tests/fixtures"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0

# ok NAME EXPECTED -- run bpp-tree on stdin, compare stdout to EXPECTED.
# Usage: ok NAME EXPECTED <<<'joins'  (extra args after EXPECTED go to bpp-tree)
check() {
    local name="$1" expected="$2"; shift 2
    local got
    got="$("$BIN" "$@" 2>/dev/null)"
    if [[ "$got" == "$expected" ]]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL %s\n  expected: %q\n  got:      %q\n' "$name" "$expected" "$got"
    fi
}

# exit_is NAME WANT -- run, compare exit code only (input on stdin).
exit_is() {
    local name="$1" want="$2"; shift 2
    "$BIN" "$@" >/dev/null 2>&1
    local rc=$?
    if [[ "$rc" == "$want" ]]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL %s: exit %s, wanted %s\n' "$name" "$rc" "$want"
    fi
}

# has NAME NEEDLE -- run (capturing stdout+stderr), assert NEEDLE present.
has() {
    local name="$1" needle="$2"; shift 2
    local got
    got="$("$BIN" "$@" 2>&1)"
    if [[ "$got" == *"$needle"* ]]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        printf 'FAIL %s: %q not found in:\n%s\n' "$name" "$needle" "$got"
    fi
}

# --- Correctness (newick) ------------------------------------------------
check t1  '(A,B);'           --newick-only <<< 'A+B'
check t2  '((A,B),(C,D));'   --newick-only <<< $'A+B\nC+D\nA_B+C_D'
check t3  '(((A,B),C),D);'   --newick-only <<< $'A+B\nA_B+C\nA_B_C+D'
check t4  '((A,B),(C,D));'   --newick-only <<< $'A_B+C_D\nC+D\nA+B'   # reverse order
check t5  '((A,B),(C,D));'   --newick-only <<< $'A+B=AB\nC+D=CD\nAB+CD'
check t6  '((A,B),C);'       --newick-only <<< $'A+B=myAB\nmyAB+C'
check t8  '(macaque,((orang,(gorilla,((chimp,bonobo),human))),(gibbon,siamang)));' \
          --newick-only <<< $'chimp+bonobo=pan\npan+human=hominini\ngorilla+hominini=homininae\norang+homininae=hominidae\ngibbon+siamang=hylobatidae\nhominidae+hylobatidae=apes\nmacaque+apes'

# t9: --joins string equals file form
check t9  '((A,B),(C,D));'   --newick-only --joins 'A+B=AB,C+D=CD,AB+CD'

# t9b: an unquoted --joins string that the shell split is caught, not silently
#      truncated (--joins value plus a stray positional argument)
exit_is t9b 2 --joins 'A+B,' 'A_B+C'
has   t9c 'only one input'    --joins 'A+B,' 'A_B+C'
# t9d: too many positional arguments
exit_is t9d 2 'A+B' 'C+D'

# t11: comments and blank lines ignored
check t11 '(A,B);'           --newick-only <<< $'# a comment\n\nA+B   # trailing\n'

# t15: newick-only prints only the newick
check t15 '((A,B),C);'       --newick-only <<< $'A+B\nA_B+C'

# t28: stdin
check t28 '(A,B);'           --newick-only <<< 'A+B'

# --- Imap / block --------------------------------------------------------
# t10/t38: counts filled from samples.imap (2 individuals per population here)
has   t10 'species&tree = 4  AFR  EUR  EAS  AMR' --imap "$FIX/samples.imap" \
          <<< $'EAS+AMR=ea\nEUR+ea=eu\nAFR+eu'
has   t38 '2  2  2  2'  --imap "$FIX/samples.imap" \
          <<< $'EAS+AMR=ea\nEUR+ea=eu\nAFR+eu'

# t14: JSON has expected fields
has   t14 '"status": "ok"'          --json --joins 'A+B=AB,C+D=CD,AB+CD'
has   t14b '"newick": "((A,B),(C,D));"' --json --joins 'A+B=AB,C+D=CD,AB+CD'

# --- Auto-resolution of forced choices -----------------------------------
# Exactly two roots are joined automatically (one rooted tree contains both).
check ta1 '((A,B),(C,D));'          --newick-only --joins 'A+B,C+D'
has   ta2 'ROOT_AUTO_JOINED'        --joins 'A+B,C+D'
exit_is ta3 0 --validate --joins 'A+B,C+D'                   # 2 roots -> valid
# A 2-member reference is auto-created (its shape is forced).
check ta4 '((A,B),(C,D));'          --newick-only --joins 'A_B+C_D'
has   ta5 'PAIR_AUTO_CREATED'       --joins 'A_B+C_D'
# A_B is a valid synonym for an explicitly-labelled clade
check ta6 '((A,B),C);'              --newick-only --joins 'A+B=abanc,A_B+C'

# --- Node rotation -------------------------------------------------------
check tr1 '((B,A),(C,D));'  --quiet --newick-only --joins 'A+B,C+D' --rotate 'A_B'
check tr2 '((C,D),(A,B));'  --quiet --newick-only --joins 'A+B,C+D' --rotate 'A_B_C_D'  # root
check tr3 '((B,A),(D,C));'  --quiet --newick-only --joins 'A+B,C+D' --rotate 'A_B;C_D'  # multiple
check tr4 '((B,A),C);'      --quiet --newick-only --joins 'A+B=ab,ab+C' --rotate 'ab'   # by label
check tr5 '((A,B),(C,D));'  --quiet --newick-only --joins 'A+B,C+D' --rotate 'A'        # tip ignored
has   tr6 'ROTATE_IGNORED_TIP'  --joins 'A+B,C+D' --rotate 'A'
has   tr7 'ROTATE_UNKNOWN'      --joins 'A+B,C+D' --rotate 'ZZ'
exit_is tr8 1 --joins 'A+B,C+D' --rotate 'ZZ'
# tr9: rotate a clade produced by a transform (found via tree traversal, so it
# works even though the node lives in the resolver's move/graft array)
check tr9 '((H,(A,B)),(C,D));'  --quiet --newick-only --joins 'A+B,C+D' --graft 'H->A_B' --rotate 'A_B_H'

# --- Moves (subtree prune-and-regraft) -----------------------------------
# base tree: ((A,B),(C,(D,E)))  from joins A+B, C+D_E, A_B+C_D_E
MV='--quiet --newick-only --joins A+B,C+D_E,A_B+C_D_E'
check tm1 '(C,((D,E),(A,B)));'    $MV --move 'A_B->D_E'      # clade to sister of clade
check tm2 '(B,(C,((D,E),A)));'    $MV --move 'A->D_E'        # tip can move
check tm3 '(((A,B),(D,E)),C);'    $MV --move 'D_E->A_B'      # move up beside A_B
check tm4 '((C,A),((D,B),E));'    $MV --move 'A->C;B->D'     # chained, in order
has   tm5 'MOVE_INVALID'          $MV --move 'A_B->A'        # target inside source
has   tm5b 'inside the clade'     $MV --move 'A_B->A'
has   tm6 'MOVE_INVALID'          $MV --move 'A_B_C_D_E->A'  # moving the root
has   tm7 'MOVE_INVALID'          --joins 'A+B,A_B+C' --move 'A_B->A_B_C'  # target is parent
has   tm8 'MOVE_NOOP'             --joins 'A+B,A_B+C' --move 'A->B'        # already sisters
has   tm9 'MOVE_UNKNOWN'          --joins 'A+B,A_B+C' --move 'Z_Q->A'      # unknown source
exit_is tm10 1 --joins 'A+B,A_B+C' --move 'A_B->A'                         # invalid -> exit 1
# move then rotate (moves apply first)
check tm11 '(((B,A),(D,E)),C);'   $MV --move 'D_E->A_B' --rotate 'A_B'

# --- Grafts (add a new tip) ----------------------------------------------
check tg1 '((A,B),(C,(D,E)));'  --quiet --newick-only --joins 'A+B,C+D' --graft 'E->D'
check tg2 '(((A,B),E),(C,D));'  --quiet --newick-only --joins 'A+B,C+D' --graft 'E->A_B'  # onto a clade
has   tg3 'GRAFT_UNKNOWN'        --joins 'A+B,C+D' --graft 'E->ZZ'                          # bad target
has   tg4 'GRAFT_INVALID'        --joins 'A+B,C+D' --graft 'A->C'                           # tip already present
has   tg5 'GRAFT_INVALID'        --joins 'A+B'     --graft 'E+F->A'                         # '+' not a valid tip name
has   tg6 'GRAFT_INVALID'        --joins 'A+B'     --graft 'X=Y->A'                         # '=' not a valid tip name
# graft a whole subtree built from a parenthesised join-formula
check tg7 '(A,((E,G),((P,Q),(R,S))));' --quiet --newick-only --joins 'E+G,A+E_G' --graft '(P+Q;R+S)->E_G'
has   tg8 'already in the tree'  --joins 'A+B,C+A_B' --graft '(A+X)->C'                     # subtree tip clashes
has   tg9 'not valid'            --joins 'A+B'       --graft '(X_Y_Z+W)->A'                 # subtree itself invalid
has   tg10 'closing'             --joins 'A+B'       --graft '(C+D->A'                      # unbalanced parens

# --- Prune (remove a tip or subtree) -------------------------------------
check tp1 '(B,(C,D));'          --quiet --newick-only --joins 'A+B,C+D' --prune 'A'        # remove a tip
check tp2 '(C,D);'              --quiet --newick-only --joins 'A+B,C+D' --prune 'A_B'      # remove a subtree
check tp3 'B;'                  --quiet --newick-only --joins 'A+B' --prune 'A'            # down to one tip
has   tp4 'PRUNE_UNKNOWN'        --joins 'A+B,C+D' --prune 'ZZ'                             # not in tree
has   tp5 'PRUNE_INVALID'        --joins 'A+B,C+D' --prune 'A_B_C_D'                        # the root

# --- MSC-M migration bands -----------------------------------------------
MJ='--joins A+B,C+D,A_B+C_D'
has tmig1 'migration = 2'             $MJ --migration 'A->C,C->B'        # block emitted
has tmig2 'A  C'                      $MJ --migration 'A->C,C->B'        # 'src  dst' row
has tmig3 'M1→'                       $MJ --display --migration 'A->C'   # source marker
has tmig4 '→M1'                       $MJ --display --migration 'A->C'   # dest marker
has tmig5 'M1:  A → C'                $MJ --display --migration 'A->C'   # legend
check tmig6 '((A,B)A_B,(C,D)C_D);'    --newick-only $MJ --migration 'A_B->C_D'  # clade endpoints labelled
has tmig7 'MIGRATION_INVALID'         --joins 'A+B,A_B+C' --migration 'A_B->A'   # ancestor/descendant
has tmig7b 'do not coexist'           --joins 'A+B,A_B+C' --migration 'A_B->A'
exit_is tmig8 1 --joins 'A+B,A_B+C' --migration 'A_B->A'
has tmig9 'MIGRATION_UNKNOWN'         $MJ --migration 'ZZ->A'            # unknown branch
has tmig10 '"source": "A"'            --json $MJ --migration 'A->C'      # JSON migration array

# --- Tree display --------------------------------------------------------
DSP='--quiet --display --joins A+B,C+D'
has td1 '├─┬ A_B'    $DSP                                    # ancestor label (implicit)
has td2 '│ ├── A'    $DSP                                    # tip under a continuing branch
has td3 '└─┬ C_D'    $DSP
has td4 '┬ A_B_C_D'  $DSP                                    # root shows its leaf-set label
has td5 '|-+ A_B'    --quiet --display --ascii --joins 'A+B,C+D'   # ASCII fallback
has td6 '`-- D'      --quiet --display --ascii --joins 'A+B,C+D'
has td7 '┬ clade'    --quiet --display --joins 'A+B=clade,clade+C' # explicit ancestor label
# display reflects transforms: after the move, a {A,B,D,E} clade exists
has td8 'A_B_D_E'    --quiet --display --joins 'A+B,C+D_E,A_B+C_D_E' --move 'A_B->D_E'

# --- Validate exit codes -------------------------------------------------
exit_is t12 0 --validate --joins 'A+B'
exit_is t13 1 --validate --joins 'A+B,C+D,E+F'   # 3+ roots -> incomplete

# --- Error detection -----------------------------------------------------
has   t17 'TAXON_JOINED_TWICE'      --joins 'A+B,A+C'
has   t18 'CLADE_JOINED_TWICE'      --joins 'A+B=AB,AB+C,AB+D'
has   t19 'DISCONNECTED'            --joins 'A+B,C+D,E+F'
has   t19b '3 separate clades'      --joins 'A+B,C+D,E+F'
has   t20 '4 separate clades'       --joins 'A+B,C+D,E+F,G+H'
has   t21 'DUPLICATE_LABEL'         --joins 'A+B=c1,C+D=c1,c1+c1x'
has   t23 'EMPTY_FILE'              <<< $'# only comments\n\n'
has   t24 'SINGLE_TAXON_TREE'       <<< 'loneTaxon'
has   t29 '"status": "error"'  --json --joins 'A+B,C+D,E+F'
exit_is t29b 1 --json --joins 'A+B,C+D,E+F'

# '_' is reserved for joined clades (never a tip, never an explicit label):
# t22: explicit label containing '_' is rejected
has   t22 'LABEL_RESERVED_UNDERSCORE' --joins 'C+D=A_B'
# t27: a 3+-member reference with no join building it is ambiguous
has   t27 'AMBIGUOUS_CLADE'           --joins 'A_B_C+D'
has   t27b "can't be inferred"        --joins 'A_B_C+D'
# a clade joined under both its explicit and implicit names -> CLADE_JOINED_TWICE
has   t27c 'CLADE_JOINED_TWICE'       --joins 'A+B=abanc,abanc+C,A_B+D'

# t25: hyphen in species name is valid
check t25 '(my-sp,other);'          --newick-only --joins 'my-sp+other'
# t26: a name containing '_' reads as a clade of its parts (auto-created)
check t26 '((homo,sapiens),pan);'   --newick-only --joins 'homo_sapiens+pan'

# --- Warnings ------------------------------------------------------------
has   t31 'POLYTOMY_UNSUPPORTED'    --joins 'A+B+C'          # phase 1: error
has   t34 'valid'                   --joins 'A+B'            # warning still produces output
exit_is t35 0 --validate --joins 'A+B'                       # warnings are not errors

# --- Large / synthetic trees ---------------------------------------------
# Build a pectinate tree of N taxa: t1+t2=c2, c2+t3=c3, ...
gen_pectinate() {
    local n="$1" s="t1+t2=c2" i
    for ((i = 3; i <= n; i++)); do s+=",c$((i-1))+t$i=c$i"; done
    printf '%s' "$s"
}
# t40: 20-taxon pectinate, correct nesting at the tips
has   t40 '((((((((((((((((((t1,t2),t3),t4),t5),t6),t7),t8),t9),t10),t11),t12),t13),t14),t15),t16),t17),t18),t19),t20);' \
          --newick-only --joins "$(gen_pectinate 20)"
# t32: >50 taxa triggers LARGE_TREE warning
has   t32 'LARGE_TREE'              --joins "$(gen_pectinate 60)"

# --- Integration: round-trip realdata block shape ------------------------
has   t37 '(AFR,(EUR,(EAS,AMR)));'  --joins 'EAS+AMR=ea,EUR+ea=eu,AFR+eu'

# t39: generated species&tree block is accepted by bpp-lint (if available)
LINT="$HOME/repos/bpp-lint/bpp-lint"
if [[ -x "$LINT" ]]; then
    "$BIN" --out "$TMP/rt" --joins 'EAS+AMR=ea,EUR+ea=eu,AFR+eu' >/dev/null 2>&1
    {
        echo "seed = 42"; echo "seqfile = s.txt"; echo "Imapfile = s.imap"
        echo "jobname = out"; echo "speciesdelimitation = 0"; echo "speciestree = 0"
        sed 's/?/4/g' "$TMP/rt.stree"
        echo "usedata = 1"; echo "nloci = 1"; echo "thetaprior = gamma 2 2000"
        echo "tauprior = gamma 2 1000"; echo "burnin = 100"; echo "sampfreq = 2"
        echo "nsample = 100"
    } > "$TMP/rt.ctl"
    # bpp-lint exits 0 when only informational keyword-default warnings remain.
    if "$LINT" -q "$TMP/rt.ctl" >/dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail=$((fail+1)); printf 'FAIL t39: bpp-lint rejected generated control file\n'
        "$LINT" -q "$TMP/rt.ctl"
    fi
else
    echo "skip t39: bpp-lint not found at $LINT"
fi

# --- Interactive mode (REPL) ---------------------------------------------
chk_contains() {
    local name="$1" hay="$2" needle="$3"
    if [[ "$hay" == *"$needle"* ]]; then pass=$((pass+1))
    else fail=$((fail+1)); printf 'FAIL %s: %q not in REPL output\n' "$name" "$needle"; fi
}

# build, save, switch trees, undo
s1="$(printf 'A+B\nC+D\nnewick\nsave bal\nnew p\nA+B\nA_B+C\nnewick\ntrees\nuse bal\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti1 "$s1" '((A,B),(C,D));'      # active 'main' newick
chk_contains ti2 "$s1" '((A,B),C);'          # 'p' (pectinate) newick
chk_contains ti3 "$s1" 'bal'                 # saved tree shown in 'trees'
chk_contains ti4 "$s1" '* p'                 # active marker on 'p'

# moves apply in the session
s2="$(printf 'A+B\nC+D_E\nA_B+C_D_E\nmove A_B->D_E\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti5 "$s2" '(C,((D,E),(A,B)));'

# empty + incomplete states
s3="$(printf 'new x\nstatus\nA+B\nC+D\nE+F\nstatus\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti6 "$s3" 'empty'
chk_contains ti7 "$s3" 'incomplete'

# undo removes the last change
s4="$(printf 'A+B\nA_B+C\nnewick\nundo\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti8 "$s4" '(A,B);'              # after undo, back to 2-taxon tree

# graft adds a tip; failed transforms are reported but not committed
s5="$(printf 'A+B\nC+D\ngraft E->D\nnewick\ngraft Z->nope\ngraft A->C\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti9  "$s5" '((A,B),(C,(D,E)));' # graft applied
chk_contains ti10 "$s5" 'GRAFT_UNKNOWN'      # bad target reported
chk_contains ti11 "$s5" 'not applied'        # rejected, not committed

# unknown commands and malformed joins do not pollute the tree
s6="$(printf 'A+B\nprint\nfoo bar\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti12 "$s6" 'unknown command'
chk_contains ti13 "$s6" '(A,B);'             # tree unchanged by junk input

# prune (remove) a tip; a failed prune is reported but not committed
s7="$(printf 'A+B\nC+D\nremove A\nnewick\nprune ZZ\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti14 "$s7" '(B,(C,D));'         # tip removed, parent suppressed
chk_contains ti15 "$s7" 'PRUNE_UNKNOWN'      # bad target reported, not applied

# attach an imap and print the species&tree block with filled counts
s8="$(printf 'EAS+AMR=ea\nEUR+ea=eu\nAFR+eu\nimap %s\nblock\nquit\n' "$FIX/samples.imap" | "$BIN" -i 2>&1)"
chk_contains ti16 "$s8" 'imap attached'
chk_contains ti17 "$s8" 'species&tree = 4  AFR  EUR  EAS  AMR'
chk_contains ti18 "$s8" '2  2  2  2'
# block without an imap uses '?' placeholders
s9="$(printf 'A+B\nC+D\nblock\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti19 "$s9" '?  ?  ?  ?'
# attaching a missing imap is an error (not attached)
s10="$(printf 'A+B\nimap /no/such/file.imap\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti20 "$s10" 'cannot open'

# 'block FILE' writes the block to a file
printf 'A+B\nC+D\nblock %s/out.stree\nquit\n' "$TMP" | "$BIN" -i >/dev/null 2>&1
chk_contains ti21 "$(cat "$TMP/out.stree" 2>/dev/null)" 'species&tree = 4  A  B  C  D'

# 'block replace FILE' splices into an existing control file, keeping the rest
cp "$FIX/bpp.ctl" "$TMP/ctl"
printf 'A+B\nC+D\nblock replace %s\nquit\n' "$TMP/ctl" | "$BIN" -i >/dev/null 2>&1
ctl="$(cat "$TMP/ctl" 2>/dev/null)"
chk_contains ti22 "$ctl" '((A,B),(C,D));'        # new newick spliced in
chk_contains ti23 "$ctl" 'phase = 0 0 0 0'       # surrounding statements preserved
chk_contains ti24 "$ctl" 'tauprior'              # ... below the block too
# the original multi-line block is gone (old newick replaced)
if [[ "$ctl" == *"(AFR, (EUR, (EAS, AMR)))"* ]]; then
    fail=$((fail+1)); echo "FAIL ti25: old species&tree block not removed"
else pass=$((pass+1)); fi

# graft a subtree in interactive mode
sg="$(printf 'E+G\nA+E_G\ngraft (P+Q; R+S)->E_G\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti26 "$sg" '(A,((E,G),((P,Q),(R,S))));'

# 'taxa' lists tree tips and the attached imap's species (flagging unused ones)
st="$(printf 'EAS+AMR\nimap %s\ntaxa\nquit\n' "$FIX/samples.imap" | "$BIN" -i 2>&1)"
chk_contains ti27 "$st" 'imap species (4):'
chk_contains ti28 "$st" 'not yet in tree:'

# migration bands: add, see them in 'block', and reject an ancestor/descendant band
smig="$(printf 'A+B\nC+D\nA_B+C_D\nmigration A->C\nmigration C->B\nblock\nmigration A_B->A\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti29 "$smig" 'added migration M1'
chk_contains ti30 "$smig" 'migration = 2'
chk_contains ti31 "$smig" 'do not coexist'      # A_B->A rejected (A is inside A_B)

# session persistence: named trees are saved (scratch 'main' is not), restored
printf 'A+B\nnew keep\nX+Y\nsession save %s/sess\nquit\n' "$TMP" | "$BIN" -i >/dev/null 2>&1
sfile="$(cat "$TMP/sess" 2>/dev/null)"
chk_contains ti32 "$sfile" 'tree keep'
if [[ "$sfile" == *"tree main"* ]]; then
    fail=$((fail+1)); echo "FAIL ti33: scratch 'main' should not be saved"
else pass=$((pass+1)); fi
sl="$(printf 'session load %s/sess\nuse keep\nnewick\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti34 "$sl" '(X,Y);'
# imap and migration bands persist too
printf 'new mm\nP+Q\nR+S\nmigration P->R\nsession save %s/s2\nquit\n' "$TMP" | "$BIN" -i >/dev/null 2>&1
sl2="$(printf 'session load %s/s2\nuse mm\nmigration\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti35 "$sl2" 'M1:  P → R'

# --- MSC-I introgression -------------------------------------------------

# CLI: model A network emits the canonical eNewick with phi on the donor ref
nA="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=0.3' --newick-only)"
exit_is ti36 0 --joins 'A+B,A_B+C' --introgression 'C->A phi=0.3' --newick-only
chk_contains ti37 "$nA" '(A)H1[&tau-parent=yes]'
chk_contains ti38 "$nA" '&phi=0.3'
chk_contains ti39 "$nA" '&tau-parent=yes'

# Model B: src=node -> tau-parent=no on the donor (bare) occurrence
nB="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=0.05 src=node' --newick-only)"
chk_contains ti40 "$nB" '&tau-parent=no'

# Model C: both tau-parent=no
nC="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=0.2 src=node dst=node' --newick-only)"
chk_contains ti41 "$nC" '(A)H1[&tau-parent=no]'

# Ancestor/descendant rejected (non-contemporaneous)
err="$("$BIN" --joins 'A+B,A_B+C' --introgression 'A_B->A' 2>&1)"
chk_contains ti42 "$err" 'do not coexist'

# Phi out of range
err="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=1.5' 2>&1)"
chk_contains ti43 "$err" 'out of range'

# Migration + introgression on the same tree = MODEL_CONFLICT
err="$("$BIN" --joins 'A+B,A_B+C' --migration 'A->C' --introgression 'C->A' 2>&1)"
chk_contains ti44 "$err" 'MODEL_CONFLICT'

# At most one event per taxon pair (regardless of direction)
err="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A ; A->C' 2>&1)"
chk_contains ti45 "$err" 'more than one event'

# REPL: introgress + mode-lock + clear + hybrid
ri="$(printf 'A+B\nA_B+C\nintrogress C->A phi=0.3\nnewick\nmigration C->A\nintro clear\nmigration C->A\nquit\n' \
        | "$BIN" -i 2>&1)"
chk_contains ti46 "$ri" 'added introgression H1'
chk_contains ti47 "$ri" '&phi=0.3'
chk_contains ti48 "$ri" 'mutually exclusive'
chk_contains ti49 "$ri" 'added migration M1'           # works after intro cleared

# REPL: hybrid bundled command is sugar for graft + introgress
rh="$(printf 'A+B\nA_B+C\nhybrid H : A, C phi=0.4\nnewick\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti50 "$rh" '(A,(H)H1[&tau-parent=yes])'    # H grafted beside A
chk_contains ti51 "$rh" '&phi=0.4'

# REPL: session save/load round-trips introgression events
printf 'new net\nA+B\nA_B+C\nintrogress C->A phi=0.3 src=node\nsession save %s/si\nquit\n' "$TMP" \
    | "$BIN" -i >/dev/null 2>&1
slI="$(printf 'session load %s/si\nuse net\nintrogression\nnewick\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti52 "$slI" 'model B'
chk_contains ti53 "$slI" '&tau-parent=no'

# bpp-lint round-trip: a complete MSC-I control file lints clean.
LINT=$HOME/repos/bpp-lint/bpp-lint
if [[ -x "$LINT" ]]; then
    BLK="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=0.3' --newick-only)"
    cat > "$TMP/lint.ctl" <<EOF
seed = 1
seqfile = x.txt
Imapfile = x.imap
jobname = out
nloci = 100
species&tree = 3  A  B  C
                  2  2  2
  $BLK
phiprior = 1 1
thetaprior = 3 0.004
tauprior = 3 0.002
burnin = 1000
sampfreq = 2
nsample = 10000
EOF
    if "$LINT" "$TMP/lint.ctl" 2>&1 | grep -qE '(^|: )error:'; then
        fail=$((fail+1)); echo "FAIL ti54: bpp-lint rejects an MSC-I control file"
    else pass=$((pass+1)); fi
    # phiprior is mandatory when &phi= is present -> lint flags it when omitted
    grep -v '^phiprior' "$TMP/lint.ctl" > "$TMP/lint.nophi.ctl"
    lo="$("$LINT" "$TMP/lint.nophi.ctl" 2>&1)"
    chk_contains ti55 "$lo" "'phiprior' is required"

    # Model D bidirectional event between sister branches A and B
    BD=$("$BIN" --joins 'A+B,A_B+C' --introgression 'A<->B phi=0.3 phi2=0.1' --newick-only)
    sed -e "s|--BLK--|$BD|" > "$TMP/d.ctl" <<EOF
seed = 1
seqfile = x.txt
Imapfile = x.imap
jobname = out
nloci = 100
species&tree = 3  A  B  C
                  2  2  2
  --BLK--
phiprior = 1 1
thetaprior = 3 0.004
tauprior = 3 0.002
burnin = 1000
sampfreq = 2
nsample = 10000
EOF
    if "$LINT" "$TMP/d.ctl" 2>&1 | grep -qE '(^|: )error:'; then
        fail=$((fail+1)); echo "FAIL ti56: bpp-lint rejects a Model D control file"
    else pass=$((pass+1)); fi
fi

# Model D: bidirectional between sister tips A and B
nD="$("$BIN" --joins 'A+B,A_B+C' --introgression 'A<->B phi=0.3 phi2=0.1' --newick-only)"
chk_contains ti57 "$nD" 'H1'
chk_contains ti58 "$nD" 'H2'
chk_contains ti59 "$nD" '&phi=0.3'
chk_contains ti60 "$nD" '&phi=0.1'

# Model D requires sister branches (A and C are not sisters under A_B+C)
err="$("$BIN" --joins 'A+B,A_B+C' --introgression 'A<->C' 2>&1)"
chk_contains ti61 "$err" 'sister branches'

# src=/dst= are rejected on bidirectional events (BPP forbids tau on model D)
err="$("$BIN" --joins 'A+B,A_B+C' --introgression 'A<->B src=node' 2>&1)"
chk_contains ti62 "$err" 'do not accept'

# --- Interactive line editor (PTY; requires python3) ---------------------
if command -v python3 >/dev/null 2>&1; then
    if python3 - "$BIN" "$FIX/samples.imap" <<'PY'
import pty, os, select, time, sys
BIN = sys.argv[1]
IMAP = sys.argv[2]
os.environ["BPPTREE_SESSION"] = "/nonexistent/bpptree-iso"   # isolate from any saved session
def session(keys):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(BIN, [BIN, "-i"])
    out = b""
    for k in keys:
        os.write(fd, k); time.sleep(0.05)
    end = time.time() + 1.5
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r: break
        try: d = os.read(fd, 4096)
        except OSError: break
        if not d: break
        out += d
    try: os.close(fd)
    except OSError: pass
    os.waitpid(pid, 0)
    return out.decode(errors="replace")
UP, CR, BS, TAB = b"\x1b[A", b"\r", b"\x7f", b"\t"
# Up recalls the previous command and re-runs it
o1 = session([b"A+B"+CR, b"newick"+CR, UP, CR, b"quit"+CR])
# recall 'newick', edit it (delete, retype 'trees') -> runs 'trees', not newick
o2 = session([b"A+B"+CR, b"newick"+CR, UP, BS*6, b"trees"+CR, b"quit"+CR])
# Tab completes a command: 'disp' -> 'display' (prints the diagram, with U+252C)
o3 = session([b"A+B"+CR, b"C+D"+CR, b"disp"+TAB, CR, b"quit"+CR])
# Tab completes a clade name: 'C_' -> 'C_D', then prune leaves (A,B)
o4 = session([b"A+B"+CR, b"C+D"+CR, b"prune C_"+TAB, CR, b"newick"+CR, b"quit"+CR])
# Tab completes an imap species not yet in the tree: 'EU' -> 'EUR'
o5 = session([("imap %s" % IMAP).encode()+CR, b"EU"+TAB, b"+AFR"+CR, b"newick"+CR, b"quit"+CR])
# Tab completes a file path for 'imap': '<dir>/sam' -> '<dir>/samples.imap'
import os.path, tempfile, shutil
pfx = (os.path.dirname(IMAP) + "/sam").encode()
o6 = session([b"A+B"+CR, b"imap "+pfx+TAB+CR, b"taxa"+CR, b"quit"+CR])
# Tab completes a '~/...' path (HOME pointed at a temp dir with a known file)
hd = tempfile.mkdtemp()
with open(os.path.join(hd, "zz_taxa.imap"), "w") as f: f.write("s1\tA\ns2\tB\n")
os.environ["HOME"] = hd
o7 = session([b"A+B"+CR, b"imap ~/zz"+TAB+CR, b"taxa"+CR, b"quit"+CR])
shutil.rmtree(hd, ignore_errors=True)
# session image: save a named tree (answer 'y' at the prompt), auto-restore it
sdir = tempfile.mkdtemp(); os.environ["BPPTREE_SESSION"] = os.path.join(sdir, "sess")
oA = session([b"new persisted"+CR, b"A+B"+CR, b"quit"+CR, b"y"+CR])
oB = session([b"trees"+CR, b"quit"+CR, b"n"+CR])
shutil.rmtree(sdir, ignore_errors=True)
ok = (o1.count("(A,B);") >= 2 and ("main" in o2) and o2.count("(A,B);") == 1
      and "┬" in o3 and "(A,B);" in o4 and "(EUR,AFR);" in o5
      and "imap species" in o6 and "imap species" in o7
      and ("restored" in oB) and ("persisted" in oB))
sys.exit(0 if ok else 1)
PY
    then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL te1: PTY line-editor recall/edit/completion"; fi

    # te2: bare invocation at a terminal prints usage and exits (no stdin hang)
    if python3 - "$BIN" <<'PY'
import pty, os, select, time, sys
BIN = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.execv(BIN, [BIN])            # no args; stdin is a tty
t0 = time.time(); ex = None
while time.time() - t0 < 3:
    w, st = os.waitpid(pid, os.WNOHANG)
    if w == pid: ex = st; break
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try: os.read(fd, 4096)
        except OSError: pass
if ex is None:                       # still running -> it hung reading stdin
    os.kill(pid, 9); os.waitpid(pid, 0); sys.exit(1)
sys.exit(0 if os.WEXITSTATUS(ex) == 2 else 1)
PY
    then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL te2: bare invocation should print usage and exit"; fi
else
    echo "skip te1/te2: python3 not found (PTY tests)"
fi

echo
echo "passed: $pass   failed: $fail"
[[ "$fail" == 0 ]]
