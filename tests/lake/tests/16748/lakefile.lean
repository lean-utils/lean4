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

/--
Reads the header of every module of `Checked`, the library that needs it, and fails on an
import from outside that library and `Init`: a check of the imports before any module compiles.
-/
target checkImports : Unit := do
  let some lib ← findLeanLib? `Checked
    | error "no library Checked"
  let jobs ← (← lib.getModuleArray).mapM fun mod => do
    return (← mod.header.fetch).map fun header => (mod.name, header.imports.map (·.module))
  (Job.collectArray jobs).mapM fun headers => do
    for (mod, imps) in headers do
      for imp in imps do
        unless imp.getRoot == `Checked || imp.getRoot == `Init do
          error s!"{mod} imports {imp}"

@[default_target] lean_lib Checked where
  globs := #[.submodules `Checked]
  needs := #[checkImports]
