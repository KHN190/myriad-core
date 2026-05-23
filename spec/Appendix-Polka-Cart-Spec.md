# Polka Format (`.pk`)

Byte-level encoding of a Polka cartridge. Companion to `Appendix-Bytecode-Spec.md`; this file pins down everything needed to read a `.pk` produced by the abrase compiler and reconstruct an in-memory `Module`.

All multi-byte integers are **little-endian**. There is no padding except where explicitly stated.

## Layout

```
HEADER         12 bytes
FUNCTION TABLE 4 bytes (count) + count entries (12 bytes each)
PAYLOADS       per function, in table order
```

## Header (12 bytes)

| offset | size | field         | value                                 |
|--------|------|---------------|---------------------------------------|
| 0      | 4    | magic         | `0xECFF00EC`                          |
| 4      | 2    | version       | `0x0201`                              |
| 6      | 2    | flags         | bit 0 = `INT32_SAFE`; others reserved zero |
| 8      | 4    | entry_fn_id   | index into the function table         |

A loader rejects a cart with the wrong magic or an unsupported version. 

`INT32_SAFE` binds value encoding: Int constants are i32 sign-extended in the low 32 bits, Float constants are **f32 bit patterns** in the low 32 bits (high 32 = 0), arithmetic stays in i32 / f32. A runtime loading such a cart must narrow float ops to f32 — it cannot read constants as f64.

## Function Table

```
count : u32
entry × count
```

Every entry is **12 bytes**. The first byte (`kind`) selects which layout applies. The remaining 11 bytes follow the variant; padding bytes are zero.

### Bytecode entry — `kind = 0x00`

| offset | size | field         |
|--------|------|---------------|
| 0      | 1    | kind = 0      |
| 1      | 1    | param_count   |
| 2      | 1    | reg_count     |
| 3      | 1    | pad (0)       |
| 4      | 2    | const_count   |
| 6      | 2    | string_count  |
| 8      | 4    | code_count    |

`code_count` counts 4-byte instructions, not bytes.

### Native entry — `kind = 0x01`

| offset | size | field         |
|--------|------|---------------|
| 0      | 1    | kind = 1      |
| 1      | 1    | param_count   |
| 2      | 2    | pad (0)       |
| 4      | 2    | name_len      |
| 6      | 6    | pad (0)       |

Function ids are implicit: position in the table is the `fn_id`.

## Payloads

Payloads appear sequentially in the same order as the function table. There are no offsets — each payload's length is fully determined by its entry's counts.

### Bytecode payload

```
constants    : u64 × const_count
const_mask   : ceil(const_count / 8) bytes, bit-packed (LSB-first per byte)
strings      : { len : u32, utf8_bytes : len } × string_count
code         : 4 bytes × code_count       (see §Instructions)
```

A bit set in `const_mask` marks the corresponding `constants[i]` as a heap-handle constant. Such entries hold a string-pool index (`0..string_count`) before loading; the loader allocates each referenced string as a heap cell and rewrites the constant slot to the resulting handle.

The `const_mask` empty case (`const_count == 0`) contributes zero bytes.

### Native payload

```
name : utf8_bytes × name_len
```

The loader resolves each native by name against the host's registered import table; an unmatched name fails the cart load.

## Instructions

Each instruction is exactly 4 bytes: an opcode byte followed by 3 operand bytes. The operand layout depends on the opcode.

| op   | mnemonic       | byte 1 | byte 2 | byte 3 | notes                          |
|------|----------------|--------|--------|--------|--------------------------------|
| 0x00 | add            | r_a    | r_b    | r_c    |                                |
| 0x01 | sub            | r_a    | r_b    | r_c    |                                |
| 0x02 | mul            | r_a    | r_b    | r_c    |                                |
| 0x03 | div            | r_a    | r_b    | r_c    | trap on r_c == 0               |
| 0x04 | mod            | r_a    | r_b    | r_c    | trap on r_c == 0               |
| 0x05 | neg            | r_a    | r_b    | 0      |                                |
| 0x06 | eq             | r_a    | r_b    | r_c    |                                |
| 0x07 | neq            | r_a    | r_b    | r_c    |                                |
| 0x08 | lt             | r_a    | r_b    | r_c    | signed i64                     |
| 0x09 | gt             | r_a    | r_b    | r_c    | signed i64                     |
| 0x0a | lte            | r_a    | r_b    | r_c    | signed i64                     |
| 0x0b | gte            | r_a    | r_b    | r_c    | signed i64                     |
| 0x0c | and            | r_a    | r_b    | r_c    | bitwise                        |
| 0x0d | or             | r_a    | r_b    | r_c    | bitwise                        |
| 0x0e | xor            | r_a    | r_b    | r_c    | bitwise                        |
| 0x0f | shl            | r_a    | r_b    | r_c    | shift count mod 64             |
| 0x10 | shr            | r_a    | r_b    | r_c    | arithmetic, count mod 64       |
| 0x11 | jmp            | 0      | imm_lo | imm_hi | imm = signed i16 (LE)          |
| 0x12 | jz             | r_a    | imm_lo | imm_hi | branch if r_a == 0             |
| 0x13 | jnz            | r_a    | imm_lo | imm_hi | branch if r_a != 0             |
| 0x14 | call           | r_a    | id_lo  | id_hi  | r_a = result dest; fn_id u16 LE |
| 0x15 | ret            | r_a    | 0      | 0      | r_a = return value             |
| 0x16 | call_reg       | r_a    | r_b    | 0      | r_a = result dest; r_b = fn_id |
| 0x17 | pushconst      | r_a    | idx_lo | idx_hi | idx is u16 (LE)                |
| 0x18 | copy           | r_a    | r_b    | 0      |                                |
| 0x19 | move           | r_a    | r_b    | 0      |                                |
| 0x1a | ld             | r_a    | r_b    | off    | off is u8                      |
| 0x1b | st             | r_a    | r_b    | off    | off is u8                      |
| 0x1c | ldidx          | r_a    | r_b    | r_c    |                                |
| 0x1d | stidx          | r_a    | r_b    | r_c    |                                |
| 0x1e | alloc          | r_a    | sz_lo  | sz_hi  | size in slots, u16 (LE)        |
| 0x1f | drop           | r_a    | 0      | 0      |                                |
| 0x20 | dei            | r_a    | r_b    | 0      | r_b holds port                 |
| 0x21 | deo            | r_a    | r_b    | 0      | r_b holds port                 |
| 0x22 | handle         | r_a    | eid_lo | eid_hi | r_a = dispatch table; effect_id u16 LE |
| 0x23 | resume         | r_a    | r_b    | 0      | r_a = arm-side dest; r_b = delivered value |
| 0x24 | addimm         | r_a    | r_b    | imm    | imm is signed i8               |
| 0x25 | subimm         | r_a    | r_b    | imm    | imm is signed i8               |
| 0x26 | fadd           | r_a    | r_b    | r_c    | IEEE 754 f64                   |
| 0x27 | fsub           | r_a    | r_b    | r_c    | IEEE 754 f64                   |
| 0x28 | fmul           | r_a    | r_b    | r_c    | IEEE 754 f64                   |
| 0x29 | fdiv           | r_a    | r_b    | r_c    | IEEE 754 f64                   |
| 0x2a | fneg           | r_a    | r_b    | 0      | IEEE 754 f64                   |
| 0x2b | flt            | r_a    | r_b    | r_c    | 0/1; false if any operand NaN  |
| 0x2c | feq            | r_a    | r_b    | r_c    | 0/1; false if any operand NaN  |
| 0x2d | raise          | r_a    | r_b    | r_c    | r_a = result dest; r_b = key reg; r_c = first user-arg reg |

Opcodes `0x2e`–`0xff` are reserved; encountering one is a load-time error.

`r_a`, `r_b`, `r_c` are u8 register ids in `0..64`; the high bits of the byte are always zero.

## Validation checklist

A conforming loader rejects a cart that:

- has wrong magic or version,
- declares `entry_fn_id` ≥ `count` of the function table,
- declares a Bytecode entry whose `reg_count > 64`,
- contains a reserved opcode byte (`0x2e`–`0xff`),
- runs past end-of-cart while reading a payload,
- has a `const_mask` bit `i` set while `constants[i]` ≥ `string_count` (handle-flagged constant points past the string pool — includes the `string_count == 0` case),
- references a native by name that the host has not registered.

A loader is **not** required to validate that the instruction stream is well-typed or that register / constant indices stay in range. Such errors trap at execution time.
