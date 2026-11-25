#!/usr/bin/env bash
source ../common.sh

./clean.sh

# This test covers issue 16748
# https://github.com/leanprover/lean4/issues/16748

# Tests that Lake runs a library's `needs` before it reads the library's import
# graph, so that a `needs` target can generate one of the library's modules.
# Reading a source first would miss a generated module on a fresh checkout
# ("bad import"), and on a regenerated module it would keep the old build
# or fail with "changed since build began".
# Also tests that a `needs` target may still read the headers of its own
# library's modules (here, to check their imports) without a build cycle.

# The build writes `Gen/Info.lean`, so work on a copy
copy_to_work lakefile.lean Gen Checked Main.lean

# `lake env lean --run` loads the library as built, without building anything
echo "# TEST: generated module absent"
test_exp ! -f Gen/Info.lean
echo one > input.txt
test_not_out "build cycle" build
test_out "value: one" env lean --run Main.lean

echo "# TEST: generated module changed"
echo two > input.txt
test_run build
test_out "value: two" env lean --run Main.lean

echo "# TEST: generated module changed again"
echo three > input.txt
test_run build
test_out "value: three" env lean --run Main.lean

echo "# TEST: generated module unchanged"
# `--no-build` fails if a module would be rebuilt
test_run build --no-build
test_out "value: three" env lean --run Main.lean

echo "# TEST: a library's needs target reads the library's headers"
test_exp -f .lake/build/lib/lean/Checked/A.olean
echo "import Gen.Use" > Checked/C.lean
test_err "Checked.C imports Gen.Use" build Checked
test_exp ! -f .lake/build/lib/lean/Checked/C.olean
rm Checked/C.lean
test_not_out "build cycle" build Checked

# Cleanup
rm -f produced.out
