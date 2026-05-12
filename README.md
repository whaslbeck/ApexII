# ApexII - WPC Pinball Reverse Engineering Suite

ApexII is a specialized toolset designed for the high-performance reverse engineering of WPC (Williams Pinball Controller) software. It provides both an advanced static analyzer (CLI) and a rich graphical environment (GUI) to map hardware interactions, discover data structures, and reconstruct documented assembly code.

## Key Features

*   **Advanced Static Analysis**: Automated code-flow discovery, including support for WPC's specific "far-call" mechanisms and banked ROM layouts.
*   **Interactive UI (apeximgui)**: A professional ImGui-based suite featuring:
    *   **Visual Call Graph**: Bowtie-layout for recursive caller/callee traversal.
    *   **Hardware Device View**: Central mapping of ASIC registers ($3F00-$3FFF) to code usage.
    *   **Automatic Table Discovery**: Pattern-based search for text and data tables.
    *   **Hex Inspector**: Multi-format inspection (Hex, Dec, Bin, ASCII) with ROM-to-CPU address mapping.
    *   **DMD Preview**: Real-time decoding and scrubbing of DMD full-frame images.
*   **Modular Architecture**: Extensible design allowing for deep integration of game-specific logic.

---

## Tool Collection

### `apexdis` (CLI Disassembler)
The core analyzer. It processes a ROM image and a configuration overlay to produce a fully annotated assembly listing.

**Usage:**
```bash
apexdis roms/romimage.rom out/romimage.asm
apexdis --xref roms/romimage.rom out/romimage.asm tests/romimage.ini
apexdis --xref --explain roms/romimage.rom out/romimage.asm tests/romimage.ini
```

Flags:

- `--xref`: emit typed `referenced_by` comments and an `XREF INDEX`
- `--explain`: emit classification/provenance comments such as `config_entry`, `config_data`, `code_flow`, `table_ptr16_data_ref`, and so on

The disassembler expects a 512 KiB or 1 MiB WPC ROM. CPU-visible addresses are used throughout assembly and config files:

- paged ROM window: `0x4000..0x7fff`
- fixed system area: `0x8000..0xffff`

Generated assembly includes:

- `.ROM_SIZE <bytes>`
- `.BANK <index>` or system bank output in the fixed area
- `.ORG <cpu-address>`
- `BANK_ID <byte>` at the start of paged banks
- generated labels like `B20_A4000` or `Bff_A8990`
- comments with logical bank, CPU address, and physical ROM offset
- optional xref and explain comments

### `apextab`

Usage:

```sh
apextab <rom_file>
```

`apextab` is a heuristic scanner that currently looks for:

- likely counted string pointer tables
- likely headerless far-pointer tables that reference those string tables

It prints candidate config lines such as:

```ini
B20_A4007 = counted(ptr16_string)
Bff_A8123 = rows[3](far_data)
```

Treat its output as suggestions, not ground truth.


### `apeximgui` (Interactive GUI)
The primary analysis workstation. It allows for live exploration, labeling, and structure definition.

**Usage:**
```bash
apeximgui ROM_PATH [CONFIG_PATH]
```
*If no CONFIG_PATH is provided, it will start with a fresh analysis. Changes can be saved as an `.ini` overlay.*

### `apexasm` (6809 Assembler)
A assembler optimized for WPC development. Used to re-assemble the reconstructed code back into a valid ROM image.

**Usage:**
```bash
apexasm game.asm output.rom
```

### `apexdmd` (DMD Utility)
A utility to decode and extract DMD frames from binary data.

### `apexini` (Config File Utilities)

A set of utilities for inspecting and maintaining `.ini` config files.

**Usage:**
```bash
apexini check   <file.ini> ...
apexini overlaps <file.ini>
apexini merge   <out.ini> <file.ini> ...
```

**`check`** — validates one or more config files and reports any errors (bad addresses, invalid specs, duplicate label names, etc.). Prints entry counts on success:

```text
myconfig.ini: OK  labels=42  entries=7  inline=15  data=3  tables=6  types=2
myconfig.ini: error: label 'FarCall' is defined at more than one address
```

**`overlaps`** — detects address conflicts and byte-range overlaps within a config:

- same address classified in two different sections (e.g. both `[inline]` and `[entries]`)
- a ranged entry (inline sig, `bytes[N]`, `far_*`, `rows[N](schema)`) extending into another entry

```text
conflict: Bff_A8005  [entries] code  vs  [inline] 1 bytes
overlap:  [inline] B3d_A7840 (byte, 4 bytes, ends 0x7843) into [data] B3d_A7842 (bytes[10])
```

**`merge`** — combines multiple config files into one clean sorted output. Later files override earlier ones for the same address. Sections are sorted alphabetically (types, schemas, symbols) or by bank+address (all others). Useful for flattening an `include =` chain into a single self-contained file:

```bash
apexini merge combined.ini base.ini overlay.apexgui.ini
```

---

## Build And Test

```sh
make
make test
```

Linux is the primary development platform and the default build path.

If you only need the command-line tools, you can build just those with:

```sh
make apexcli
```

Build products:

- `build/apexdis`
- `build/apexasm`
- `build/apexgui`
- `build/apeximgui`
- `build/apextab`
- `build/apexini`

### Dependencies

For the full build including `apeximgui` you need:

- a C compiler
- a C++ compiler
- `make`
- `pkg-config`
- SDL2 development headers and libraries
- OpenGL development headers and libraries

The browser UI `apexgui` and the CLI tools do not depend on SDL2/OpenGL.

### Linux

The current `Makefile` is aimed at Linux first.

Typical requirements:

- `gcc`
- `g++`
- `make`
- `pkg-config`
- `libsdl2-dev`
- OpenGL development package (`libgl-dev` or distro equivalent)

Then:

```sh
make
make test
```

### Windows

Use **MSYS2 MinGW-w64**. The project is not currently set up for a direct MSVC / Visual Studio build.

Recommended environment:

- MSYS2 `UCRT64` or `MINGW64` shell
- `make`
- `gcc`
- `g++`
- `pkg-config` / `pkgconf`
- `git`
- `SDL2`

Typical MSYS2 package set:

- `base-devel`
- `mingw-w64-ucrt-x86_64-toolchain` or `mingw-w64-x86_64-toolchain`
- `mingw-w64-ucrt-x86_64-pkgconf` or `mingw-w64-x86_64-pkgconf`
- `mingw-w64-ucrt-x86_64-SDL2` or `mingw-w64-x86_64-SDL2`
- `git`

Notes:
- Windows build is not yet tested, main development and test platform is Linux and MacOS (It would be nice to hear from you if you manage to build and run the toolkit on windows)
- `apeximgui` links OpenGL as `-lopengl32` on Windows.
- `make test` expects a POSIX shell environment and tools such as `sh`, `cmp`, `grep`, `sed`, `od`, and `wc`, so run it from the MSYS2 shell, not plain `cmd.exe`.

### macOS

Use Apple Clang plus Homebrew-installed libraries.

Requirements:

- Xcode Command Line Tools
- `make`
- `pkg-config`
- `sdl2`

Typical setup:

```sh
xcode-select --install
brew install pkg-config sdl2
make
make test
```

Notes:

- `apeximgui` links against `-framework OpenGL` on macOS.
- OpenGL is deprecated on macOS, but it is sufficient for the current ImGui frontend.
- The CLI test suite uses standard Unix command-line tools that are available on macOS by default.
---

## Assembly Syntax

Core directives:

```asm
.ROM_SIZE 524288
.BANK 0x00
.ORG 0x4000
BANK_ID 0x20
FILL_TO_BANK_END
```

Data directives:

```asm
.DB 0x12, 0x34
.DW 0x8000
STRING "PRESS \"ENTER\""
```

Far pointers are encoded as `addr_hi, addr_lo, bank_byte`:

```asm
FAR_CODE B21_A4006, 0x01
FAR_STRING B21_A4001, 0x01
FAR_PTR B21_A4004, 0x01
FAR_TABLE B21_A5000, 0x01
```

If the encoded bank byte matches the generated bank label, the explicit bank byte may be omitted:

```asm
FAR_CODE B3d_A784b
```

Table pseudo-ops:

```asm
TABLE_PTR B3b_A415c
TABLE_FAR_CODE Bff_Aedbe
TABLE_FAR_STRING B21_A4001, 0x01
TABLE_FAR_PTR B21_A4004, 0x01
TABLE_FAR_TABLE B21_A5000, 0x01
```

Inline payload after `JSR`/`JMP` is emitted indented:

```asm
    JSR FarCall
        INLINE_FAR_CODE B38_A4001, 0x18 ; for JSR FarCall

    JSR Panic
        INLINE_BYTE 0x04 ; for JSR Panic

    JSR InlineComplex
        INLINE_PTR B21_A4004 ; for JSR InlineComplex
        INLINE_FAR_PTR B21_A4004, 0x01 ; for JSR InlineComplex
        INLINE_CODE_PTR B21_A4006 ; for JSR InlineComplex
        INLINE_WORD 0x1234 ; for JSR InlineComplex
```

`.DB` output now includes end-of-line comments with logical start address and ASCII preview:

```asm
    .DB 0x41, 0x42, 0x00 ; 0x4000 |AB.|
```

## Config File Format

Config files are INI-like.

- empty lines are ignored
- text after unescaped `;` or `#` is ignored outside quotes
- use `\;`, `\#`, `\\`, `\"`, `\n` inside quoted values
- the same section may appear multiple times
- repeated sections accumulate
- later entries for the same key override earlier ones in practice
- `include = other.ini` is supported at top level

Relative includes are resolved relative to the including file.

Example:

```ini
include = common_wpc.ini
include = addams_tables.ini
```

Addresses:

- system addresses may be written as `0x8990` or `Bff_A8990`
- banked addresses use generated bank-label keys like `B3b_A4001`

### `[options]`

```ini
[options]
labels_are_entries = false
```

Supported options:

- `labels_are_entries`
  - legacy behavior default: `true`
  - recommended new behavior: `false`
  - when `false`, `[labels]` only names addresses; `[entries]`, `[data]`, and `[tables]` control classification

### `[labels]`

Assigns names to known code/data/table locations.

```ini
[labels]
0x8990 = FarCall
0x82d1 = Panic
B3d_A784b = ThingAwardFarEntry
```

Multiple labels may point at the same address. They are emitted as aliases.

### `[entries]`

Adds explicit code entry points.

```ini
[entries]
0x8123 = code
B38_A5100 = code
```

`= entry` is also accepted.

### `[inline]`

Defines routines that consume inline bytes after `JSR`/`JMP`.

Examples:

```ini
[inline]
0x8990 = far_code, FarCall
0x82d1 = 1, Panic
0x8c97 = bytes:2, far_code, Unknown_take_2_bytes_then_far_code_8c97
0x8123 = ptr16_data, far_data, ptr16_code, word, bytes[2]
0x8200 = byte:mode
0x8210 = far_code:handler
```

Supported forms:

- `<addr> = <N>`
- `<addr> = <N>, <alias>`
- `<addr> = byte:<name>`
- `<addr> = far_code`
- `<addr> = far_code, <alias>`
- `<addr> = far_code:<name>`
- `<addr> = bytes:<N>, <field>`
- `<addr> = <field>[, <field>...]`

Inline fields use the same field grammar as table rows:

- `byte`
- `bytes[N]`
- `word`
- `ptr16_string`
- `ptr16_data`
- `ptr16_code`
- `ptr16_table`
- `far_string`
- `far_data`
- `far_table`
- `far_code`

Important limitation:

- a full multi-parameter naming model does **not** exist yet
- current named support is only for the simple single-parameter cases such as `byte:name` or `far_code:name`
- multiple typed inline parameters work, but not with a separate persisted name for every field

### `[schemas]`

Reusable table row schemas.

```ini
[schemas]
menu_row = bytes[1], word, far_data
score_event = byte, ptr16_string, far_code
```

Usage:

```ini
[tables]
B20_A4007 = rows[1](menu_row)
B30_A5000 = counted(score_event)
```

### `[tables]`

Defines table starts and row layouts.

Counted tables have a 3-byte header:

- 2 bytes row count
- 1 byte row width

Headerless tables specify the row count in config.

Examples:

```ini
[tables]
B3c_A4001 = counted(ptr16_string)
B3b_A5588 = counted(ptr16_data)
Bff_A8001 = rows[116](far_code)
B20_A4007 = rows[1](byte, far_data)
```

Legacy aliases are still accepted:

```ini
B3b_A4001 = counted_ptr16_string
Bff_A8001 = far_code[116]
```

Supported row fields:

- `byte`
- `byte[N]`
- `bytes[N]`
- `word`
- `word[N]`
- `ptr16_string`
- `ptr16_data` or `ptr16_ptr`
- `ptr16_code`
- `ptr16_table`
- `far_string`
- `far_data` or `far_ptr`
- `far_table`
- `far_code`

Examples:

```ini
B20_A4007 = rows[1](byte, far_data)
B30_A5000 = counted(bytes[2], word, ptr16_string, far_code)
```

### `[data]`

Defines standalone data ranges or standalone far pointers.

```ini
[data]
Bff_A8002 = bytes[3]
B3b_A415c = string
B20_A400b = far_code
B20_A4010 = far_string
B20_A4013 = far_data
B20_A4016 = far_table
```

Supported forms:

- `bytes[N]`
- `string`
- `far_code`
- `far_string`
- `far_data` or `far_ptr`
- `far_table`

Use `[data]` when bytes must stay data even if they look reachable from code.

### `[symbols]`

Defines RAM/ASIC symbols emitted as equates near the top of the output.

```ini
[symbols]
_ROM_BANK_SHADOW = 0x0011
_ASIC_ROM_PAGE = 0x3ffc
DMD_FRAMEBUFFER_3800 = 0x3800
```

### `[routine_docs]` And `[table_docs]`

Adds docs to routine/table comment blocks.

```ini
[routine_docs]
0x8990 = "WPC far-call helper\; consumes a far-code pointer.\nUse \# for literal hash."

[table_docs]
Bff_A8001 = Headerless dispatcher table containing far-code routine entry pointers.
```

Multi-line docs should be quoted and use `\n`.

## Cross References And Explain Output

Configured pointers and discovered flows produce typed reference comments where possible:

```asm
; referenced_by table:B20_A4007, code:B21_A4006, data:B20_A400d
```

Meaning:

- `code:` instruction flow or inline code pointer
- `table:` configured table field
- `data:` configured standalone pointer/data field

`--xref` also emits a grouped `XREF INDEX` at the end of the file.

`--explain` emits provenance comments such as:

```asm
; explain label source=config_label
; explain kind=code source=config_entry
; explain inline source=config_inline
```

These are useful for debugging misclassification and config interactions.

## GUI Overlay Files

The GUI writes an overlay config next to the chosen base config:

```text
tests/taf_l7_minimal.ini.apexgui.ini
```

Typical contents:

```ini
; Apex GUI overlay
include = taf_l7_minimal.ini

[labels]
B20_A40d5 = RoundtripGuiLabel

[inline]
B20_A40d5 = far_code

[entries]
B20_A40d5 = code

[routine_docs]
B20_A40d5 = "Roundtrip GUI routine doc"
```

Current GUI overlay sections:

- `[labels]`
- `[inline]`
- `[entries]`
- `[data]`
- `[tables]`
- `[routine_docs]`
- `[table_docs]`

## Diagnostics And Validation

The loader rejects several ambiguous or unsafe states:

- invalid label or symbol names
- names colliding with assembler syntax like `JSR`, `STRING`, `TABLE_PTR`
- same label name assigned to more than one address
- exact address conflicts between `[entries]` and `[data]`
- exact address conflicts between `[tables]` and `[data]`
- invalid inline/table/data syntax

Truncated or invalid inline payloads are reported to `stderr` and also emitted as warning comments in the disassembly.

The roundtrip test script reports the first byte mismatch like this:

```text
mismatch rom=0x012345 bank=0x28 cpu=0x4567 original=0xab rebuilt=0xcd
```

## Far Pointer Bank Bytes

Far pointers are `address-high, address-low, bank-byte`.

- `0xff` means fixed system area
- for paged targets, the disassembler first interprets the byte as a ROM bank ID
- if that fails, it falls back to physical bank index interpretation

When encoded bank byte and generated label bank differ, the emitted pseudo-op keeps the explicit byte:

```asm
INLINE_FAR_CODE B38_A4001, 0x18
```

That preserves byte-identical assembly.

---

## Usage Guide

### General Workflow
1.  **Open** the ROM in `apeximgui`.
2.  **Analyze**: Use the "Hardware" window to find solenoid/lamp handlers.
3.  **Label**: Name routines and data ranges to improve readability.
4.  **Tables**: Use "Search Tables (Auto)" to find text/data structures. Define other tables (e.g. jump-tables without the standard WPC table header)
5.  **Export**: Save your progress as an `.ini` overlay.
6.  **Listing**: Generate a full `.asm` file using `apexdis`.

Now you can modify the assembly and rebuild an WPC Rom with apexasm. NOTE: checksum is not (yet) fixed automatically, if you change the ROM contents you have to fix the checksum with external tools. 

### apeximgui Screenshots
![Main Interface](docs/screenshots/screenshot-01.png)
*Figure 1: Main Disassembly, Navigator, Labels, DMD View*

![Call Graph](docs/screenshots/screenshot-02.png)
*Figure 2: Disassembly with tooltip, shows instruction details

![Hardware Mapping](docs/screenshots/screenshot-03.png)
*Figure 3: Call Graph*

### Essential Hotkeys
*   `G` / `L`: Focus Goto / Label field.
*   `/`: Focus global filter.
*   `J` / `K`: Move selection down/up.
*   `F` / `Enter`: Follow link/pointer.
*   `X`: Show incoming references (XRefs).
*   `B`: Add bookmark at current location.
*   `Ctrl+S`: Save overlay file.
*   `Ctrl+F`: Global search.
*   `Alt + Left/Right`: Navigate through history.

---

## Configuration Format (.ini)

ApexII uses a simple INI-based format to persist your analysis. These files can be shared and are used by `apexdis` to generate commented code.

See [docs/config-format.md](docs/config-format.md) for the full reference.

Example:
```ini
[labels]
Bff_A8000 = Reset_Entry
B00_A4500 = Lamp_Handler

[data]
B00_A7000 = string
B21_A4000 = dmd_fullframe

[tables]
B01_A5000 = counted(ptr16_string)
```

---

## Legal & Safety Notice

**Disclaimer of Liability**: ApexII is an analysis tool. Modifying ROM images and running them on real hardware can lead to permanent damage to your pinball machine (e.g., burnt solenoids, ASIC failure). The authors take no responsibility for any hardware damage or loss of data.

**Copyright Note**: This tool is intended for educational purposes and the preservation of pinball history. Users are responsible for complying with local copyright laws regarding the disassembly and modification of proprietary firmware.

---

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for the full text.


## Authors

Supervisor: Walter Haslbeck (redball@haslbeck.org), Code: Codex, Gemini, Sonnet
