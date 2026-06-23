# ApexII Config File Format

Config files are INI-like text files passed to `apexdis` and `apeximgui`. They annotate a ROM with labels, entry points, table layouts, inline signatures, data ranges, and documentation.

## General Syntax

```ini
; this is a comment
# this is also a comment
include = other.ini

[section]
key = value
```

Rules:
- Empty lines and leading/trailing whitespace are ignored.
- Text after an unescaped `;` or `#` is a comment and ignored.
- Quoted values may contain `\;`, `\#`, `\\`, `\"`, and `\n` as escape sequences.
- The same section name may appear multiple times; entries accumulate.
- A later entry for the same key/address in the same section overrides earlier ones.
- `include = path` loads another config file. Relative paths are resolved relative to the including file. `include` works at the top level regardless of any surrounding section header.

## Address Notation

Two forms are accepted everywhere an address appears as a key:

| Form | Meaning |
|---|---|
| `0x8990` | CPU address in the fixed system bank (`0x8000`–`0xffff`) |
| `Bff_A8990` | Same address, bank byte explicit (`ff` = system bank) |
| `B3b_A4001` | CPU address `0x4001` in the paged bank whose first byte is `0x3b` |

The generated label format is `Bbb_Aaaaa` (2-digit hex bank byte, 4-digit hex CPU address).

## Sections

### `[options]`

Global behavior switches.

```ini
[options]
labels_are_entries = false
```

| Option | Default | Meaning |
|---|---|---|
| `labels_are_entries` | `false` | When `true`, every `[labels]` entry is also treated as a code entry point. Leave `false` for fine-grained control via `[entries]`. |

Recommended: keep the default (`false`) so that labels and code classification are independent.

### `[labels]`

Assigns human-readable names to addresses. Names are used in disassembly output, xrefs, and the GUI.

```ini
[labels]
0x8990 = FarCall
0x82d1 = Panic
B3d_A784b = ThingAwardFarEntry
```

Multiple labels may share an address; all are emitted. A name must not be assigned to more than one address.

Valid label names follow the same rules as assembler symbol names and must not collide with assembler keywords (`JSR`, `STRING`, `TABLE_PTR`, …).

### `[entries]`

Classifies addresses as code entry points, causing the disassembler to recursively trace from those addresses.

```ini
[entries]
0x8123 = code
B38_A5100 = code
```

The value must be `code` or may be left empty. When `labels_are_entries = false`, this is the only way to classify addresses as code other than following flow from reset vectors.

### `[exclude_refs]`

Suppresses speculative code references to the listed addresses. The analyser normally records a "code" cross-reference whenever it encounters an instruction whose immediate operand happens to be a valid in-ROM address (e.g. `LDD #$4000`). If that operand is not actually a pointer — just a register value or constant — the resulting reference is a false positive. Listing the target address here prevents it from being recorded.

```ini
[exclude_refs]
B3d_A784b = 1
Bff_A9000 = 1
0x8990    = 1
```

The value on the right-hand side is ignored; `1` is conventional. Bank-qualified and bare-address forms are both accepted.

This section has no effect on references that arise from actual branch or jump instructions — those are always recorded regardless.

### `[inline]`

Declares routines that consume a fixed-layout payload of bytes immediately after the `JSR` or `JMP` instruction that calls them. The disassembler skips over those bytes and emits typed `INLINE_*` pseudo-ops instead of disassembling them as code.

Syntax:

```ini
[inline]
<addr> = <field>[, <field>...]
```

Each `<field>` is one of the field kinds listed in the [Field Kind Reference](#field-kind-reference) below, optionally followed by a repeat count in brackets.

Examples:

```ini
[inline]
0x82d1 = byte
0x8990 = far_code
0x8123 = ptr16_data, far_data, ptr16_code, word, byte[2]
B20_A4006 = byte
```

Using a named type as the field kind annotates the emitted `INLINE_BYTE` or `INLINE_WORD` with the enum name (see [`[types]`](#types)):

```ini
[types]
mode_param:byte = 0x42:attract, 0xff:game

[inline]
0x82d1 = mode_param
```

Output:

```asm
    JSR Panic
        INLINE_BYTE 0x42 ; for JSR Panic mode_param=attract
```

Banked addresses work the same as in `[labels]`:

```ini
[inline]
B20_A4006 = byte
```

### `[schemas]`

Named row schemas for reuse in `[tables]` and `[inline]`.

```ini
[schemas]
menu_row = byte, word, far_data
score_entry = byte, ptr16_string, far_code
```

A schema name may be used anywhere a comma-separated field list is accepted:

```ini
[tables]
B20_A4007 = rows[1](menu_row)
B30_A5000 = counted(score_entry)
```

### `[tables]`

Defines the start address and layout of a table. The disassembler emits typed `TABLE_*` pseudo-ops for each row.

Three layout forms are supported:

**Counted table** — a 3-byte header precedes the rows: `count_hi, count_lo, row_width`.

```ini
[tables]
B3c_A4001 = counted(ptr16_string)
B30_A5000 = counted(score_entry)
B30_A6000 = counted(byte, word, far_data)
```

Shorthands for the two most common counted layouts:

```ini
B3b_A4a0a = counted_ptr16_string
B3b_A55b9 = counted_ptr16_data
```

**Headerless rows** — an explicit row count is given in config.

```ini
[tables]
Bff_A8001 = rows[116](far_code)
B20_A4007 = rows[4](byte, far_data)
B30_A5200 = rows[8](menu_row)
```

**Far-code array** — shorthand for a headerless array of far-code pointers.

```ini
[tables]
Bff_A8001 = far_code[116]
```

This is equivalent to `rows[116](far_code)`.

### `[data]`

Classifies ranges as data, preventing the disassembler from treating them as code even if they appear reachable from a jump table or other pointer.

```ini
[data]
Bff_A8002 = bytes[3]
B3b_A415c = string
B20_A400b = far_code
B20_A4010 = far_string
B20_A4013 = far_data
B20_A4016 = far_table
B20_A4019 = dmd_fullframe
```

A bank-qualified address key is required (system addresses must use the `Bff_` prefix or the numeric form `0x8xxx`).

Supported data kinds:

| Value | Meaning |
|---|---|
| `bytes[N]` | Raw bytes, length N |
| `string` | Null-terminated ASCII string |
| `dmd_fullframe` | DMD full-frame bitmap |
| `far_string` | 3-byte far pointer to a string |
| `far_data` | 3-byte far pointer to data |
| `far_table` | 3-byte far pointer to a table |
| `far_code` | 3-byte far pointer to code |
| `far_dmd_fullframe` | 3-byte far pointer to a DMD frame |

### `[symbols]`

Defines RAM and ASIC address equates emitted near the top of the disassembly output.

```ini
[symbols]
_ROM_BANK_SHADOW = 0x0011
_ASIC_ROM_PAGE   = 0x3ffc
DMD_FRAMEBUFFER  = 0x3800
```

Symbol names follow the same naming rules as labels.

### `[docs]`

Attaches a documentation string to any address — code, table, or data. The string is emitted as a `; doc …` comment in the disassembly.

```ini
[docs]
0x8990 = "WPC far-call helper\; consumes a 3-byte far-code pointer."
0x82d1 = Short inline byte selects behavior mode.
Bff_A8001 = Headerless far-code dispatcher table.
B3c_A4001 = Classic WPC counted string pointer table.
```

Values may be unquoted (everything after `=` up to the end-of-line comment) or quoted. Inside quotes: `\;`, `\#`, `\\`, `\"`, and `\n` are recognized.

**RAM / ASIC addresses.** A doc on an address that is not part of the ROM (RAM and ASIC I/O, `0x0000`–`0x3fff`) would otherwise have no disassembly line to attach to. Such docs are emitted near the top of the output instead:

- If a `[symbols]` equate names the address, the doc is emitted directly above that equate:

  ```asm
  ; doc Player 1 score, 4-byte packed BCD.
  Score_P1 = 0x0150
  ```

- Otherwise it is listed in a dedicated `; ---- RAM/ASIC documentation (no symbol) ----` block:

  ```asm
  ; 0x00a3:
  ; doc scratch flag used by the attract loop.
  ```

**Backwards compatibility:** The legacy section names `[routine_docs]` and `[table_docs]` are still accepted and merged into `[docs]` on load. New files and all write paths (GUI overlay, `apexini merge`) always use `[docs]`. To migrate existing files in-place:

```sh
apexini migrate myconfig.ini          # rewrites in-place, writes .bak backup
apexini migrate base.ini overlay.ini  # migrate multiple files at once
```

### `[types]`

Declares named byte or word types with optional symbolic enum values. When a named type is used as a field kind in `[inline]` or `[tables]`, the disassembler annotates the emitted value with its symbolic name.

Syntax:

```ini
[types]
TypeName:byte = value:EnumName[, value:EnumName...]
TypeName:word = value:EnumName[, value:EnumName...]
```

Example:

```ini
[types]
game_mode:byte = 0x00:attract, 0x01:game, 0x02:tilt

[inline]
0x8100 = game_mode
```

Output when the inline byte is `0x01`:

```asm
        INLINE_BYTE 0x01 ; for JSR SetMode game_mode=game
```

If the value does not match any declared enum name, it is emitted as a hex literal with the type name:

```asm
        INLINE_BYTE 0x42 ; for JSR SetMode game_mode=0x42
```

Named types may also appear in `[schemas]` and `[tables]` row definitions. Only `byte` and `word` base kinds are supported.

**Size limit:** the combined text of a single type's enum value list (all `value:name` pairs, however they are split across continuation lines) is parsed through a fixed buffer of 65536 bytes — roughly 3000 enum values at typical name lengths. A single physical config line is likewise limited to 65536 bytes. Definitions beyond these limits are truncated; raise `CONFIG_MAX_TYPE_VALUES` / `CONFIG_MAX_LINE` in `src/apex_config.c` (and rebuild) if you need more.

## Field Kind Reference

Used in `[inline]`, `[schemas]`, and `[tables]` row definitions.

| Kind | Size | Meaning |
|---|---|---|
| `byte` | 1 | Raw byte |
| `byte[N]` | N | N consecutive raw bytes |
| `word` | 2 | Big-endian 16-bit value |
| `word[N]` | 2×N | N consecutive words |
| `ptr16_string` | 2 | 16-bit pointer to a string |
| `ptr16_data` | 2 | 16-bit pointer to data |
| `ptr16_code` | 2 | 16-bit pointer to code |
| `ptr16_table` | 2 | 16-bit pointer to a table |
| `ptr16_dmd_fullframe` | 2 | 16-bit pointer to a DMD frame |
| `far_string` | 3 | Far pointer (`addr_hi, addr_lo, bank`) to a string |
| `far_data` | 3 | Far pointer to data |
| `far_table` | 3 | Far pointer to a table |
| `far_code` | 3 | Far pointer to code |
| `far_dmd_fullframe` | 3 | Far pointer to a DMD frame |
| `TypeName` | 1 or 2 | Named type from `[types]`; inherits its base kind |

Repeat counts (`byte[N]`, `word[N]`) expand to N consecutive fields of that kind and are emitted as N separate lines.

## Validation

The config loader aborts with an error for:

- Invalid or duplicate label/symbol names.
- Names that collide with assembler keywords.
- The same label name mapped to more than one address.
- An `[entries]` address that also appears in `[data]` at the same location.
- An `[entries]` address that also appears in `[tables]` at the same location.
- Invalid syntax in any section.

Truncated or structurally invalid inline payloads (payload extends beyond the known ROM content) are reported to `stderr` and emitted as a warning comment in the disassembly:

```asm
; WARNING: inline payload truncated for JSR Panic (expected 3 bytes, got 1)
```
