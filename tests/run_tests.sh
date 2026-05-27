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

echo
echo "passed: $pass   failed: $fail"
[[ "$fail" == 0 ]]
