#!/usr/bin/env bash
source ../common.sh

./clean.sh

# This test covers issue 16748
# https://github.com/leanprover/lean4/issues/16748

# Tests that Lake runs a library's `needs` before it reads the sources of the
# library's modules, so that a `needs` target can generate one of them.
# Reading a source first would miss a generated module on a fresh checkout
# ("bad import"), and on a regenerated module it would keep the old build
# or fail with "changed since build began".

# The build writes `Gen/Info.lean`, so work on a copy
copy_to_work lakefile.lean Gen Main.lean

# `lake env lean --run` loads the library as built, without building anything
echo "# TEST: generated module absent"
test_exp ! -f Gen/Info.lean
echo one > input.txt
test_run build
test_out "value: one" env lean --run Main.lean

echo "# TEST: generated module changed"
echo two > input.txt
test_run build
test_out "value: two" env lean --run Main.lean

echo "# TEST: generated module changed again"
echo three > input.txt
test_run build
test_out "value: three" env lean --run Main.lean

# Cleanup
rm -f produced.out
