import Std.Http

open Std Http
open Std.Http.Protocol.H1

/-!
Tests where a request-target ends. The target ends at the space before the version
(RFC 9112 §3); a byte that cannot appear in a request-target (RFC 9112 §3.2, RFC 3986 §2), such as
`CR`, `LF` or `DEL`, makes the request line invalid. The server machine answers `400 Bad Request` as
soon as it reads such a byte, without waiting for a space or for `maxUriLength` bytes, and it
does not accept the target shortened at that byte. A valid request and an overlong target (`414`)
behave as before, and every outcome is the same wherever a read ends.
-/

/-- What a server machine did with its input. -/
structure Outcome where
  /-- The first line it wrote, empty if it wrote nothing. -/
  status : String
  /-- Whether it reported a failure. -/
  failed : Bool
  /-- Whether it read a whole message head. -/
  endHeaders : Bool
  deriving BEq, Repr

/--
Feeds `parts` in order to a fresh server machine without ending the input and returns what it did.
After each part it steps the machine until the machine settles: it asks for more input
(`needMoreData`) or has read a message head (`endHeaders`), or, once it has reported a failure,
until it has halted, which it does after writing its error response. Returns `none` if the machine
has not settled after 16 steps for a part.
-/
def run (parts : List ByteArray) (config : Protocol.H1.Config := {}) : Option Outcome := Id.run do
  let mut m : Machine .receiving := { config }
  let mut out := ByteArray.empty
  let mut events : Array (Event .receiving) := #[]
  for part in parts do
    m := m.feed part
    let mut settled := false
    for _ in [0:16] do
      let (m', r) := m.step
      m := m'
      out := out ++ r.output.toByteArray
      events := events ++ r.events
      let failed := events.any (· matches .failed _)
      if m.halted || (!failed && r.events.any (· matches .needMoreData _ | .endHeaders _)) then
        settled := true
        break
    unless settled do return none
  return some {
    status := ((String.fromUTF8! out).splitOn "\r\n").head!
    failed := events.any (· matches .failed _)
    endHeaders := events.any (· matches .endHeaders _) }

def fail (name : String) (msg : String) : IO Unit :=
  throw <| IO.userError s!"[{name}] {msg}"

/--
Checks that `input` fed in one read gives `expected`, and that it gives the same when fed in two
reads split at every byte boundary.
-/
def expect (name : String) (input : String) (expected : Outcome) (config : Protocol.H1.Config := {}) :
    IO Unit := do
  let bytes := input.toUTF8
  let whole := run [bytes] config
  unless whole == some expected do
    fail name s!"expected {repr expected}, got {repr whole}"
  for k in [1:bytes.size] do
    let split := run [bytes.extract 0 k, bytes.extract k bytes.size] config
    unless split == some expected do
      fail name s!"split at byte {k}: expected {repr expected}, got {repr split}"

def bad : Outcome := { status := "HTTP/1.1 400 Bad Request", failed := true, endHeaders := false }
def tooLong : Outcome := { status := "HTTP/1.1 414 URI Too Long", failed := true, endHeaders := false }
def headRead : Outcome := { status := "", failed := false, endHeaders := true }
def waiting : Outcome := { status := "", failed := false, endHeaders := false }

#eval do
  -- No space between the target and the version; the input holds no space after the target.
  expect "no SP before the version" "GET /xHTTP/1.1\r\nHost:a\r\n\r\n" bad
  -- The request line ends right after the target.
  expect "line ends after the target" "GET /x\r\nHost:a\r\n\r\n" bad
  expect "line ends after the target, nothing more" "GET /x\r\n" bad
  -- A byte that cannot be part of a target in place of the space.
  expect "CR in place of SP" "GET /x\rHTTP/1.1\r\nHost:a\r\n\r\n" bad
  expect "LF in place of SP" "GET /x\nHTTP/1.1\r\nHost:a\r\n\r\n" bad
  expect "DEL in place of SP" "GET /x\x7FHTTP/1.1\r\nHost:a\r\n\r\n" bad
  expect "non-URI character in the target" "GET /x{HTTP/1.1\r\nHost:a\r\n\r\n" bad
  -- A space later in the input does not make the target end there.
  expect "DEL in the target, SP later" "GET /x\x7Fy HTTP/1.1\r\nHost:a\r\n\r\n" bad
  expect "CR in the target, SP later" "GET /x\r\nHost: a\r\n\r\n" bad
  -- The input ends at the offending byte: the line is rejected at that byte, not after more input.
  expect "input ends at CR" "GET /x\r" bad
  expect "input ends at LF" "GET /x\n" bad
  expect "input ends at DEL" "GET /x\x7F" bad
  expect "input ends at a non-URI character" "GET /x{" bad

#eval do
  -- A target cut off by the end of the input is still waited for.
  expect "incomplete target" "GET /x" waiting
  -- A valid request reaches the end of its head.
  expect "valid request" "GET /x?a=b HTTP/1.1\r\nHost:a\r\n\r\n" headRead

-- `maxUriLength` bounds the target: a target of exactly that length is accepted, a longer one gets
-- 414 whether or not a space follows it, and a target of exactly that length followed by a byte
-- that cannot continue it is invalid, not too long.
#eval do
  let config : Protocol.H1.Config := { maxUriLength := 300 }
  let exact := "GET /" ++ String.ofList (List.replicate 299 'a')
  let rest := "HTTP/1.1\r\nHost:a\r\n\r\n"
  expect "exact-limit target, then SP" s!"{exact} {rest}" headRead config
  expect "exact-limit target, then CR" s!"{exact}\r{rest}" bad config
  expect "exact-limit target, then a target byte" s!"{exact}a {rest}" tooLong config
  expect "exact-limit target, then nothing" exact waiting config
  let long := "/" ++ String.ofList (List.replicate 400 'a')
  expect "long target" s!"GET {long} {rest}" tooLong config
  expect "long target, no SP" s!"GET {long}" tooLong config
