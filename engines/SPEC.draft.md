# cairn.engine/v1-draft: engine profiles and the formula language

**DRAFT AND PROVISIONAL.** `contracts/engine/v1` is not released
([cairn-driving-log-selfhosted#20](https://github.com/ParkWardRR/cairn-driving-log-selfhosted/issues/20)).
Until it is, the schema (`engine.schema.draft.json`), this text and the vectors
(`vectors/`) live beside the profiles they describe. All three should move into the
contract, and this directory should then follow the released version. Nothing here is a
stable interface; the schema id is `cairn.engine/v1-draft` so that no profile can be
mistaken for a released one.

## What a profile is

One file per engine, `engines/<engine_id>.yaml`, where `<engine_id>` equals the `engine_id`
field. It says how to read that engine's ECU and what its electrical and sleep behaviour
means; `tools/enginegen` validates it and compiles it into C tables. Fields are in
`engine.schema.draft.json`; the points the schema cannot state:

* **`unknown` is an answer; a missing key is an error.** A section or value nobody has
  established is written `unknown`. The firmware treats it as "no data" and uses its own
  device default (and reports which values came from the profile), never another engine's
  number.
* **`status`** is `stub` (identity only; the generator rejects a stub that states anything),
  `derived` (extracted from behaviour the firmware already had) or `verified` (checked
  against raw ECU replies from the real car).
* **PIDs.** `tier: hot` PIDs share one multi-PID Mode 01 request, in file order, at most six.
  `tier: cold` PIDs are requested one at a time, one `cold_slot` per cycle, out of
  `cadence.cold_slots`. One PID per capture-record `field`.
* **A formula yields the value exactly as the capture record stores it**, including any
  saturation, so the table describes the data on the card. `range` is the `[min, max]` the
  formula can produce, and the generator proves it from the expression.
* **Identity.** The profile hash is SHA-256 of the file's bytes with CRLF read as LF. The
  build identity is SHA-256 of `cairn.engine-build/v1-draft\n` followed, for each selected
  engine in `engine_id` order, by `<engine_id>\n<version>\n<hex hash>\n`. The firmware carries
  both and logs `engines=<id>@<version>/<hash16>,... build=<hash16>` at boot.

## Formula language

Integer-only, no state, no loops, no calls out: it cannot execute anything, and always
terminates. Every value is an **int32** and every operation's result must lie in int32 or
evaluation fails; nothing wraps. Implementations in C, Go, Swift or TypeScript therefore agree
exactly (a TypeScript double computes the exact result of any int32 operation up to the point
it leaves the range, and leaving the range is an error either way).

```
expr     := or
or       := and ( '|' and )*
and      := shift ( '&' shift )*
shift    := add ( ('<<' | '>>') add )*
add      := mul ( ('+' | '-') mul )*
mul      := unary ( ('*' | '/' | '%') unary )*
unary    := '-' unary | primary
primary  := INT | VAR | FUNC '(' expr (',' expr)* ')' | '(' expr ')'
INT      := decimal or 0x-hex literal, at most 2147483647
VAR      := A | B | C | D          the reply's data bytes, 0..255, in order
FUNC     := min(x,y) | max(x,y) | clamp(x,lo,hi)      clamp = min(max(x,lo),hi)
```

Precedence and associativity are C's. Whitespace is ignored. No comparisons, no ternary, no
floating point, no identifiers other than those above. At most 256 characters, nesting at most
32.

* `/` truncates toward zero and `%` takes the sign of the dividend (as C99); division or
  remainder by zero is an error.
* `>>` is arithmetic (it floors); `<<` multiplies by 2^n and is range-checked. A shift count
  outside 0..31 is an error.
* `&` and `|` act on the two's-complement int32 values.

**Static checks (compile time, in `enginegen`).** From interval analysis over the variables
the PID's `bytes` provides (each 0..255, the rest unavailable): a formula may not use a
variable beyond `bytes`; a divisor or modulus interval must exclude 0 (write
`A / max(B, 1)`); a shift count must be a constant in 0..31; no intermediate may be able to
leave int32; and the result interval must lie inside the declared `range`. So an accepted
formula cannot fail at run time on any reply, and evaluators still check, because bytecode in
flash is only as trustworthy as the flash.

## Bytecode

A stack machine, 16 entries deep, no jumps. The program must leave exactly one value.

| code | op | | code | op |
|---|---|---|---|---|
| 01 | push u8 (imm8) | | 25 | neg |
| 02 | push u16 (imm16 LE) | | 26 | and |
| 03 | push i32 (imm32 LE) | | 27 | or |
| 04 | push s8 (imm8, sign-extended) | | 28 | shl |
| 10..13 | load A..D | | 29 | shr |
| 20 | add | | 30 | min |
| 21 | sub | | 31 | max |
| 22 | mul | | 32 | clamp (x lo hi) |
| 23 | div | | | |
| 24 | mod | | | |

Binary ops pop `b` then `a` and push `a op b`. Runtime errors: `ERR_TRUNC` (immediate runs off
the end), `ERR_BAD_OP`, `ERR_STACK` (underflow or deeper than 16), `ERR_DIV_ZERO`,
`ERR_OVERFLOW`, `ERR_SHIFT`, `ERR_RESULT` (not exactly one value left).

The compiler picks the shortest push: `0..255` push u8, `-128..-1` push s8, `256..65535`
push u16, anything else push i32. A negated literal is a literal. That is part of the vectors,
so another compiler can be held to them.

## Vectors

`vectors/expr.draft.txt`: `E` lines (expression, inputs, expected value and the exact
bytecode), `X` lines (expressions the compiler must reject, and why) and `B` lines (raw
bytecode and the value or error an evaluator must produce, the cases a compiler never emits).
`vectors/invalid/*.yaml`: profiles that must be rejected, each headed by
`# expect-error: <text the rejection must contain>`. The valid vectors are the profiles
themselves.

Two implementations run them: the Rust generator (`make engines-test`) and the C evaluator
(`make -C test/host engine`). The contract needs a third, independent consumer before release.
