import Gen.Info

/-! Imports the module that the library's `needs` target generates. -/

def Gen.label : String := s!"value: {Gen.Info.value}"
