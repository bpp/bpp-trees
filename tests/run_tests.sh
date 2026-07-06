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

# REPL stacking: two pulses on the same recipient (NEAN->HUMAN, then A->HUMAN).
# Used to be refused with "a hybrid node has two parents"; now routes through
# the graph constructor like the CLI, so it commits and emits stacked eNewick.
sstk="$(printf 'A+B=AB\nC+D=CD\nAB+CD=R\nintrogress A->C\nintrogress B->C\nintro\nnwk\nquit\n' | "$BIN" -i 2>&1)"
chk_contains tirepl_stk1 "$sstk" 'added introgression H1'
chk_contains tirepl_stk2 "$sstk" 'added introgression H2'
chk_contains tirepl_stk3 "$sstk" 'H1:  A'        # legend lists both events
chk_contains tirepl_stk4 "$sstk" 'H2:  B'
chk_contains tirepl_stk5 "$sstk" ')H2[&tau-parent=yes])H1[&tau-parent=yes]'   # stacked emit
# Self-loop is still rejected through the graph path.
sbad="$(printf 'A+B\nintrogress A->A\nquit\n' | "$BIN" -i 2>&1)"
chk_contains tirepl_stk6 "$sbad" 'donor and recipient are the same'

# Stacking onto an IMPORTED event whose label contains '_' (akey-style 'nh_hyb').
# The new event's name is validated; existing imported labels are trusted -- the
# old behaviour erroneously re-checked them and rejected the underscore.
cat > "$TMP/single_nh.stree" <<EOF
species&tree = 4 NEAN GBR AFR1 AFR2
                 1    1   1    1
                 ((NEAN,(GBR)nh_hyb[&phi=0.05,&tau-parent=no]),(nh_hyb[&tau-parent=yes],(AFR1,AFR2)A));
EOF
snh="$(printf 'read %s\nintrogress AFR1->GBR\nintro\nquit\n' "$TMP/single_nh.stree" | "$BIN" -i 2>&1)"
chk_contains tirepl_stk7 "$snh" 'added introgression H1'
chk_contains tirepl_stk8 "$snh" 'nh_hyb:'         # the imported event survives
chk_contains tirepl_stk9 "$snh" 'H1:  AFR1'       # the new event was committed
[[ "$snh" != *"must not contain '_'"* ]] && pass=$((pass+1)) || \
    { fail=$((fail+1)); echo "FAIL tirepl_stk10: imported '_' label rejected"; }

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

# 'random N [seed=K]' generates a random binary tree on N tips. The seed makes
# the topology reproducible; default tip names go a..z, aa..zz, ...
srnd="$(printf 'random 5 seed=42\nnwk\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rnd1 "$srnd" 'random tree: 5 tips, 4 joins'
chk_contains ti_rnd2 "$srnd" '((a,((b,c),d)),e);'              # seed=42 is stable
srnd2="$(printf 'random 28 seed=1\ntaxa\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rnd3 "$srnd2" 'aa'                              # 27th name
chk_contains ti_rnd4 "$srnd2" 'bb'                              # 28th name
# 'random N as NAME' creates a new named tree, leaving the active scratch
# tree untouched.
srnd3="$(printf 'A+B\nrandom 4 seed=3 as rnd\nuse main\nnwk\nuse rnd\nstatus\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rnd5 "$srnd3" '(A,B);'                          # main untouched
chk_contains ti_rnd6 "$srnd3" '[rnd] 4 taxa'                    # new named tree
# Validation: N < 2 rejected with a sensible message; bad args show usage.
srnd4="$(printf 'random 1\nrandom xyz\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rnd7 "$srnd4" 'need at least 2 tips'
chk_contains ti_rnd8 "$srnd4" 'usage: random N'

# 'rtopology' permutes the topology of the active (or named) tree, keeping
# its tip set. Refuses if the tree carries migration or introgression events.
srt="$(printf 'random 6 seed=11\nnwk\nrtopology\nnwk\ntaxa\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rt1 "$srt" 'random tree: 6 tips'
chk_contains ti_rt2 "$srt" 'rtopology: re-randomised'
# Same tip set after rtopology -- 'taxa' lists a..f.
chk_contains ti_rt3 "$srt" 'a b c d e f'
# Refusal paths: migration band present.
srt2="$(printf 'A+B\nC+D\nA_B+C_D\nmigration A->C\nrtopology\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rt4 "$srt2" 'has 1 migration band'
# Refusal paths: introgression event present.
srt3="$(printf 'A+B\nC+D\nA_B+C_D\nintrogress C->A\nrtopology\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rt5 "$srt3" 'has 1 introgression event'
# rtopology on an unknown tree name reports it and changes nothing.
srt4="$(printf 'A+B\nrtopology nope\nnwk\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rt6 "$srt4" "no tree named 'nope'"
chk_contains ti_rt7 "$srt4" '(A,B);'
# rtopology can target a named tree from elsewhere -- the active tree
# stays put.
srt5="$(printf 'A+B\nnew f\nrandom 4 seed=4\nuse main\nrtopology f\nuse main\nnwk\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_rt8 "$srt5" "re-randomised 'f'"
chk_contains ti_rt9 "$srt5" '(A,B);'                            # main untouched

# Polytomy import: a Newick with a polytomy (>2 children at one node) is
# left-folded into binary joins. Walk used to use the whole-node implicit
# label as the running left operand of intermediate joins, so the resolver
# saw the multi-taxa name referenced before any join built it
# (AMBIGUOUS_CLADE). Reading a 21-way root polytomy must produce a valid
# binary tree on the same 21 tips.
cat > "$TMP/poly.nwk" <<'EOF'
(t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,t11,t12,t13,t14,t15,t16,t17,t18,t19,t20,t21);
EOF
exit_is ti_poly1 0 --read "$TMP/poly.nwk" --newick-only
poly="$("$BIN" --read "$TMP/poly.nwk" --newick-only 2>&1)"
# all 21 tips present in the output Newick
for tip in t1 t11 t21; do
    chk_contains "ti_poly_$tip" "$poly" "$tip"
done
[[ "$poly" != *"AMBIGUOUS_CLADE"* ]] && pass=$((pass+1)) || \
    { fail=$((fail+1)); echo "FAIL ti_poly_err: AMBIGUOUS_CLADE on polytomy import"; }
# Inner polytomy too (not just the root) -- a 3-way clade should fold cleanly.
cat > "$TMP/inner.nwk" <<'EOF'
((a,b,c),(d,e));
EOF
exit_is ti_poly_inner 0 --read "$TMP/inner.nwk" --newick-only

# 'labels off' suppresses internal-node labels in display; tips and markers
# still render. The state is sticky across displays until toggled back.
slbl="$(printf 'A+B\nC+D\nA_B+C_D\ndisplay ascii\nlabels off\ndisplay ascii\nlabels on\ndisplay ascii\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_lbl1 "$slbl" 'A_B_C_D'            # shown by default
chk_contains ti_lbl2 "$slbl" 'internal labels: hidden'
chk_contains ti_lbl3 "$slbl" 'internal labels: shown'
# After 'labels off', the bare-+ line for the root has no trailing label.
if echo "$slbl" | grep -qE '^\+$|^\+ *$'; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti_lbl4: bare '+' line missing after 'labels off'"; fi
# Migration / introgression markers still render even with labels off.
slbl2="$(printf 'A+B\nC+D\nA_B+C_D\nintrogress C->A\nlabels off\ndisplay ascii\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_lbl5 "$slbl2" 'C H1'
chk_contains ti_lbl6 "$slbl2" 'A '$(printf '\xe2\x87\x9d')'H1'

# --- stacking order in the legend ----------------------------------------
# Two networks that agree on donor/recip/phi/model per event but disagree on
# eNewick nesting -- i.e. on the age order of stacked events on one lineage
# -- must be distinguishable in the display. This was silently collapsed
# before: a Model B stacked ABOVE a Model D on the GBR lineage vs BELOW it
# both looked identical in the legend, hiding the temporal constraint.
so_old="$("$BIN" --read "$FIX/bpp/fig2c-b-over-d.nwk" --display 2>&1)"
so_new="$("$BIN" --read "$FIX/bpp/fig2c-d-over-b.nwk" --display 2>&1)"
chk_contains ti_stack1 "$so_old" 'stacking order (older'
chk_contains ti_stack2 "$so_old" 'H1/H2 '$(printf '\xe2\x86\x92')' nh'     # bidir older
chk_contains ti_stack3 "$so_new" 'nh '$(printf '\xe2\x86\x92')' H1/H2'     # unidir older
# Plain, unstacked networks emit no stacking block.
so_yeast="$("$BIN" --read "$FIX/bpp/yeast-msci.stree" --display 2>&1)"
[[ "$so_yeast" != *"stacking order"* ]] && pass=$((pass+1)) || \
    { fail=$((fail+1)); echo "FAIL ti_stack4: unstacked yeast net shows stacking block"; }
# REPL: 'introgress A->C; introgress B->C' stacks B's pulse under A's on C.
# A was added first so is older in the graph.
so_repl="$(printf 'A+B=AB\nC+D=CD\nAB+CD=R\nintrogress A->C\nintrogress B->C\nintro\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_stack5 "$so_repl" 'stacking order (older'
chk_contains ti_stack6 "$so_repl" 'H1 '$(printf '\xe2\x86\x92')' H2'

# --- view-command target selector ---------------------------------------
# display / newick / block / intro all accept a target tree without switching
# the active tree. 'display TREE' (bare) works for display; '@NAME' works
# uniformly. Before, 'display t1' silently rendered the active tree.
sv="$(printf 'A+B\nnew t2\nC+D\nuse main\ndisplay t2\nnewick t2\nnewick main\ndisplay @t2\nblock @t2\nintro @t2\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_view1 "$sv" '(A,B);'                       # newick main
chk_contains ti_view2 "$sv" '(C,D);'                       # newick t2
chk_contains ti_view3 "$sv" 'species&tree = 2  C  D'       # block @t2 (t2 has C, D)
chk_contains ti_view4 "$sv" '(no introgression events)'    # intro @t2
# Unknown tree name is reported and the command is a no-op.
sv2="$(printf 'A+B\ndisplay @nope\nnewick @nope\nquit\n' | "$BIN" -i 2>&1)"
chk_contains ti_view5 "$sv2" "no tree named 'nope'"

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

# Model D IMPORT now also goes through the graph (recover_introgressions retired):
# read a bidirectional network back, faithfully re-emit it, and recover the
# coupled legend. Re-emission must carry phi but NO tau-parent on the bidir
# nodes, and round-trip idempotently.
echo "$nD" > "$TMP/dimp.nwk"
di="$("$BIN" --read "$TMP/dimp.nwk" --display --ascii 2>&1)"
chk_contains ti63 "$di" "A $(printf '\xe2\x87\x84') B"     # ⇄ legend (donor ⇄ recipient)
chk_contains ti64 "$di" 'model D'
chk_contains ti65 "$di" 'phi=0.3 / 0.1'
dn="$("$BIN" --read "$TMP/dimp.nwk" --newick-only 2>/dev/null)"
chk_contains ti66 "$dn" '&phi=0.3'
chk_contains ti67 "$dn" '&phi=0.1'
if echo "$dn" | grep -q 'tau-parent'; then            # model D nodes carry no tau-parent
    fail=$((fail+1)); echo "FAIL ti68b: model-D re-emit has tau-parent (BPP forbids it)"
else pass=$((pass+1)); fi
echo "$dn" > "$TMP/dimp2.nwk"; dn2=$("$BIN" --read "$TMP/dimp2.nwk" --newick-only 2>/dev/null)
if [[ "$dn" = "$dn2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti68c: model-D import not idempotent"; fi

# --- Import / round-trip ------------------------------------------------

# CLI: --read with a plain Newick file
echo '((A,B),(C,D));' > "$TMP/p.nwk"
out="$("$BIN" --read "$TMP/p.nwk" --newick-only)"
chk_contains ti63 "$out" '((A,B),(C,D));'

# CLI: --read a species&tree+migration block (the .stree --out writes)
"$BIN" --joins 'A+B,C+D,A_B+C_D' --migration 'A->C,B->D' --out "$TMP/m" --quiet
out="$("$BIN" --read "$TMP/m.stree" --display --ascii 2>&1)"
chk_contains ti64 "$out" 'M1:  A → C'
chk_contains ti65 "$out" 'M2:  B → D'

# CLI: --read a full BPP control file (extracts species&tree + migration)
cat > "$TMP/ctl.ctl" <<EOF
seed = 1
species&tree = 4  A  B  C  D
                  2  2  2  2
  ((A,B),(C,D));
migration = 1
  A  C
phiprior = 1 1
EOF
out="$("$BIN" --read "$TMP/ctl.ctl" --newick-only)"
chk_contains ti66 "$out" '((A,B),(C,D));'

# MSC-I round-trip: write the eNewick, read it back, expect byte identity
nwk0="$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A phi=0.3' --newick-only)"
echo "$nwk0" > "$TMP/i.nwk"
nwk1="$("$BIN" --read "$TMP/i.nwk" --newick-only)"
if [[ "$nwk0" = "$nwk1" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti67: MSC-I round-trip not idempotent"
     echo "  out: $nwk0"; echo "  in:  $nwk1"; fi

# Model D round-trip: emit the coupled form, read it, recover bidir, re-emit
nwkD0="$("$BIN" --joins 'A+B,A_B+C' --introgression 'A<->B phi=0.3 phi2=0.1' --newick-only)"
echo "$nwkD0" > "$TMP/d.nwk"
nwkD1="$("$BIN" --read "$TMP/d.nwk" --newick-only)"
if [[ "$nwkD0" = "$nwkD1" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti68: Model D round-trip not idempotent"
     echo "  out: $nwkD0"; echo "  in:  $nwkD1"; fi

# REPL: 'read FILE as NAME' creates a tree from a file
"$BIN" --joins 'A+B,C+D,A_B+C_D' --migration 'A->C' --out "$TMP/r" --quiet
ri="$(printf 'read %s/r.stree as restored\nuse restored\nnewick\nmigration\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti69 "$ri" "into 'restored'"
chk_contains ti70 "$ri" '((A,B),(C,D));'
chk_contains ti71 "$ri" 'M1:  A → C'

# REPL: export -> wipe -> read cycle (the user's stated workflow)
ex="$(printf 'A+B\nC+D\nA_B+C_D\nintrogress A_B->C_D phi=0.2\nblock %s/exp.stree\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti72 "$ex" "wrote species&tree block"
back="$(printf 'read %s/exp.stree\nnewick\nintrogression\nquit\n' "$TMP" | "$BIN" -i 2>&1)"
chk_contains ti73 "$back" '&phi=0.2'
chk_contains ti74 "$back" "A_B $(printf '\xe2\x87\x9d') C_D"   # legend: donor ⇝ recipient

# --- Real BPP example files (yeast + anopheles MSci, from the BPP repo) ---

# Yeast MSci: Model B, Sbay basal -- the bare H has tau-parent=yes (primary
# parent at root), the labelled (Sbay)H has tau-parent=no (introgression
# source at the (Skud,...) clade). bpp-tree must SWAP to recover Sbay basal.
SQ=$(printf '\xe2\x87\x9d')                                 # U+21DD ⇝
yo="$("$BIN" --read "$FIX/bpp/yeast-msci.stree" --display --ascii 2>&1)"
chk_contains ti75 "$yo" 'Sbay_Scer_Skud_Smik_Spar'          # Sbay basal: in root clade label
chk_contains ti76 "$yo" "Skud $SQ Sbay"                     # donor Skud, recipient Sbay
chk_contains ti77 "$yo" 'model B'

# Anopheles MSci: TWO coupled Model B events (h: R->Q, f: A->b clade)
ao="$("$BIN" --read "$FIX/bpp/anopheles-msci.stree" --display --ascii 2>&1)"
chk_contains ti78 "$ao" "R $SQ Q"
chk_contains ti79 "$ao" "A $SQ b"
chk_contains ti80 "$ao" 'model B'

# Re-emit is idempotent for both files
n1=$("$BIN" --read "$FIX/bpp/yeast-msci.stree" --newick-only)
echo "$n1" > "$TMP/y.nwk"; n2=$("$BIN" --read "$TMP/y.nwk" --newick-only)
if [[ "$n1" = "$n2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti81: yeast MSci re-read not idempotent"; fi

n1=$("$BIN" --read "$FIX/bpp/anopheles-msci.stree" --newick-only)
echo "$n1" > "$TMP/a.nwk"; n2=$("$BIN" --read "$TMP/a.nwk" --newick-only)
if [[ "$n1" = "$n2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti82: anopheles MSci re-read not idempotent"; fi

# Ghost introgression (ARGmigrationROC): 3% GHOST -> AFR, written BPP-style with
# phi=0.97 on AFR's NATIVE parent edge. Per BPP, the donor (GHOST) contribution
# is 1-phi = 0.03; bpp-tree must report phi=0.03, not 0.97 (regression guard for
# the phi-orientation bug -- phi sat on the def/recipient-primary occurrence).
go="$("$BIN" --read "$FIX/bpp/ghost-msci.stree" --display --ascii 2>&1)"
chk_contains ti83 "$go" "GHOST $SQ AFR"        # direction: donor GHOST, recipient AFR
chk_contains ti84 "$go" 'phi=0.03'             # donor contribution = 1 - 0.97
chk_contains ti85 "$go" 'model B'
if echo "$go" | grep -q 'phi=0.97'; then
    fail=$((fail+1)); echo "FAIL ti86: ghost phi not complemented (reported 0.97, donor gets 1-phi)"
else pass=$((pass+1)); fi
# Re-emit puts phi on the GHOST/donor side and is idempotent
n1=$("$BIN" --read "$FIX/bpp/ghost-msci.stree" --newick-only)
echo "$n1" > "$TMP/g.nwk"; n2=$("$BIN" --read "$TMP/g.nwk" --newick-only)
if [[ "$n1" = "$n2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti87: ghost MSci re-read not idempotent"; fi

# Neanderthal/Akey trees: MULTIPLE introgression events in one network.
# m1 = single event (MOD->NEAN); m2 = two NESTED events (the nh_hyb donor ref
# sits inside hn_hyb's recipient clade). These guard two multi-event import
# bugs: a heap-use-after-free from labels[] aliasing freed tree strings, and a
# polluted implicit label ('..._nh_hyb') when a clade encloses another event's
# bare hybrid reference. Both must read cleanly and round-trip idempotently.
m1="$("$BIN" --read "$FIX/bpp/neander-m1.stree" --display --ascii 2>&1)"
chk_contains ti88 "$m1" "MOD $SQ NEAN"
chk_contains ti89 "$m1" 'phi=0.05'
n1=$("$BIN" --read "$FIX/bpp/neander-m1.stree" --newick-only)
echo "$n1" > "$TMP/m1.nwk"; n2=$("$BIN" --read "$TMP/m1.nwk" --newick-only)
if [[ "$n1" = "$n2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti90: neander-m1 re-read not idempotent"; fi

m2="$("$BIN" --read "$FIX/bpp/neander-m2.stree" --display --ascii 2>&1)"
chk_contains ti91 "$m2" "MOD $SQ NEAN"          # event hn_hyb
chk_contains ti92 "$m2" "VC $SQ CEU"            # event nh_hyb (the nested one)
chk_contains ti93 "$m2" 'phi=0.02'              # donor VC contributes 2% to CEU
if echo "$m2" | grep -q '_nh_hyb'; then          # implicit label must not leak the hybrid token
    fail=$((fail+1)); echo "FAIL ti94: neander-m2 implicit label leaked a hybrid token"
else pass=$((pass+1)); fi
n1=$("$BIN" --read "$FIX/bpp/neander-m2.stree" --newick-only)
echo "$n1" > "$TMP/m2.nwk"; n2=$("$BIN" --read "$TMP/m2.nwk" --newick-only)
if [[ "$n1" = "$n2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti95: neander-m2 re-read not idempotent"; fi

# m3 = STACKED: two MOD->NEAN pulses on one lineage (hn2 inner, hn1 outer) plus
# VC->CEU. The flat IntroList cannot represent this; the old model failed with
# INTROGRESSION_UNKNOWN ('mod_pre2' not in the tree). It is now carried as the
# graph and re-emitted by direct serialisation -- the step-2 payoff. It must
# read (exit 0), re-emit the stacked structure faithfully, and round-trip.
exit_is ti96 0 --read "$FIX/bpp/neander-m3.stree" --newick-only
m3="$("$BIN" --read "$FIX/bpp/neander-m3.stree" --newick-only 2>/dev/null)"
chk_contains ti97 "$m3" ')hn2[&tau-parent=yes])hn1[&tau-parent=yes]'  # stacked recipient lineage
chk_contains ti98 "$m3" 'hn2[&phi=0.005,&tau-parent=no]'             # inner pulse, donor side
chk_contains ti99 "$m3" 'hn1[&phi=0.05,&tau-parent=no]'             # outer pulse, donor side
echo "$m3" > "$TMP/m3.nwk"; m3b=$("$BIN" --read "$TMP/m3.nwk" --newick-only 2>/dev/null)
if [[ "$m3" = "$m3b" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti100: neander-m3 re-read not idempotent"; fi
# the species&tree block carries the stacked network too
b3="$("$BIN" --read "$FIX/bpp/neander-m3.stree" 2>/dev/null)"
chk_contains ti101 "$b3" 'species&tree = 6'
chk_contains ti102 "$b3" 'hn2[&phi=0.005'

# Display + legend for the stacked network are DERIVED FROM THE GRAPH (step 3).
# The two MOD->NEAN pulses share one lineage -- NEAN is the recipient of both
# and MOD the donor of both -- which the flat event list could never show.
d3="$("$BIN" --read "$FIX/bpp/neander-m3.stree" --display --ascii 2>&1)"
chk_contains ti103 "$d3" "MOD $SQ NEAN"            # both pulses MOD -> NEAN
chk_contains ti104 "$d3" "VC $SQ CEU"              # third event VC -> CEU
chk_contains ti105 "$d3" 'hn1:  MOD'               # legend names each event
chk_contains ti106 "$d3" 'hn2:  MOD'
chk_contains ti107 "$d3" 'phi=0.005'               # inner pulse phi in legend
chk_contains ti108 "$d3" 'phi=0.02'                # VC->CEU phi
chk_contains ti109 "$d3" "NEAN ${SQ}hn1"           # NEAN bears a recipient marker
chk_contains ti110 "$d3" "MOD hn1${SQ}"            # MOD bears a donor marker

# --- CONSTRUCTING stacked networks (step 4) ------------------------------
# Users can now BUILD a stacked network from a base tree + ordered introgress
# lines with '= NAME'. Two pulses share the recipient lineage (MOD->NEAN twice),
# which introlist_apply rejects but the graph constructor handles: insert a
# hybrid immediately above the recipient (latest innermost) and an anonymous
# donor-attachment above the donor.
CBASE='ALTAI+VC=NEAN,VINDIJA+CHAG=VC,YRI+CEU=MOD,NEAN+MOD=HN,HN+DENISOVA'
CINTRO='MOD->NEAN=hn1 phi=0.05 src=node dst=branch, MOD->NEAN=hn2 phi=0.005 src=node dst=branch, VC->CEU=nh phi=0.02 src=node dst=branch'
exit_is ti111 0 --joins "$CBASE" --introgression "$CINTRO" --newick-only
cn="$("$BIN" --joins "$CBASE" --introgression "$CINTRO" --newick-only 2>/dev/null)"
chk_contains ti112 "$cn" ')hn2[&tau-parent=yes])hn1[&tau-parent=yes]'  # stacked recipient
chk_contains ti113 "$cn" 'hn2[&phi=0.005,&tau-parent=no]'             # inner donor pulse
chk_contains ti114 "$cn" 'hn1[&phi=0.05,&tau-parent=no]'             # outer donor pulse
echo "$cn" > "$TMP/cn.nwk"; cn2=$("$BIN" --read "$TMP/cn.nwk" --newick-only 2>/dev/null)
if [[ "$cn" = "$cn2" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti115: constructed stacked net not idempotent"; fi
# the donor bare ref precedes its subtree on each attachment (BPP phi pairing)
chk_contains ti116 "$cn" '(hn1[&phi=0.05,&tau-parent=no],(hn2[&phi=0.005'
cd3="$("$BIN" --joins "$CBASE" --introgression "$CINTRO" --display --ascii 2>&1)"
chk_contains ti117 "$cd3" "MOD $SQ NEAN"
chk_contains ti118 "$cd3" "VC $SQ CEU"

# '= NAME' is optional -- events auto-name H1, H2, ...
ca="$("$BIN" --joins "$CBASE" --introgression 'MOD->NEAN phi=0.05, MOD->NEAN phi=0.005' --display --ascii 2>&1)"
chk_contains ti119 "$ca" 'H1:  MOD'
chk_contains ti120 "$ca" 'H2:  MOD'

# Referencing a prior event's name as an endpoint attaches above its hybrid
# node (order override): hn1 sits ABOVE hn2 on the recipient lineage.
co="$("$BIN" --joins "$CBASE" --introgression 'MOD->NEAN=hn2 phi=0.005 src=node dst=branch, MOD->hn2=hn1 phi=0.05 src=node dst=branch' --newick-only 2>/dev/null)"
chk_contains ti121 "$co" ')hn2[&tau-parent=yes])hn1[&tau-parent=yes]'

# Name validation: no collision with a tip/clade, no '_'.
chk_contains ti122 "$("$BIN" --joins "$CBASE" --introgression 'MOD->NEAN=NEAN phi=0.05, MOD->NEAN=h2 phi=0.01' 2>&1)" 'already a tip, clade, or event'
chk_contains ti123 "$("$BIN" --joins "$CBASE" --introgression 'MOD->NEAN=h_1 phi=0.05, MOD->NEAN=h2 phi=0.01' 2>&1)" "must not contain '_'"

# A reciprocal pair is still rejected (not stacking; use a <-> event).
chk_contains ti124 "$("$BIN" --joins 'A+B,A_B+C' --introgression 'C->A ; A->C' 2>&1)" 'more than one event'

# Unification: every MSC-I import (incl. model A, both tau-parent=yes) now goes
# through the graph. Re-reading a model-A network must preserve the DONOR's phi
# (0.2), not its complement -- a regression guard for the normalized-phi path.
"$BIN" --joins 'A+B,C+D,A_B+C_D' --introgression 'A_B->C_D phi=0.2' --out "$TMP/ma" --quiet 2>/dev/null
maL="$("$BIN" --read "$TMP/ma.stree" --display --ascii 2>&1)"
chk_contains ti132 "$maL" "A_B $SQ C_D"
chk_contains ti133 "$maL" 'phi=0.2'
chk_contains ti134 "$maL" '[model A]'

# --- EDITING a stacked (graph-carried) network (step 5) ------------------
# Editing the base tree must re-pin the events: the events are derived from the
# graph, the edit lands on the base tree, and the network is rebuilt. Before
# step 5 the edit was silently ignored (the stale graph was emitted).
pm3="$("$BIN" --read "$FIX/bpp/neander-m3.stree" --prune DENISOVA --newick-only 2>/dev/null)"
if echo "$pm3" | grep -q DENISOVA; then
    fail=$((fail+1)); echo "FAIL ti125: --prune on stacked net was ignored (DENISOVA still present)"
else pass=$((pass+1)); fi
chk_contains ti126 "$pm3" ')hn2[&tau-parent=yes])hn1[&tau-parent=yes]'   # stacking survives the edit
chk_contains ti127 "$pm3" 'nh_hyb[&phi=0.02'                            # all three events re-pinned
echo "$pm3" > "$TMP/pm3.nwk"; pm3b=$("$BIN" --read "$TMP/pm3.nwk" --newick-only 2>/dev/null)
if [[ "$pm3" = "$pm3b" ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti128: pruned stacked net not idempotent"; fi
exit_is ti129 0 --read "$FIX/bpp/neander-m3.stree" --prune DENISOVA --newick-only
# the UNEDITED import is still emitted faithfully (preserves the input labels)
chk_contains ti130 "$("$BIN" --read "$FIX/bpp/neander-m3.stree" --newick-only 2>/dev/null)" 'mod_pre'
# an edit that removes an event endpoint auto-drops the orphaned event(s)
# with a warning; the edit goes through (so the read is not refused).
exit_is ti131 0 --read "$FIX/bpp/neander-m3.stree" --prune CEU --newick-only
chk_contains ti131a "$("$BIN" --read "$FIX/bpp/neander-m3.stree" --prune CEU --newick-only 2>&1)" INTROGRESSION_DROPPED
# the resulting tree has no event annotations (all three referenced CEU,
# directly or via the MOD clade it lived in)
ti131b=$("$BIN" --read "$FIX/bpp/neander-m3.stree" --prune CEU --newick-only 2>/dev/null)
if [[ "$ti131b" != *'[&phi'* && "$ti131b" != *'tau-parent'* ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti131b: pruned-orphan tree still carries event annotations"; fi

# Single-event drop: yeast has one event Skud->Sbay; pruning either endpoint
# drops the event and emits the plain base tree.
exit_is ti131c 0 --read "$FIX/bpp/yeast-msci.stree" --prune Sbay --newick-only
ti131d=$("$BIN" --read "$FIX/bpp/yeast-msci.stree" --prune Sbay --newick-only 2>&1)
chk_contains ti131e "$ti131d" "INTROGRESSION_DROPPED"
chk_contains ti131f "$ti131d" "Sbay' is no longer in the tree"
ti131g=$("$BIN" --read "$FIX/bpp/yeast-msci.stree" --prune Sbay --newick-only 2>/dev/null)
if [[ "$ti131g" != *'[&phi'* ]]; then pass=$((pass+1))
else fail=$((fail+1)); echo "FAIL ti131g: post-prune tree still annotated"; fi

# Partial drop: anopheles has TWO events (h: R->Q, f: A->b). Pruning Q kills
# event h but event f survives -- the resulting network must still carry f.
exit_is ti131h 0 --read "$FIX/bpp/anopheles-msci.stree" --prune Q --newick-only
ti131i=$("$BIN" --read "$FIX/bpp/anopheles-msci.stree" --prune Q --newick-only 2>&1)
chk_contains ti131j "$ti131i" "event h"
ti131k=$("$BIN" --read "$FIX/bpp/anopheles-msci.stree" --prune Q --newick-only 2>/dev/null)
chk_contains ti131l "$ti131k" '[&phi=0.7'

# And bpp-lint accepts the re-emitted forms (semantic equivalence to originals)
if [[ -x "$LINT" ]]; then
    for f in "$FIX/bpp/yeast-msci.stree" "$FIX/bpp/anopheles-msci.stree" "$FIX/bpp/ghost-msci.stree" \
             "$FIX/bpp/neander-m1.stree" "$FIX/bpp/neander-m2.stree" "$FIX/bpp/neander-m3.stree"; do
        nwk=$("$BIN" --read "$f" --newick-only)
        species=$(awk '/species&tree/{$1=$2=$3=$4="";print;exit}' "$f")
        n=$(echo "$species" | wc -w | tr -d ' ')
        counts=$(awk -v n=$n 'BEGIN{for(i=0;i<n;i++)printf "1 ";print ""}')
        cat > "$TMP/lt.ctl" <<EOF
seed = 1
seqfile = x
Imapfile = x.imap
jobname = out
nloci = 100
species&tree = $n $species
                  $counts
  $nwk
phiprior = 1 1
thetaprior = 3 0.004
tauprior = 3 0.002
burnin = 1000
sampfreq = 2
nsample = 10000
EOF
        if "$LINT" "$TMP/lt.ctl" 2>&1 | grep -qE '(^|: )error:'; then
            fail=$((fail+1)); echo "FAIL: bpp-lint rejects re-emit of $(basename "$f")"
        else pass=$((pass+1)); fi
    done
fi

# --- Network graph: faithful eNewick build + idempotent re-serialise -----
# The graph model (src/graph.c) is the faithful replacement for the heuristic
# recover_introgressions. tests/graph_roundtrip builds the graph from each
# fixture, re-serialises it, and re-parses+re-serialises the result; exit 0
# means the graph built and "parse -> graph -> string" is byte-stable. Unlike
# the legacy IntroList path, the graph represents STACKED introgressions, so it
# reads the M3 network (two MOD->NEAN pulses on one lineage) that the old model
# could not. Idempotency here is the structural oracle for "robust to every
# eNewick"; bpp's own reading is checked separately where a bpp binary exists.
GRT="$ROOT/tests/graph_roundtrip"
if [[ -x "$GRT" ]]; then
    for f in yeast-msci anopheles-msci ghost-msci neander-m1 neander-m2 neander-m3; do
        if "$GRT" "$FIX/bpp/$f.stree" >/dev/null 2>&1; then pass=$((pass+1))
        else fail=$((fail+1)); echo "FAIL gt-$f: graph build/round-trip (idempotency) failed"; fi
    done
    # M3 structure: the recipient (NEAN) lineage stacks hn2 then hn1; both donor
    # pulses ride MOD's lineage (mod_pre2/mod_pre1); VC->CEU is the third event.
    m3o="$("$GRT" "$FIX/bpp/neander-m3.stree" 2>/dev/null)"
    chk_contains gt-m3a "$m3o" ')hn2[&tau-parent=yes])hn1[&tau-parent=yes]'  # stacked recipient
    chk_contains gt-m3b "$m3o" 'hn2[&phi=0.005,&tau-parent=no]'             # inner donor pulse
    chk_contains gt-m3c "$m3o" 'hn1[&phi=0.05,&tau-parent=no]'              # outer donor pulse
    chk_contains gt-m3d "$m3o" '(CEU)nh_hyb[&tau-parent=yes]'               # VC->CEU recipient
    # Canonicalisation: phi sitting on a recipient-primary occurrence is moved to
    # the donor-side bare ref and complemented (ghost 0.97 -> 0.03 on GHOST side).
    go="$("$GRT" "$FIX/bpp/ghost-msci.stree" 2>/dev/null)"
    chk_contains gt-ghost "$go" 'H[&phi=0.03,&tau-parent=no]'
else
    echo "skip gt: tests/graph_roundtrip not built (run 'make test')"
fi

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
