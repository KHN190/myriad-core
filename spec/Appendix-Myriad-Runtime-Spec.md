# Myriad Runtime

Myriad is the runtime that loads a Polka cartridge and executes it. This spec defines runtime state and the behaviour of each opcode that depends on it. Pure-computation opcodes (arithmetic, comparison, bitwise, immediate-arith, float, branch) are fully specified in `Appendix-Bytecode-Spec.md`.

## VM Spec Sheet

64 regs/call · 65 535 slots/cell · 65 535 constants/fn · 65 535 fns/cart · 2^24 heap slots · 4-byte instructions

## 1. Data Model

- **Value**: an untagged `u64`.
- **Cell**: a heap object holding `N` `u64` slots. Each slot carries a *value/handle* tag observable through opcodes — not addressable through `ld` / `st`. Cells are reference-counted.
- **Register**: a `u64` plus a value/handle tag. A function call sees 64 fresh registers `r0..r63`; each tag is observable to `ld` / `st` / `copy` / `move` / `ref` / arg passing / return.
- **Handle**: an opaque `u64` identifying a live cell. `HANDLE_NONE = u64::MAX` is the null handle. Reading or writing through `HANDLE_NONE`, or through a handle whose target has been freed, traps.

The tag is the only thing that distinguishes a value from a handle — bit patterns alone are ambiguous. Every operation that moves a `u64` between a register, a cell slot, the const pool, or a call argument/return slot also moves the tag. A "boxed" cell-reference is built explicitly: `alloc r, 1; st val, r, 0`.

Runtime state:

- **Call stack** of in-flight function calls. Each entry remembers its function, its resume point, its 64-register window, and the register the caller named to receive its return value.
- **Heap** of rc'd cells. The VM detects use-after-free of stale handles as a non-bypassable safety net (the mechanism is impl-defined and not cart-observable).
- **Region stack** — empty at program start (§5).
- **Handler stack** — independent of the call stack; one entry per active `handle` block (§4).

## 2. Memory

`ld`, `copy`, and `pushconst` produce a new observer of the source value+tag; the source remains live. `st`, `stidx`, `move`, arg passing, and `ret` transfer ownership and consume the source. `drop` releases one observer; when the last observer of a cell goes, the runtime reclaims it (timing and transitive behaviour impl-defined). When `st` overwrites a slot, whatever the slot previously held loses one observer.

- **`alloc r_a, size`**: produce a fresh cell of `size` slots, every slot value-tagged with unspecified contents (the cart must `st` before reading). `size == 0` allowed. Handle → `r_a`.
- **`ld r_a, r_b, off`**: `r_b` handle-tagged; copy slot value+tag into `r_a`.
- **`st r_a, r_b, off`**: `r_b` handle-tagged; move `r_a`'s value+tag into the slot, **consuming** `r_a` (value 0, tag *value*). Use `copy` first if the source must survive.
- **`ldidx r_a, r_b, r_c`** / **`stidx r_a, r_b, r_c`**: same as `ld` / `st` with offset from `r_c`, compared as `u64` against cell size; out-of-range traps.
- **`drop r_a`**: release `r_a`'s observer; the register is cleared.
- **`pushconst r_a, idx`**: copy `constants[idx]` into `r_a`, tag from the per-function const handle-mask (`Appendix-Polka-Cart-Spec.md`). Handle-flagged constants are module-lifetime string handles (§7).
- **`copy r_a, r_b`**: copy value+tag from `r_b` to `r_a`.
- **`move r_a, r_b`**: transfer value+tag from `r_b` to `r_a`; `r_b` is consumed (value 0, tag *value*).

## 3. Calling Convention

Across `call` / `call_reg` / `ret` and native dispatch, the destination register `r_a` named in the call instruction is the register in the caller's window that receives the return value (plus its tag).

`call r_a, fn_id` and `call_reg r_a, r_b`:

1. Resolve the target. If it's a **native** chunk (§8), see below.
2. Otherwise begin a new call. The caller stages args at registers `r(caller_reg_count)..r(caller_reg_count + param_count - 1)` — the `param_count` register slots **immediately past** the caller's declared register count. The callee reads them as `r0..r(param_count-1)` (the callee's window starts at exactly that address). Execution starts at the callee's first instruction. The destination register `r_a` is remembered so `ret` knows where to deliver the result.

`ret r_a`:

- End the call. `r_a`'s value+tag transfers (as in `move`) into the caller's destination register. The cart is responsible for `drop`ping any other live handles in the callee's window before `ret`.
- If no caller exists, the VM halts cleanly; `r_a`'s value is the program result.
- Otherwise execution resumes in the caller at the instruction following the `call`. Return-arm rerouting hooks in here — see §4.6.

**Native call**: a native runs synchronously, reading args from `r0..r(param_count-1)` of a fresh window (each tag preserved) and writing the result+tag back into the caller's destination register. No call frame is opened.

**Halt** is signalled by `ret` from the top-level call, by `System.0x01` (clean exit with explicit code), or by `System.0x02` (panic with a string handle).

## 4. Effect Protocol — Dispatch device `0xE0`

### 4.1 Handler frame, body, continuation

`handle r_a, effect_id` pushes a handler frame. `r_a` is observed (not consumed). Its **body** is the call opened by the next `call` / `call_reg` executed while this handler is on top. Multiple handlers may share the same body (consecutive `handle` instructions before a single `call`); when that body rets, return-arm rerouting applies **innermost-first** along the handler stack.

`raise dest, key_reg, args_base` is an atomic effect-op call. It does not interrupt the body's call in the call-stack sense — the body stays in progress. What it loses is its register state at the raise site, which the VM snapshots into a fresh **continuation cell** attached to the matching handler frame; the VM then invokes the arm. The body's resume point is whatever the call stack already remembers (the instruction following `raise`).

A continuation cell carries the body's register snapshot and a *consumed* flag. It is opaque to the cart.

### 4.2 Ports

| Port | Dir | Semantics |
|---|---|---|
| `0x01` | out | Pop the top handler frame; reclaim every continuation cell it owned. Value ignored. |
| `0x03` | out | Set the active handler's pending return-arm `fn_id`. `0xFFFF` clears it. |
| `0x04` | out | Set the active handler's pending return-arm env. |

The cart never reads from `0xE0` and never writes ports `0x00` / `0x02`; lookup and arm dispatch are the runtime's atomic job inside `raise` (§4.4).

### 4.3 Dispatch table

`handle r_a, effect_id`: `r_a` is a heap handle to a cell of `2 * n_ops` slots, one `(arm_fn_id, env)` pair per op:

```
slot[2*i    ] = arm_fn_id_i   (u16 in low bits; 0xFFFF = no entry)
slot[2*i + 1] = env_i         (handle or value; slot tag preserved on read)
```

A lookup index past the table, or an `arm_fn_id` of `0xFFFF`, signals "no entry"; the walker continues outward. If no handler matches at all, the outcome is **unspecified** (§11). `HANDLE_NONE` for `r_a` installs a handler with no entries.

### 4.4 `raise` semantics

`raise dest, key_reg, args_base` performs, atomically:

1. Read `(effect_id<<8) | op_id` from `key_reg`.
2. Walk handlers top-down. For each, look up `slot[2*op_id]` in its dispatch table: if it holds a valid `arm_fn_id`, this handler matches and `(arm_fn_id, env)` is the resolved pair. Otherwise (slot is `0xFFFF` or past the table) continue outward — even outward handlers with the same `effect_id` are valid fall-through targets, so multiple handlers may compose by each covering a subset of an effect's ops. If no handler matches, behaviour is **unspecified** (§11).
3. Snapshot the body's current register window into a fresh continuation cell attached to the matched handler.
4. Open a new call into `arm_fn_id` with the arm window populated as in §4.5 (env from the dispatch table, user args copied from `args_base..` in the body's window).

Two completion paths follow, depending on whether the arm calls `resume`:

**Arm returns without resuming.** The continuation snapshot is discarded. The arm's final ret value+tag flows into `dest` of the body's window, and the body resumes at the instruction following `raise`.

**Arm calls `resume r_a, r_b`.** A three-step dance (full detail in §4.7):

1. `r_b` is delivered into `dest` of the body's window; the body's snapshot is restored and execution resumes after `raise`.
2. When the body eventually `ret`s, its return value+tag is delivered into `r_a` of the arm's window; the arm resumes after the `resume` instruction.
3. When the arm finally `ret`s, control leaves the handle block entirely; the arm's ret value+tag becomes the value of the handle expression and flows to whoever called the function that contained the `handle` block.

The number of user args is `arm.param_count - 2`.

### 4.5 Arm calling convention

Arms are ordinary bytecode functions. The arm sees:

```
r0 = env             (handler's env for this op; tag matches env's tag)
r1 = k_or_ret_env    (0 for op arms; the return-arm env on the return-arm path)
r2..r(2+nargs-1)     (user args of the effect op, in source order, tags preserved)
```

The arm receives no explicit continuation handle. The continuation it can `resume` is implicit: the most-recent unconsumed cell on the active handler frame. `resume` is only well-defined when the arm lexically contains it. The handler frame remains on the handler stack while the arm runs. Returning normally abandons the continuation (it is reclaimed when the handler frame pops) and the value flows through ordinary `ret` to the body's effect-op destination register.

### 4.6 Return arm

A handler may set a return arm (`DEO 0xE003` for `fn_id`, `DEO 0xE004` for env) at any point during its lifetime. Each write overwrites the prior pending value. Both `fn_id` and env default to "unset" when a handler is pushed.

The return arm fires **on the body's ret** — both in the no-resume and resume paths. Body's `V` is rerouted through the return arm; the return arm's `V'` replaces `V` (`V'` flows to the handle's caller in the no-resume path, to `resume.r_a` in the resume path). The arm's own subsequent ret (`W` in the resume path) does **not** pass through the return arm.

The return arm sees:

```
r0 = pending_return_arm_env
r1 = 0
r2 = V (body's return value, tag preserved)
```

The return arm must emit `DEO 0xE001` to pop the handler before its own `ret`.

**The handler is never auto-popped**. The cart is responsible for emitting `DEO 0xE001` on every exit path:

- No-return-arm case: cart emits `DEO 0xE001` immediately after the body's call returns (i.e., directly after the handle block in source order).
- Return-arm case: the return arm itself emits `DEO 0xE001` before its own `ret`.
- Early-exit / panic paths: cart emits `DEO 0xE001` (or equivalent unwind) before leaving the handle's lexical scope.

Popping a handler whose frame is no longer on the handler stack traps as unspecified (§11.1).

### 4.7 `resume` and single-shot

`resume r_a, r_b`: see §4.4 for the three-step semantics. `r_a` is **not** a continuation handle — the active continuation is implicit (the most-recent unconsumed cell on the active handler frame), and is marked consumed by this instruction.

Single-shot: a second `resume` against the same continuation is unspecified (§11). Future revisions may add multi-shot via a new port; existing semantics will not change.

## 5. Region Protocol — Region device `0xE1`

| Port | Dir | Semantics |
|---|---|---|
| `0x00` | out | Push a new region onto the region stack. |
| `0x01` | out | Pop the top region; force-free every cell recorded under it. Value ignored. |
| `0x02` | out | Deep-forget: DFS from the operand handle, cycle-bounded by a visited set. For each visited handle: if its cell is currently attached to the **top** region, detach it and recurse into the cell's handle-tagged slots; otherwise (cell is in a different region, no region, or already forgotten) **stop walking that branch**. Detachment only changes region ownership; rc is untouched. |

The region stack starts empty. Allocations made with no region active are managed by the runtime's default lifetime mechanism (§2). While a region is active, every newly allocated cell — including continuation cells produced by `raise` — is attached to the current top region and stays attached across `call` / `ret` until the region is popped or the cell is explicitly forgotten. A cell belongs to at most one region.

Force-freeing through region-pop bypasses the default mechanism. Any handle to a freed cell becomes stale; subsequent access traps via the stale-handle check (§11.1).

## 6. Devices

Port encoding: `(device_id << 8) | port_id`. `DEI` / `DEO` against an absent device traps. `0xE0` Dispatch and `0xE1` Region are VM mechanisms (§4, §5); the cart never writes them from source.

### 6.1 System (`0x00`)

| Port | Dir | Semantics |
|---|---|---|
| `0x00` | in | Spec version, packed as `(major << 48) \| (minor << 32) \| patch`, with `major:16`, `minor:16`, `patch:32`. |
| `0x01` | out | Halt. Low 32 bits = exit code. |
| `0x02` | out | Panic. Operand is a string handle (§7); host prints to stderr and halts with code 1. |
| `0x03` | in | Module load flags (host-defined; zero on a bare host). |

### 6.2 Console (`0x10`)

| `0x00` | in | One byte from stdin. `-1` on EOF. |
| `0x01` | out | Low byte to stdout. |
| `0x02` | out | Low byte to stderr. |
| `0x03` | out | Flush stdout and stderr. |

IDs `0x20`–`0xDF` are unassigned; portable extensions go through imports/exports (§10), not new device IDs.

## 7. Strings

A string handle is opaque. The cart's only legal operations on it are: pass to the mandatory string-handling natives, pass to `System.0x02`, store in cells / pass through calls like any other handle, and `drop`. The cart cannot inspect string contents through `ld`; the underlying cell representation is impl-defined.

The loader produces a string handle for every handle-flagged constant whose pre-load value is in `0..string_count`, allocating a module-lifetime cell that holds the corresponding pool entry.

## 8. Mandatory Imports

Native functions live in the same function table as user bytecode functions, occupying the leading slots. They are invoked via the normal `call` opcode; the runtime routes by chunk kind. A conforming core host must provide every name below.

| Group | Names |
|---|---|
| console | `print`, `println` |
| float math | `ceil`, `flr`, `cos`, `sin`, `sqrt` |
| int math | `max`, `min`, `abs` |
| float-typed math | `__float_max`, `__float_min`, `__float_abs` |
| conversions | `__int_to_f`, `__char_to_f`, `__bool_to_f`, `__float_to_i`, `__char_to_i`, `__bool_to_i`, `__int_to_c`, `__int_to_s`, `__float_to_s`, `__bool_to_s`, `__char_to_s`, `__string_to_s`, `__unit_to_s` |
| string ops | `__concat`, `__to_str`, `__str_len`, `__str_byte_at`, `__str_eq` |
| system | `halt`, `abort` |

`__to_str(v: u64) -> handle`: fallback string conversion for sites where compile-time dispatch can't pin the type (notably effect-op return values). If `v` is a string handle, return it unchanged. Otherwise format the raw `u64` as a signed `i64` decimal and return a freshly allocated string. Prefer typed helpers (`__int_to_s` etc.) when the type is known.

`__str_len(s: handle) -> i64`: byte length of the string. (Not character count; multi-byte UTF-8 codepoints contribute their byte width.)

`__str_byte_at(s: handle, i: i64) -> i64`: byte value `0..=255` at index `i`. Returns `-1` if `i < 0` or `i >= __str_len(s)`.

`__str_eq(s1: handle, s2: handle) -> bool`: byte-wise equality. Returns `1` for `True`, `0` for `False`.

A cart that references a name the host has not registered fails to load with `unresolved import: <name>`.

## 9. Identifiers

- **`effect_id` (u16)**: cart-compiler assigned within `0x0000`–`0xFEFF`. Each cart is its own namespace. `0xFF00`–`0xFFFE` reserved for extension-defined effects (§10). `0xFFFF` reserved sentinel, never used.
- **`op_id` (u8)**: cart-compiler assigned within an effect.
- **`fn_id` (u16)**: function table index.

## 10. Extensions

Anything beyond pure computation is an **extension**: a host may register additional imports and may invoke exports. Imports register through `register_native(name, fn)` + `register_host_fn(decl)`; an unregistered name causes `unresolved import: <name>` at load time. 

An extension that introduces effects allocates its `effect_id`s in `0xFF00`–`0xFFFE`; collisions between extensions a cart depends on are the extension specs' problem to resolve. 

A portable extension is defined by a separate spec fixing a namespace, signatures, and normalization rules. Carts that touch no extension run on a bare core host. Core ships no built-in effects.

## 11. Sandbox

User code may corrupt its own behaviour arbitrarily. The VM enforces a single boundary: user code cannot threaten the embedding. Worst outcome is a clean halt with an error. Host function safety is the embedding's responsibility. Out-of-memory halts the VM directly; there is no user recovery path.

### 11.1 Error model

Errors fall into two classes:

**Sandbox traps** — the runtime **must** enforce these; violation would let user code escape the sandbox:

- instruction-pointer / code-buffer bounds
- cell-slot / array bounds (offset compared as `u64` against cell size; out-of-range traps)
- `HANDLE_NONE` or stale-handle access (including any access after a region force-free)
- `DEI` / `DEO` against an absent device
- unresolved native import (caught at load time)
- out of memory
- arithmetic traps already named (`div` / `mod` by zero)

A sandbox trap halts the VM cleanly with a non-zero exit code and a message to stderr.

**Cart-correctness errors** — the runtime is **not required** to detect these:

- `raise` with no matching handler
- `raise` whose `arm_fn_id` is not a bytecode chunk, or whose `arm.param_count` doesn't match the call site
- a second `resume` against an already-consumed continuation
- `resume` with no active handler frame
- any Dispatch `DEO` port with no active handler frame
- failing to `drop` a handle the cart no longer needs (leak, not UAF)

The cart compiler is expected to emit code that avoids cart-correctness errors. Implementations that do choose to detect any of them must still halt cleanly.

## 12. Forward Compatibility

- **Single-shot continuations**: §4.7 is the current contract. A future revision may add multi-shot through a new port without changing existing port semantics. An arm must lexically contain its `resume` for this restriction to hold.
- **Continuation cell layout**: §4.1 is opaque to the cart; future revisions may restructure.

## 13. Stability

Immutable: core device port semantics (System / Console / Dispatch / Region), mandatory imports set, the observable contracts in §1–§7 and §9. New mandatory imports may be added; existing semantics do not change. Extension import sets are versioned independently.
