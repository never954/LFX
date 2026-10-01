# LFX Mentorship (2026, Term 3) — Proposal
## Support for the Wide Arithmetic Proposal in WasmEdge (Issue #5153)

> `⟨FILL⟩` marks details only you can supply.

---

### 1. Basic Information

- **Name:** ⟨FILL⟩
- **Email:** ⟨FILL⟩
- **GitHub:** ⟨FILL⟩
- **Project:** Support for Wide Arithmetic, a Phase 3 WebAssembly proposal
- **Mentors:** Hung-Ying Tai (@hydai), Yi-Ying He (@q82419)

---

### 2. Summary

This project implements the four WebAssembly Wide Arithmetic instructions —
`i64.add128`, `i64.sub128`, `i64.mul_wide_s`, `i64.mul_wide_u` — across all of
WasmEdge's execution modes: the interpreter, the LLVM AOT compiler, and the JIT
engine. The work spans opcode registration and proposal gating, binary
loader parse/serialize, validator stack typing, interpreter execution, and LLVM
lowering, with conformance verified against the upstream specification tests.

---

### 3. Problem Statement

WebAssembly natively supports integers only up to 64 bits. Programs that need
128-bit arithmetic — cryptography, big-integer libraries, blockchain — currently
emulate it with sequences of 64-bit operations plus explicit carry/borrow
detection (a wrap-around comparison after each partial add). A single 128-bit
addition expands into several Wasm instructions with branching.

The underlying hardware already does this efficiently: a 64-bit CPU computes the
low half, records a **carry flag**, and applies **add-with-carry (ADC)** to the
high half. Because core WebAssembly had no way to express this, runtimes were
forced down the slow emulated path. The Wide Arithmetic proposal adds the
vocabulary so a runtime can lower these operations straight to the native
carry/borrow and widening-multiply instructions — smaller binaries and
materially faster big-integer code, most notably through WasmEdge's LLVM AOT/JIT
path where `i64.add128` becomes a native `add`/`adc` pair.

---

### 4. Instruction semantics

| Instruction | Opcode | Stack type | Meaning |
|---|---|---|---|
| `i64.add128` | `0xFC 19` | `[i64 i64 i64 i64] → [i64 i64]` | 128-bit add of two (lo, hi) pairs → (lo, hi) |
| `i64.sub128` | `0xFC 20` | `[i64 i64 i64 i64] → [i64 i64]` | 128-bit subtract |
| `i64.mul_wide_s` | `0xFC 21` | `[i64 i64] → [i64 i64]` | signed 64×64 → 128-bit product (lo, hi) |
| `i64.mul_wide_u` | `0xFC 22` | `[i64 i64] → [i64 i64]` | unsigned 64×64 → 128-bit product (lo, hi) |

*(Exact operand ordering will be pinned to the upstream `wide-arithmetic` spec
text in Week 1 before final handlers are written.)*

---

### 5. Implementation architecture

A verified, file-by-file plan based on reading the current `master`. Each row
cites the concrete insertion point and the existing pattern to follow.

**5.1 Opcode registration + proposal gating.** `include/common/enum.inc` — add
four opcode entries for sub-opcodes 19–22 (the `0xFC` block currently ends at
`table.fill`, sub-opcode 17, so there is no collision). Add a new proposal
`WideArithmetic` to the proposal list and gate the four opcodes behind it,
following the pattern used for `SIMD`, `Threads`, etc.

**5.2 Loader — parse + serialize.** The `0xFC` prefix decode is generated from
`enum.inc`, so registering the opcodes makes them decodable automatically. The
four instructions carry no immediate operands, so they only need to be listed in
the no-immediate group in `lib/loader/ast/instruction.cpp`, and mirrored on the
encode side in `lib/loader/serialize/serial_instruction.cpp` with loader
round-trip tests.

**5.3 Validator — stack typing.** `lib/validator/formchecker.cpp` expresses
instruction stack effects through a generic `StackTrans(Take, Put)`. Because the
Multi-value proposal is already supported, two-result instructions need no new
machinery — only the correct signatures:
`add128`/`sub128` → `StackTrans({I64,I64,I64,I64},{I64,I64})`;
`mul_wide_*` → `StackTrans({I64,I64},{I64,I64})`.

**5.4 Interpreter — execution.** `lib/executor/engine/engine.cpp`, next to the
existing `I64__add` case. The 128-bit math uses `WasmEdge::uint128_t` /
`int128_t` from `include/common/int128.h`, which is `unsigned __int128` on
GCC/Clang and a struct-with-operators fallback on MSVC — so the Windows build
stays correct without bespoke intrinsics.

**5.5 LLVM AOT/JIT — lowering.** `lib/llvm/compiler/numericInstr.cpp`, next to
the `I64__add` lowering (`Builder.createAdd(LHS, RHS)`). For `add128`/`sub128`:
zero-extend the four `i64`s to `i128`, reassemble the two operands, `createAdd`/
`createSub`, then split the result back to two `i64`s (`trunc` low, `lshr 64` +
`trunc` high). For `mul_wide_u`/`mul_wide_s`: `zext`/`sext` both `i64` to `i128`,
`createMul`, split. LLVM folds these to the native carry/widening-multiply
sequences.

---

### 6. Prototype: a working interpreter path

To validate the plan rather than assert it, I implemented the **complete
interpreter path for all four instructions** as a prototype in a local build of
`master` (developed with AI assistance; see §12). The change is a 61-line diff
across four files — exactly the stages in §5.1–5.4:

```
 include/common/enum.inc        |  6 ++++++
 lib/executor/engine/engine.cpp | 40 ++++++++++++++++++++++++++++++++++++++++
 lib/loader/ast/instruction.cpp |  6 ++++++
 lib/validator/formchecker.cpp  |  9 +++++++++
```

It parses, validates, and executes a `.wat` module using all four instructions.
Verified against independently computed test vectors (results printed as signed
`i64`):

| Function | Inputs | Result (lo, hi) | Check |
|---|---|---|---|
| `add128` | (2⁶⁴−1) + 1 | `0, 1` | carry into high word |
| `sub128` | 2⁶⁴ − 1 | `-1, 0` | borrow from high word |
| `mul_wide_u` | (2⁶⁴−1)² | `1, -2` | full unsigned 128-bit product |
| `mul_wide_s` | (−1)·(−1) | `1, 0` | signed widening |

**Why this approach is the right one (analysis from reading the codebase):**

- **No new validator machinery is needed.** The `4→2` / `2→2` stack shapes are
  expressed directly through the existing generic `StackTrans` because Multi-value
  is already supported. A common assumption is that these instructions require a
  new validator signature form; in this codebase they do not.
- **Cross-platform 128-bit math is already solved in-tree.** Rather than adding
  MSVC-specific `_addcarry_u64` intrinsics, the implementation reuses
  `common/int128.h`'s portable `uint128_t`/`int128_t`, keeping the interpreter
  identical across GCC/Clang and MSVC. This is simpler and consistent with how
  the rest of WasmEdge already handles 128-bit values.
- **The opcode decode is table-generated**, so registration in `enum.inc`
  propagates to the loader automatically — reducing the change surface and the
  risk of a hand-written switch drifting out of sync.

The prototype de-risks the interpreter milestones and establishes the exact test
vectors reused for the AOT/JIT differential tests. Patch, `.wat`, and output are
included with this submission.

---

### 7. Testing strategy

- **Differential tests:** table-driven cases (zero, carry across the 64-bit
  boundary, signed vs unsigned multiply, `INT64_MIN`/max edges) run through the
  interpreter, AOT, and JIT and asserted equal.
- **Upstream spec tests:** the suite is fetched from `WasmEdge/wasmedge-spectest`
  (currently pinned at `ee503c07`, `wasm-core-20260728`) via
  `test/spec/CMakeLists.txt`; `WASMEDGE_SPECTEST_FOLDERS` does not yet list a
  `wide-arithmetic` entry, and the upstream `WebAssembly/wide-arithmetic` repo
  ships `test/core/wide-arithmetic.wast`. Making the spec run green therefore
  requires those tests in the WasmEdge spectest mirror and the pin bumped.
  **Open question for the mentors (Week 1):** should the mentee PR the
  `wide-arithmetic` folder into `wasmedge-spectest` and bump the tag, or is that
  mirror updated on a separate maintainer cadence? This determines whether spec
  integration is my task or a dependency, and I have scheduled it explicitly.
- **Loader round-trip tests** under `test/loader` for the new opcodes.

---

### 8. 12-week timeline

| Weeks | Milestone |
|---|---|
| **1** | Pin operand order to the spec; confirm spec-test mirror ownership with mentors; add `WideArithmetic` proposal flag + four opcodes; wire configuration gating. |
| **2** | Loader parse + serialize; loader/serializer round-trip tests. |
| **3** | Validator `StackTrans` signatures; validation unit tests (accept + reject). |
| **4** | Finalize interpreter handlers (building on the prototype) + edge-case tests. |
| **5** | Interpreter hardening; buffer for slippage. |
| **6–7** | LLVM lowering for `add128`/`sub128`; correct on both AOT and JIT. |
| **8–9** | LLVM lowering for `mul_wide_s`/`mul_wide_u`; differential tests across all three engines. |
| **10** | Spec-test integration (per Week-1 decision); drive to green. |
| **11** | Cross-platform hardening (MSVC path via `int128.h`), review iteration; buffer. |
| **12** | Documentation in `WasmEdge/docs`; final review and feedback. |

---

### 9. Risks & mitigations

- **Spec-test mirror dependency** — surfaced Week 1 so it never blocks Week 10.
- **Operand-order / lo–hi mistakes** — mitigated by pinning to the spec first and
  by differential testing across engines (the prototype already exercises this).
- **LLVM API drift** — build against the LLVM version CI uses; keep lowering to
  stable `IRBuilder` primitives (`zext`, `sext`, `shl`, `lshr`, `trunc`, `add`,
  `sub`, `mul`).

---

### 10. Availability & disclosure

I have university mid-term / end-semester examinations from **late November to
early December**, lasting roughly **15–20 days**, during which my weekly progress
will be lower than usual. The timeline above front-loads the interpreter and
validator work and keeps buffer weeks, so this period lands on lighter
integration/documentation tasks rather than the core implementation. I will flag
the exact dates to the mentors at the start and adjust the affected weeks
accordingly.

⟨FILL: your weekly hours and time zone.⟩

---

### 11. Background

⟨FILL: a short, factual note — C++ experience, and computer-organization /
digital-logic familiarity (carry/borrow, ALU behavior, bit-level reasoning),
plus any prior open-source or systems work.⟩

---

### 12. AI assistance disclosure

Parts of the prototype and this document were prepared with AI assistance
(Claude, Anthropic). All code was reviewed and is understood by me; WasmEdge's
own contribution guidelines provide for disclosing AI assistance via an
`Assisted-by:` trailer, which I will use on any commits.

---

### 13. Pre-test report

Completed and included with this submission (Pre-tests 1–3).
