# ApexII

ApexII is a reverse engineering toolchain for Williams WPC (WPC/WPC-S/WPC95) with 6809 CPUs

It currently contains the following command-line tools:

- `apexdis`: ROM -> assembly disassembler
- `apexasm`: assembly -> ROM assembler
- `apextab`: heuristic helper for finding likely text and far-pointer tables
- `apexini`: config file utilities (syntax check, overlap detection, merge)
- `apexmatch`: transfers labels and inline signatures from an analysed ROM to a different ROM version by fingerprint matching
- `apexcompare`: compares two ROM versions and reports routines/strings/tables as identical, moved, changed, removed or added
- `apexmeta`: displays ROM metadata (OS version, game version, checksum, hashes) and can fix or disable the hardware checksum
- `apexdmd`: decodes WPC DMD (dot-matrix display) full-frame images to PBM/PGM, including whole frame tables

The project is optimized for reversible work:

1. disassemble a ROM
2. improve ROM-specific metadata in a config file
3. assemble the generated source again
4. verify byte-identical roundtrip output

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
- `build/apeximgui`
- `build/apextab`
- `build/apexini`
- `build/apexmatch`
- `build/apexcompare`
- `build/apexmeta`
- `build/apexdmd`

### Dependencies

For the full build including `apeximgui` you need:

- a C compiler
- a C++ compiler
- `make`
- `pkg-config`
- SDL2 development headers and libraries
- OpenGL development headers and libraries

The CLI tools do not depend on SDL2/OpenGL.

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

## Tool Overview

### `apeximgui`

`apeximgui` is the native desktop UI built on [Dear ImGui](https://github.com/ocornut/imgui). It provides interactive disassembly, inline editing, and cross-reference navigation on top of the same analysis engine used by the CLI tools.

**Launch:**

```sh
build/apeximgui <rom> [config.ini]   # open ROM with optional config
build/apeximgui                      # open ROM/INI file pickers at startup
```

When started without arguments (and no saved session), a file picker appears automatically — first to select the ROM, then optionally to select a config INI.

**File menu:**

| Item | Description |
|---|---|
| Save Overlay (`Ctrl+S`) | Write accumulated edits to `<config>.apeximgui.ini` |
| Save INI As… | Export the full merged config (base + all edits) to a new `.ini` file |
| Consolidate into Base INI | Merge overlay into the base INI and reset the overlay to empty |
| Re-analyze (`F5`) | Re-run analysis without clearing cached results |
| Force Full Re-analyze (`Shift+F5`) | Flush analysis cache and re-run from scratch |

**Windows menu:** All panels can be shown or hidden individually. The layout can be reset to the default via Windows → Reset Layout.

**Key panels:**

- **Disassembly** — scrollable listing with inline syntax highlighting and clickable label links
- **Labels / Banks / Bookmarks** — navigation panels
- **Edit** — set labels, classify bytes (code/data/string/table/inline), add docs
- **References** — incoming and outgoing cross-references for the selected address
- **Hex** — raw byte view, synced to disassembly selection
- **Inline Sigs / Code Entries** — bulk-edit inline signatures and code entry points
- **Symbols** — named RAM/hardware address definitions
- **ROM Map** — visual bank/block overview
- **Coverage** — per-kind classification percentages (code/data/table/sprite/unclassified/unknown/free) plus a worklist of unclassified gaps with click-to-jump and "jump to next gap"
- **ROM Info** — OS version, game version string, checksum status, CRC-32/SHA-1/SHA-256 hashes (read-only; hashes computed lazily on first open)
- **Match from Reference** — GUI front-end for `apexmatch` (see below)
- **ROM Compare** — GUI front-end for `apexcompare`: diff two ROM versions into a filterable table (identical/moved/changed/removed/added) with a Top-3 candidate sidebar for the selected routine

**Keyboard shortcuts (selected):**

| Key | Action |
|---|---|
| `J` / `K` | Move selection down / up |
| `N` / `P` | Next / prev block boundary |
| `F` / `Enter` | Follow link / jump to target |
| `[` / `]` | History back / forward |
| `G` | Go to address |
| `L` | Edit label |
| `C` | Mark as code |
| `D` | Mark as data |
| `Ctrl+S` | Save overlay |
| `F5` | Re-analyze |

**Match from Reference panel (Windows → Match from Reference):**

Transfers labels and inline signatures from an already-annotated reference ROM to the currently open ROM using the same fingerprint engine as `apexmatch`:

1. Open the panel, enter (or Browse for) the reference ROM and its config INI.
2. Click **Run Match**. With **System scan** enabled the scan phase runs automatically.
3. Results appear sorted by confidence tier — Exact (L1+L2, ≥90%), High (L1+callees, ≥75%), Medium (L1 only, ≥55%).
4. Click **Accept All Exact** to apply all exact matches in one step. The disassembly updates immediately.
5. Review High/Medium matches individually — click the address button to jump to the target location, then **Accept** to apply.

Accepted matches write labels, code entries, inline signatures, and routine docs to the active overlay. The panel stays open so results can be accepted incrementally.

#### Screenshots

![Main Interface](docs/screenshots/screenshot-01.png)
*Figure 1: Main Disassembly, Navigator, Labels, DMD View*

![Call Graph](docs/screenshots/screenshot-02.png)
*Figure 2: Disassembly with tooltip, shows instruction details

![Hardware Mapping](docs/screenshots/screenshot-03.png)
*Figure 3: Call Graph*


### `apexdis`

Usage:

```sh
build/apexdis [--xref] [--explain] <input-rom> <output-asm> [config.ini]
```

Examples:

```sh
build/apexdis roms/addam_h4.rom out/addam_h4.asm
build/apexdis --xref roms/addam_h4.rom out/addam_h4.asm tests/addam_inline.ini
build/apexdis --xref --explain roms/addam_h4.rom out/addam_h4.explain.asm tests/addam_inline.ini
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

### `apexasm`

Usage:

```sh
build/apexasm <output-rom> <input-asm> [input-asm ...]
```

Examples:

```sh
build/apexasm out/addam_h4.rebuilt out/addam_h4.asm
build/apexasm out/patched.rom base.asm patch.asm
```

Later input files may overwrite earlier bytes at the same `.BANK`/`.ORG` address.


### `apextab`

Usage:

```sh
build/apextab <rom_file>
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

### `apexini`

Usage:

```sh
build/apexini <check|overlaps|merge> ...
```

Three subcommands for working with config files.

#### `check`

```sh
build/apexini check <file.ini> ...
```

Loads each config file and reports any syntax or semantic errors — invalid addresses, bad specs, duplicate label names, unknown section names, and so on. Prints a summary of entry counts on success.

```text
tests/addam_inline.ini: OK  labels=448  entries=0  inline=73  data=0  tables=12  types=0
tests/config_duplicate_label.ini: error: label 'DuplicateName' is defined at more than one address
```

The exit code is 0 if all files are valid, 1 if any file has an error.

#### `overlaps`

```sh
build/apexini overlaps <file.ini>
```

Loads the config (including any `include =` chain) and checks for two classes of problem:

- **Same-address conflicts**: the same address is classified in two different sections, e.g. both `[inline]` and `[entries]`.
- **Range overlaps**: an entry with a known byte length extends into the start of another entry in the same bank. Known lengths are: `inline` signatures (always known), `bytes[N]` data, `far_*` data and fields (3 bytes), `dmd_fullframe` data (512 bytes), and `rows[N](schema)` tables.

```text
conflict: Bff_A8005  [entries] code  vs  [inline] 1 bytes
overlap:  [inline] B3d_A7840 (byte, 4 bytes, ends 0x7843) into [data] B3d_A7842 (bytes[10])
```

Exits with 0 if no problems are found, 1 otherwise.

#### `merge`

```sh
build/apexini merge <out.ini> <file.ini> ...
```

Loads all input files sequentially into one combined config (later files override earlier ones for the same address, following the same deduplication rules as `load_config`). Writes a clean sorted INI to `<out.ini>`:

- `[types]`, `[schemas]`, `[symbols]` — sorted alphabetically by name
- `[labels]`, `[entries]`, `[inline]`, `[data]`, `[tables]`, `[docs]` — sorted by bank then address

Conflicts that would be rejected during normal loading (e.g. the same label name mapped to two different addresses in two files) are reported as merge errors and the output file is not written.

```sh
build/apexini merge combined.ini base.ini overlay.apexgui.ini extra.ini
# merged 3 file(s) into combined.ini
```

#### `migrate`

```sh
build/apexini migrate <file.ini> ...
```

Rewrites each file in-place, replacing the legacy `[routine_docs]` and `[table_docs]` sections with a single `[docs]` section. A `.bak` backup is written before overwriting. Use this to bring older config files up to the current format:

```sh
build/apexini migrate tests/myconfig.ini
# migrated tests/myconfig.ini  (12 doc entries → [docs])
```

### `apexmatch`

Usage:

```sh
build/apexmatch <source.rom> <source.ini> <target.rom> [options]
```

Examples:

```sh
# Transfer WPC OS labels from an analysed Addams Family ROM to Gilligan's Island
build/apexmatch roms/addam_h4.rom addam_h4_complex.ini roms/afgldlx3.rom \
    --scan --output afgldlx3_os_labels.ini --stats

# Transfer labels between two versions of the same game (tighter match)
build/apexmatch roms/taf_l7.rom taf_l7.ini roms/taf_l8.rom \
    --inject-paged --output taf_l8_from_l7.ini
```

Options:

- `--min-confidence N`: skip matches below N% (default: 55)
- `--min-instrs N`: require at least N instructions for medium-confidence matches (default: 5; reduces false positives for short stub routines)
- `--inject-paged`: also inject paged-bank entry points from the source into the target analysis (default: system bank only — safe for cross-game matching)
- `--scan`: after the code-flow phase, scan every byte position in the target's system bank for exact L1+L2 fingerprint matches; finds functions that are at slightly different addresses and not reachable by code-flow injection alone (e.g. `Delay_N`, `Mem_*` helpers that moved by a few bytes between OS builds); adds ~1-2 s of runtime; recommended for cross-game matching
- `--output FILE`: write the generated `.ini` to FILE instead of stdout
- `--stats`: print match counts and fingerprint totals to stderr
- `--verbose`: list named source functions that found no match (helps identify what the target ROM is still missing)

`apexmatch` fingerprints each named code label in the source using three independent FNV-32 hashes:

- **L1** — mnemonic sequence only (address-independent): catches routines that have simply moved to a new address
- **L2** — mnemonic sequence + immediate operand values: distinguishes routines with different constants
- **L3** — sequence of callee L1 hashes: structural match based on what the routine calls

Scoring: exact (L1 + L2, 90) / high (L1 + callees, 75) / medium (L1 only, 55). Conflicts — two source functions matched to the same target address — are resolved by keeping the higher-confidence match.

The output is a `.ini` overlay containing:

- `[labels]` — matched function addresses with confidence annotations
- `[inline]` — inline-byte signatures transferred from the source config
- `[docs]` — documentation strings transferred from the source config

The generated file can be used directly with `apexdis` and validated with `apexini check`.

**Typical use cases:**

1. **Cross-game WPC OS knowledge transfer**: Addams Family, Gilligan's Island, and other WPC games share the same operating system in the system bank (0x8000–0xffff). After fully annotating one game's OS layer, `apexmatch` propagates labels like `JSR_Far`, `DisplayEffect_Start`, `LampLogical_SetInline`, and their inline signatures to any other WPC ROM automatically. In practice this yields ~250–300 exact matches for the WPC OS layer.

2. **Same-game version upgrade**: When a new ROM version is released, routine addresses often shift. `apexmatch --inject-paged` transfers the full label set including game-specific routines, providing a starting point that requires only incremental review rather than starting from scratch.

### `apexcompare`

Compares two ROM versions and classifies every code routine, string and
configured table as **identical**, **moved** (relocated, identical body),
**changed** (modified in place), **removed** (only in A) or **added** (only in
B). Correspondence is structural — each routine in A is re-fingerprinted
against B's raw bytes at the same address (capped to A's instruction count), so
an unchanged routine matches regardless of B's own analysis, and relocated
bodies are still found by their fingerprint elsewhere.

Usage:

```sh
build/apexcompare <romA> <romB> [iniA] [options]
```

Options:

- `--ini-b FILE` — config for ROM B (enables discovery of B-only tables)
- `--inject-paged` — also seed B with A's paged-bank entry points (default:
  system-bank only, safer when banks have shifted)
- `--min-instrs N` — skip code routines shorter than N instructions (default 5)
- `--no-code` / `--no-strings` / `--no-tables` — restrict the comparison
- `--show-identical` — list identical entries too (default: summarised only)
- `--only STATUS` — print only one status (identical/moved/changed/removed/added)

Example:

```sh
build/apexcompare roms/taf_l7.rom roms/taf_l8.rom tests/taf_l7.ini
```

```
417 identical, 3 moved, 38 changed, 12 removed, 25 added
changed   code   Bff_A9000  -> Bff_A9000   ScoreAdd    same flow, operands changed
moved     code   Bff_A8123  -> Bff_A8140   CheckSwitch relocated
...
```

The same engine drives the **ROM Compare** GUI window (a sortable/filterable
diff table plus a Top-3 candidate sidebar for the routine under the cursor).

### `apexmeta`

Usage:

```sh
build/apexmeta <rom-file> [options]
```

Displays metadata for a WPC ROM file and optionally modifies the checksum.

**Example output:**

```text
ROM:           roms/addam_h4.rom
Size:          524288 bytes (512 KB)

OS Version:    3.21
Game Version:  REV. H-4  (offset 0x4CDC2)

Checksum:
  Stored:      0xFB06  (CPU 0xFFEE / file +0x7FFEE)
  Computed:    0xFB06
  Status:      VALID
  Delta:       0x3404  (CPU 0xFFEC / file +0x7FFEC)

Hashes:
  CRC-32:      D0BBD679
  SHA-1:       ebd8c4981dd68a4f8e2dea90144486cb3cbd6b84
  SHA-256:     571b53155d62ae9ed8f8b357ae33cecb1cbc72b642aca3a67b06118a786e73b9
```

**Metadata fields:**

- **OS Version** — major.minor decimal (bytes immediately before the reset routine, located via the 6809 reset vector at `0xFFFE`)
- **Game Version** — first occurrence of the pattern `REV. [A-Z]-[0-9]+` in the ROM
- **Checksum** — 16-bit sum of all ROM bytes mod 65536, stored at CPU `0xFFEE`; the delta/fixup word at `0xFFEC` participates in the sum and can be adjusted to satisfy the equation without changing any code
- **Delta = 0x00FF** — special value that causes the WPC hardware to skip the checksum check entirely

**Options:**

| Option | Description |
|---|---|
| `--fix -o out.rom` | Recompute a valid (checksum, delta) pair and write to `out.rom` |
| `--disable -o out.rom` | Set delta to `0x00FF` to bypass the hardware checksum check |
| `--verify` | Exit 0 if checksum valid, 1 if invalid (no other output; useful in scripts) |
| `-o <file>` | Required with `--fix` / `--disable`; output ROM path |

No external dependencies — CRC-32, SHA-1, and SHA-256 are implemented inline.

### `apexdmd`

Decodes WPC DMD (dot-matrix display) full-frame images out of a ROM and writes
them as portable bitmap/greymap files you can open in any image viewer. A DMD
full frame is 128×32 pixels; the hardware shows 4 brightness levels by combining
two 1-bit planes.

Usage:

```sh
build/apexdmd <rom> <Bxx_Ayyyy|0xhhhh> <out.pbm>            # one plane  -> PBM (1-bit)
build/apexdmd --pair <rom> <addr0> <addr1> <out.pgm>        # two planes -> PGM (4 grey levels)
build/apexdmd --table <rom> <config.ini> <table_addr> <outdir>   # whole frame table
```

Addresses use the same forms as the rest of the toolchain: `Bxx_Ayyyy`
(`xx` = bank in hex, `yyyy` = CPU address) or `0xhhhh` for the system bank.

**Single frame** — decode one full frame to a 1-bit PBM:

```sh
build/apexdmd roms/taf_l7.rom B34_A4001 out/frame.pbm
```

**Plane pair** — many DMD images store two planes that combine into a 4-level
greyscale frame. Give both plane addresses to get a PGM:

```sh
build/apexdmd --pair roms/taf_l7.rom B34_A4001 B34_A41c2 out/frame.pgm
```

**Frame table** — bulk-decode every frame referenced by a far-pointer table. The
table must be classified in the config as a headerless `rows[n](far_dmd)` (or
`rows[n](far_data)`) table — exactly what the GUI's *Tables* search or a `[tables]`
entry produces:

```sh
build/apexdmd --table roms/taf_l7.rom tests/taf_l7.ini Bff_Ae735 out/dmd_frames
```

This writes one `frameNNN.pbm` per row into `out/dmd_frames/` plus a
`summary.tsv` listing each frame's index, target address, decoder type, and byte
length. It is the quickest way to dump an entire animation for review.

The decoder understands the standard WPC full-frame encodings (raw and the
run-length/“skip” variants); the printed `type=0x..` value reports which one was
used. No external dependencies — PBM/PGM are written directly.

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

See [docs/config-format.md](docs/config-format.md) for the full reference.

Quick example:

```ini
include = common.ini

[options]
labels_are_entries = false

[labels]
0x8990 = FarCall
B3d_A784b = ThingAwardEntry

[entries]
0x8990 = code

[inline]
0x8990 = far_code
0x8123 = ptr16_data, far_data, ptr16_code, word, byte[2]
0x8456 = far_code, flow_stop

[schemas]
menu_row = byte, word, far_data

[tables]
B3c_A4001 = counted(ptr16_string)
B20_A4007 = rows[4](menu_row)

[data]
Bff_A8002 = bytes[3]
B20_A400b = far_code

[symbols]
_ROM_BANK_SHADOW = 0x0011

[docs]
0x8990 = "WPC far-call helper\; consumes a 3-byte far-code pointer."
```

An inline signature may carry a trailing `flow_stop` token (e.g.
`far_code, flow_stop`). It marks the target as a tail-call helper that never
returns to its caller — after the inline arguments are consumed, code decoding
stops instead of running on into whatever follows. This avoids false
`classification_conflict` warnings for `JSR`-style dispatchers like `JMP_Far`.

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

[docs]
B20_A40d5 = "Roundtrip GUI routine doc"
```

Current GUI overlay sections:

- `[labels]`
- `[inline]`
- `[entries]`
- `[data]`
- `[tables]`
- `[docs]`

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

## Practical Roundtrip Example

```sh
make
build/apexdis roms/addam_h4.rom out/addam_h4.asm tests/addam_inline.ini
build/apexasm out/addam_h4.rebuilt out/addam_h4.asm
cmp roms/addam_h4.rom out/addam_h4.rebuilt
```

If `cmp` prints nothing and exits with `0`, the roundtrip is byte-identical.

## Current Limitations

- `apextab` is heuristic and intentionally narrow
- `apeximgui` uses overlay configs, but `apexdis` only sees them if you pass the overlay explicitly
- the GUI is single-process/local-server oriented and has no auth or multi-user model

---

## Legal & Safety Notice

**Disclaimer of Liability**: ApexII is an analysis tool. Modifying ROM images and running them on real hardware can lead to permanent damage to your pinball machine (e.g., burnt solenoids, ASIC failure). The authors take no responsibility for any hardware damage or loss of data.

**Copyright Note**: This tool is intended for educational purposes and the preservation of pinball history. Users are responsible for complying with local copyright laws regarding the disassembly and modification of proprietary firmware.

---

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for the full text.


## Authors

Supervisor: Walter Haslbeck (redball@haslbeck.org), Code by AI
