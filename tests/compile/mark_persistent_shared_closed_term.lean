/-!
Stress test of a closed term whose value running tasks already share. The compiler extracts `T.get`
in `viaClosed` as a closed term, which the runtime initializes on the first thread that reads it and
then marks persistent (`lean_obj_once_cold`). By then `T`'s value is shared with the workers, which
keep taking and dropping references to it. A marking that stored a count of 0 there let an
adjustment already past its check land on that 0, and a later drop freed the value while `T` still
pointed to it (seen as heap corruption). `test_mark_persistent_shared` in
`tests/misc_dir/rc_sticky` replays the race deterministically; this test lets it happen, so it
fails only some of the time on a runtime with the bug (16 of 30 runs on an Apple M1).

The interpreter does not extract closed terms, hence `.no_interpret_test`.
-/

/-- A few small heap objects, built at run time (`never_extract`: not a closed term of its own,
so persistent only through `viaClosed`). -/
@[never_extract, noinline] def build (_ : Unit) : Array String :=
  (Array.range 64).map fun i => toString (i * 1000003)

def T : Thunk (Array String) := Thunk.mk fun u => build u

/-- `T.get`, which the compiler extracts as a closed term. -/
@[noinline] def viaClosed (_ : Unit) : Array String := T.get

/-- Takes and drops references to `T`'s elements many times. -/
@[noinline] def churn (t : Thunk (Array String)) (k : Nat) : Nat := Id.run do
  let a := t.get
  let mut s := 0
  for r in [0:k] do
    let mut out : Array String := #[]
    for e in a do
      out := out.push e
    s := s + out.size + r
  return s

/-- Forces a thunk passed as an argument (no closed term). -/
@[noinline] def force (t : Thunk (Array String)) : Nat := t.get.size

def main : IO Unit := do
  -- Force `T` on a task: its value is then shared.
  let forced ← IO.asTask (prio := .dedicated) do return force T
  discard <| IO.ofExcept (← IO.wait forced)
  let mut started := #[]
  let mut workers := #[]
  for j in [0:8] do
    let p ← IO.Promise.new
    started := started.push p
    workers := workers.push (← IO.asTask (prio := .dedicated) do
      p.resolve ()
      pure (churn T (200000 + j)))
  -- Once every worker runs, a task reads `viaClosed` first.
  let late ← IO.asTask (prio := .dedicated) do
    for p in started do IO.wait p.result!
    pure (viaClosed ()).size
  let mut total := 0
  for w in workers do
    total := total + (← IO.ofExcept w.get)
  IO.println s!"{total} {← IO.ofExcept late.get}"
