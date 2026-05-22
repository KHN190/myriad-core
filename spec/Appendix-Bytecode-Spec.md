# Polka Bytecode

Polka is the bytecode the abrase compiler emits and the Myriad runtime consumes. This file defines instruction layout and the opcode catalogue.

## 1. Instruction Layout

Each instruction is 4 bytes. Operands are register ids (8-bit) or immediates (8/16-bit):

```
[op|r_a|r_b|r_c]
[op|r_a|imm16]
[op|     imm16]
[op|r_a|r_b|imm8]
```

Register ids are `0..63`. Constants exceeding 16 bits live in the data pool and are loaded with `pushconst`.

## 2. Opcodes

46 opcodes, `0x00`–`0x2d`. Slots `0x2e`–`0xff` reserved; an unknown opcode is a load-time error.

All integer-arithmetic, comparison, bitwise, immediate-arithmetic, and float ops (§2.1–§2.3, §2.10, §2.11) produce a value-tagged result, regardless of the input tags.

### 2.1 Integer arithmetic — `0x00`–`0x05`

| Op | Form | Semantics |
|---|---|---|
| `0x00` | `add r_a, r_b, r_c` | i64, wraps |
| `0x01` | `sub r_a, r_b, r_c` | i64, wraps |
| `0x02` | `mul r_a, r_b, r_c` | i64, wraps |
| `0x03` | `div r_a, r_b, r_c` | trap on `r_c == 0` |
| `0x04` | `mod r_a, r_b, r_c` | trap on `r_c == 0` |
| `0x05` | `neg r_a, r_b` | i64 negate |

### 2.2 Integer comparison — `0x06`–`0x0b`

`eq neq lt gt lte gte` — `r_a := r_b cmp r_c`, result 0 or 1, signed i64.

### 2.3 Bitwise — `0x0c`–`0x10`

`and or xor shl shr` — `shr` is arithmetic; shift count taken mod 64.

### 2.4 Control flow — `0x11`–`0x16`

| Op | Form | Notes |
|---|---|---|
| `0x11` | `jmp +off` | |
| `0x12` | `jz r_a, +off` | `r_a` is the test register |
| `0x13` | `jnz r_a, +off` | `r_a` is the test register |
| `0x14` | `call r_a, fn_id` | `r_a` is the destination for the return value |
| `0x15` | `ret r_a` | `r_a` is the return value |
| `0x16` | `call_reg r_a, r_b` | `r_a` is the destination for the return value; `r_b` holds the `fn_id` |

`+off` is signed i16, relative to the **next** instruction (PC after the branch). `off == 0` falls through. Full call/ret semantics — argument staging, tag transfer, native dispatch, halt-on-empty-stack — in Runtime §3.

### 2.5 Data movement — `0x17`–`0x19`

| `0x17` | `pushconst r_a, idx` |
| `0x18` | `copy r_a, r_b` |
| `0x19` | `move r_a, r_b` |

Tag and rc rules in Runtime §2.

### 2.6 Memory — `0x1a`–`0x1d`

| `0x1a` | `ld r_a, r_b, off` |
| `0x1b` | `st r_a, r_b, off` |
| `0x1c` | `ldidx r_a, r_b, r_c` |
| `0x1d` | `stidx r_a, r_b, r_c` |

Offsets are in 64-bit slots; offset 0 is the first slot. Full behaviour — tag propagation, bounds — in Runtime §2.

### 2.7 Heap — `0x1e`–`0x1f`

| `0x1e` | `alloc r_a, size` — up to 65 535 slots; `size == 0` allowed |
| `0x1f` | `drop r_a` |

Runtime §2.

### 2.8 Host I/O — `0x20`–`0x21`

| `0x20` | `dei r_a, r_b` — read port `low16(r_b)` → `r_a` |
| `0x21` | `deo r_a, r_b` — write `r_a` to port `low16(r_b)` |

Port encoding `(device_id << 8) | port_id`. Device semantics in Runtime §4 (Dispatch), §5 (Region), §6 (System / Console).

### 2.9 Effect — `0x22`–`0x23`, `0x2d`

| `0x22` | `handle r_a, effect_id` — `r_a` holds the dispatch-table handle |
| `0x23` | `resume r_a, r_b` — `r_a` is the arm-side destination for the body's eventual final value; `r_b` is the value delivered to the body |
| `0x2d` | `raise r_a, r_b, r_c` — atomic effect-op call. `r_a` = result dest; `r_b` holds `(effect_id<<8) \| op_id`; `r_c` = first user-arg register |

Full protocol in Runtime §4.

### 2.10 Immediate arithmetic — `0x24`–`0x25`

| `0x24` | `addimm r_a, r_b, imm8` — sign-extended |
| `0x25` | `subimm r_a, r_b, imm8` — sign-extended |

Codegen uses these when a literal fits in `i8`.

### 2.11 Float — `0x26`–`0x2c`

IEEE 754 f64.

| `0x26` | `fadd` | `0x29` | `fdiv` |
| `0x27` | `fsub` | `0x2a` | `fneg r_a, r_b` |
| `0x28` | `fmul` | `0x2b` | `flt` |
|        |        | `0x2c` | `feq` |

`flt` and `feq` return 0 / 1; both false if either operand is NaN. `>` lowers to `flt` with operands swapped. `>= <= !=` have no dedicated opcode; emit-time error.

## 3. Stability

Immutable: opcode set and encoding, instruction formats. New opcodes may be added in `0x2e`–`0xff`; existing semantics do not change.
