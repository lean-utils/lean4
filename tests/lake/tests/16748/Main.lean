import Gen.Use

/-! Prints the value that the built library holds for the generated module. -/

def main : IO Unit := IO.println Gen.label
