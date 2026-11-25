import Lake

open System Lake DSL

package test

/-- Writes `Gen/Info.lean` from `input.txt`, rewriting it only when its text changes. -/
target info pkg : Unit := Job.async do
  let path := pkg.dir / "Gen" / "Info.lean"
  let input ← IO.FS.readFile (pkg.dir / "input.txt")
  let text := s!"def Gen.Info.value : String := {repr input.trimAscii.toString}\n"
  let old? ← if (← path.pathExists) then some <$> IO.FS.readFile path else pure none
  unless old? == some text do
    IO.FS.writeFile path text

@[default_target] lean_lib Gen where
  globs := #[.submodules `Gen]
  needs := #[info]
