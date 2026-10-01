# Understanding the Wide Arithmetic Proposal — a study guide

This document teaches every part of the proposal from the ground up, so you can
explain and defend it in your own words. It is split into:

- **Part A** — the concepts and the pipeline (understand the proposal)
- **Part B** — what we actually implemented (the prototype), mapped to the plan
- **Part C** — likely mentor questions and how to answer them

---

# Part A — Understanding the proposal

## A.0 The 30-second version

WebAssembly can only do math on integers up to 64 bits. Some programs
(cryptography, big integers, blockchain) need 128-bit math. Today they fake it
with many 64-bit instructions. The **Wide Arithmetic** proposal adds 4 real
instructions so the runtime can do 128-bit add/subtract and 64×64→128 multiply
directly. Our job: teach WasmEdge to understand and run these 4 instructions in
all three of its execution modes (interpreter, AOT, JIT).

## A.1 The mental model: WasmEdge is a stack machine

WebAssembly executes on a **value stack**. Instructions don't name registers;
they **pop** their inputs off the top of the stack and **push** their outputs
back. Example — `i64.add`:

```
stack before:  [ ... , a , b ]   (b on top)
i64.add        pops b, pops a, pushes (a+b)
stack after:   [ ... , (a+b) ]
```

Everything in this project is about pushing/popping the stack correctly and
doing the right math in between.

## A.2 What an "instruction" is made of

Each instruction has an **opcode** — a numeric ID. Most are one byte; some live
under a **prefix**. Our four use the `0xFC` prefix followed by a sub-number:

| Instruction | Opcode |
|---|---|
| `i64.add128` | `0xFC 19` |
| `i64.sub128` | `0xFC 20` |
| `i64.mul_wide_s` | `0xFC 21` |
| `i64.mul_wide_u` | `0xFC 22` |

The `0xFC` block in WasmEdge currently stops at `0xFC 17` (`table.fill`), so
19–22 are unused — no collision.

## A.3 The four instructions, in plain terms

A 128-bit number is stored as **two 64-bit halves**: a **low** half and a
**high** half. Wasm has no 128-bit value type, so these instructions pass the
halves as separate `i64`s.

- **`i64.add128`** — add two 128-bit numbers. Takes 4 `i64`s (low+high of each),
  returns 2 `i64`s (low+high of the sum). Signature `[i64 i64 i64 i64] → [i64 i64]`.
- **`i64.sub128`** — same shape, subtraction. `LHS − RHS`.
- **`i64.mul_wide_s`** — multiply two **signed** 64-bit numbers; the product can
  be up to 128 bits. Takes 2 `i64`s, returns 2 `i64`s. `[i64 i64] → [i64 i64]`.
- **`i64.mul_wide_u`** — same, **unsigned**.

**Operand order (from the spec — important):** for `add128`/`sub128` the stack,
read top→bottom, is `RHS_high, RHS_low, LHS_high, LHS_low`. So the first value
you pop is the RHS high half. `v1 = LHS`, `v2 = RHS`, and `sub128` computes
`v1 − v2`.

## A.4 Why the instructions matter (the "why")

**Carry and borrow.** When you add the two low halves and the result is too big
for 64 bits, a **carry** of 1 must be added into the high half. Subtraction has
the mirror image: a **borrow**. Today, Wasm code detects carry by comparing the
sum against an operand (a wrap-around check) and branches — several instructions
per add.

**Hardware already does this for free.** A 64-bit CPU sets a **carry flag** on
overflow and has an **add-with-carry (ADC)** instruction. The problem was only
that Wasm had no way to *say* "128-bit add," so it couldn't reach that hardware.

**The payoff.** With these instructions, WasmEdge's LLVM AOT/JIT can lower
`i64.add128` straight to a native `add` + `adc`. Result: smaller `.wasm` and
much faster big-integer code.

## A.5 The pipeline — the five stages a Wasm instruction passes through

When a `.wasm` file runs, each instruction flows through these stages. To add
four instructions we touch each stage once.

### Stage 1 — Opcode registration (`include/common/enum.inc`)
**What:** a central table lists every opcode. We add four lines for 19–22, and
add a new **proposal** flag `WideArithmetic`.
**Why:** this table is the single source of truth. Adding entries here makes the
opcodes *exist* in the runtime's vocabulary, and — because the loader's decoder
is generated from this same table — makes them **decodable automatically**.
**Key idea to remember:** the decode is table-generated, so one edit here
propagates to the parser.

### Stage 2 — Loader / parser (`lib/loader/ast/instruction.cpp`, `serialize/`)
**What:** the loader turns raw bytes into instruction objects. Our four carry no
extra data (no "immediate operands"), so they just join the group of
"read-the-opcode-and-you're-done" instructions. The **serializer** (the reverse:
objects → bytes) is a manual switch and needs the same four added.
**Why:** without this, the runtime sees the bytes but doesn't know they're
complete instructions.

### Stage 3 — Validator (`lib/validator/formchecker.cpp`)
**What:** before running, WasmEdge type-checks the stack. It uses a generic
helper `StackTrans(Take, Put)` meaning "this instruction takes these types and
puts back these types." We declare:
- `add128`/`sub128`: `StackTrans({I64,I64,I64,I64}, {I64,I64})`
- `mul_wide_*`: `StackTrans({I64,I64}, {I64,I64})`
**Why it's easy (the insight):** returning **two** results looks unusual, but
WasmEdge already supports the **Multi-value** feature, so `StackTrans` already
handles any "N in, M out" shape. No new validator machinery is needed — a common
misconception is that it does.

### Stage 4 — Interpreter (`lib/executor/engine/engine.cpp`)
**What:** the interpreter runs each instruction. We add four cases that pop the
`i64` halves, do the 128-bit math, and push the two result halves.
**The portability insight:** we use `WasmEdge::uint128_t` / `int128_t` from
`include/common/int128.h`. On GCC/Clang this is the native `__int128`; on Windows
(MSVC, which lacks `__int128`) it's a hand-written struct with the same
operators. So the *same* code is correct on every platform — no special-case
intrinsics.

### Stage 5 — LLVM AOT/JIT (`lib/llvm/compiler/numericInstr.cpp`)
**What:** WasmEdge also compiles Wasm to native code via LLVM. Here we emit LLVM
IR: zero/sign-extend the `i64`s to `i128`, do `add`/`sub`/`mul`, then split the
`i128` result back into two `i64`s (`trunc` for the low half, shift-right +
`trunc` for the high half), and push both.
**Why this is the performance win:** LLVM folds that `i128` IR into the native
carry/widening-multiply instructions automatically.

## A.6 Testing — how we prove it's correct

- **Differential testing:** run the same inputs through interpreter, AOT, and JIT
  and assert identical results (carry-boundary cases, signed vs unsigned multiply,
  min/max edges).
- **Upstream spec tests:** the official proposal ships a `wide-arithmetic.wast`
  with ~99 assertions. WasmEdge pulls spec tests from a mirror repo
  (`wasmedge-spectest`) that does **not** yet include the wide-arithmetic folder,
  so making the spec suite green depends on that folder being added upstream —
  which is why the proposal asks the mentors, in Week 1, whether that's the
  mentee's PR or a maintainer task.

---

# Part B — What we have actually implemented

We built a **working prototype of Stage 4 (the interpreter) for all four
instructions**, wired through Stages 1–3 so it runs end to end:

- **Stage 1** — added the four opcodes to `enum.inc`.
- **Stage 2** — added them to the loader's no-immediate group (parse). *(We did
  not do the serializer or the proposal flag yet — the prototype leaves them
  always-on, which matches WasmEdge's default-on behavior.)*
- **Stage 3** — added the `StackTrans` signatures in the validator.
- **Stage 4** — implemented the four handlers using `common/int128.h`.

**It runs.** A `.wat` using all four instructions parses, validates, and executes,
and the results match hand-computed values:

| Function | Inputs | Result (lo, hi) | Meaning |
|---|---|---|---|
| `add128` | (2⁶⁴−1) + 1 | `0, 1` | the carry landed in the high word |
| `sub128` | 2⁶⁴ − 1 | `-1, 0` | the borrow came from the high word |
| `mul_wide_u` | (2⁶⁴−1)² | `1, -2` | full unsigned 128-bit product |
| `mul_wide_s` | (−1)·(−1) | `1, 0` | signed multiply |

*(`-1` printed = `0xFFFF…FFFF`, `-2` = `0xFFFF…FFFE`, because the CLI prints i64
as signed.)*

**What is NOT yet implemented (the remaining mentorship work):**
1. **Stage 5 — LLVM AOT/JIT lowering.** Verified feasible (the stack is a plain
   vector, so pushing two results is two `stackPush` calls; all needed IR builders
   exist) but not yet written.
2. **Serializer** (Stage 2 reverse direction).
3. **Proposal gating** — add `WideArithmetic` to the default set + a
   `--disable-wide-arithmetic` toggle + `hasProposal()` checks.
4. **Spec-test integration** and **docs**.

So the prototype covers roughly the interpreter slice — enough to prove the
approach and de-risk the plan, with the larger AOT/JIT + testing + docs work
remaining for the mentorship itself.

---

# Part C — Questions a mentor may ask (and honest answers)

**Q: Why does a single range check identify SIMD opcodes (pre-test 2)?**
Because the opcode table (`enum.inc`) emits all `0xFD`-prefixed SIMD opcodes as
one contiguous block, so their enum values form an unbroken range; a
`first ≤ code ≤ last` comparison is exact.

**Q: The instructions return two values — doesn't the validator need rework?**
No. The validator's `StackTrans(Take, Put)` already expresses any "N in, M out"
shape, and Multi-value is enabled by default. We just declare the right
signatures.

**Q: How do you keep 128-bit math working on Windows/MSVC?**
Reuse `include/common/int128.h`. It defines `uint128_t`/`int128_t` as native
`__int128` on GCC/Clang and an equivalent struct on MSVC, so one implementation
is portable — no `_addcarry_u64` intrinsics needed.

**Q: What's the operand order for `sub128`?**
Top of stack down is `RHS_high, RHS_low, LHS_high, LHS_low`. We reconstruct
`LHS` and `RHS` and compute `LHS − RHS`.

**Q: How does the LLVM path produce two results?**
The AOT compiler's stack is a `std::vector`; `stackPush` is a `push_back`. After
building the `i128` op we `trunc` the low half and `lshr 64`+`trunc` the high
half and push both.

**Q: What could block the spec tests?**
The `wasmedge-spectest` mirror has no `wide-arithmetic` folder yet. That folder
must be added upstream and the pin bumped; I flagged this to confirm ownership in
Week 1 so it doesn't block the schedule.

---

# Part D — How instructions are actually added to the ISA (expanded mechanics)

## D.1 The X-macro trick: one line → the whole ISA identity

WasmEdge's "instruction set" is the `OpCode` enum plus the per-stage code that
knows each opcode. The clever part is that **one line** in `enum.inc` feeds many
places, via the C preprocessor "X-macro" pattern.

Your line (where `OFC` is an alias for `Line_FC`):

```
OFC(I64__add128, "i64.add128", 0xFC, 19)
```

The same `enum.inc` file is `#include`d many times, each time after redefining
`Line_FC` to extract a different projection of that line:

1. **The enum value** — `enum_ast.hpp` does
   `#define Line_FC(NAME,STRING,PREFIX,EXTEND) NAME,` then includes `enum.inc`.
   Your line expands to `I64__add128,` inside `enum class OpCode`. The opcode now
   **exists** as a value.
2. **The name map** — included again with
   `#define Line_FC(...) {OpCode::NAME, STRING},`, building the `OpCode → "i64.add128"`
   table (this is what lets logs/errors print the mnemonic).
3. **The decoder** — `instruction.cpp` does, inside the `0xFC` switch,
   `#define Line_FC(NAME,STRING,PREFIX,EXTEND) case EXTEND: return OpCode::NAME;`,
   so your line becomes `case 19: return OpCode::I64__add128;`. The byte `19`
   (under prefix `0xFC`) now decodes to your opcode — **automatically**.

So one line simultaneously creates the enum entry, the printable name, and the
decoder case. **That is the mechanical meaning of "adding the instruction to the
ISA."** What the line does *not* do is define behavior — the loader's operand
reading, the validator's type rule, the interpreter's math, and the LLVM
lowering are per-opcode cases you write (Stages 2–5). The table keeps the
*identity* in sync across the codebase; you supply the *meaning*.

## D.2 Stage 2 in detail — bytes to an instruction object

The bytes for `i64.add128` are `0xFC 0x13` (`0x13` = 19). The loader reads the
prefix byte, reads the sub-opcode as a LEB128 varint, and the generated switch
maps `19 → OpCode::I64__add128`. Then a **second** switch asks "does this opcode
carry immediates (extra data bytes)?" `memory.init` would read a data index;
ours read nothing, so we placed them in the no-immediate group. The instruction
object is now complete. The **serializer** does the reverse (object → bytes) and
needs the same four cases to write `.wasm` back out.

## D.3 Stage 3 in detail — validation is a type-level dry run

Before any values exist, the validator runs the program once on an abstract stack
of **types**. `StackTrans` is literally:

```cpp
Expect<void> FormChecker::StackTrans(Span<const ValType> Take,
                                     Span<const ValType> Put) {
  EXPECTED_TRY(popTypes(Take)); // assert these types are on top; pop them
  pushTypes(Put);               // push the result types
  return {};
}
```

For `add128` we pass `Take = {I64,I64,I64,I64}`, `Put = {I64,I64}`: the validator
confirms four `i64`s are present, removes them, and pushes two `i64`s. If a
program has the wrong types before `add128`, this rejects it *before* execution.
"No new machinery" is literally true: `popTypes`/`pushTypes` already handle any
counts (that generality came with Multi-value), so a 4→2 rule is just data handed
to the same helper.

## D.4 Stage 4 in detail — the carry, and why we don't compare

This is the core of the "how do we add 128-bit without the original comparisons"
question. A 128-bit value is held as two 64-bit halves, `lo` and `hi`.

**The emulated way — what a compiler does WITHOUT the instruction:**

```
r_lo  = a_lo + b_lo
carry = (r_lo < a_lo) ? 1 : 0     // detect wrap-around by COMPARING
r_hi  = a_hi + b_hi + carry
```

The `carry = (r_lo < a_lo)` comparison and the branch that follows are the
"tax" — extra instructions, and a branch the CPU can mispredict.

**Our way — reconstruct the true 128-bit values and add once:**

```cpp
const uint128_t A = (static_cast<uint128_t>(AHi) << 64) | ALo;
const uint128_t B = (static_cast<uint128_t>(BHi) << 64) | BLo;
const uint128_t Res = A + B;                        // ONE 128-bit add, no compare
StackMgr.push(ValVariant(static_cast<uint64_t>(Res)));        // low half
StackMgr.push(ValVariant(static_cast<uint64_t>(Res >> 64)));  // high half
```

There is **no comparison**. Carry still happens correctly because `uint128_t` is
`unsigned __int128`: when the host compiler that builds WasmEdge compiles
`A + B`, it emits the CPU's native `add` + `adc` (add-with-carry) pair, and the
hardware carry flag propagates from the low half into the high half. We are not
avoiding carry — we are letting the hardware do it instead of re-deriving it with
a compare-and-branch. (`sub128` is the same with `-` and the borrow flag;
`mul_wide_s`/`_u` reconstruct via signed `int128_t` / unsigned `uint128_t` so the
single `*` becomes the CPU's widening multiply.)

**Three layers to keep straight (useful for defense):**

- **Guest level:** the `.wasm` now uses **one** instruction instead of ~5 emulated
  ones — smaller binary, fewer dispatches.
- **Host/interpreter level:** our handler's `A + B` compiles (inside WasmEdge's
  own binary) to native `add`+`adc`, so even the interpreter uses the hardware
  carry, not a Wasm-level comparison.
- **AOT/JIT level (Stage 5):** LLVM emits `add`+`adc` **directly into the guest's
  compiled native code**, so the guest runs at hardware speed with no interpreter
  loop. This is where the largest speed-up lives.

---

# Part E — What is distinctive about THIS proposal (honest)

**Framing first:** there is essentially one correct way to implement this in
WasmEdge; the 5-stage pipeline is fixed by the codebase, so every serious
applicant's technical plan looks similar. This proposal does **not** claim a
novel or "more efficient" approach — claiming that would be a red flag, because
the implementation is standard. It differentiates on **evidence and judgment**.

**Genuine differentiators (defensible):**

1. **A working prototype, not a description.** The interpreter path for all four
   instructions runs and is verified against the spec's operand order (including
   the non-commutative `sub128`) and independent computation.
2. **Codebase-depth insights that correct the common surface narrative:**
   - *No new validator machinery* — the generic `StackTrans` plus Multi-value
     already handle the N→M shapes.
   - *Reuse `common/int128.h`* — the in-tree portable type, instead of MSVC
     `_addcarry_u64` intrinsics that several applicants proposed (the
     non-idiomatic, worse choice).
3. **Current, correct spec-test diagnosis** — the actual pin (`ee503c07`) and a
   Week-1 ownership question, rather than a stale commit or an omission.
4. **Spec-grounded correctness** — opcodes and operand order confirmed against the
   upstream proposal, not guessed.

**Table-stakes (present in every serious proposal — do not oversell):** the
5-stage pipeline, using a 128-bit type, a 12-week timeline.

**Honest weakness:** no merged WasmEdge PRs or bug finds yet, which some
competitors have. The proposal competes on plan quality, demonstrated
understanding, and credible "I will finish" signals — not on out-crediting them.

**How to phrase the edge without overclaiming:**

> "I validated the standard approach with a working interpreter prototype, and
> identified two codebase-specific simplifications — the generic `StackTrans`
> with Multi-value, and reuse of `common/int128.h` — that keep the change small
> and portable across GCC/Clang and MSVC."
