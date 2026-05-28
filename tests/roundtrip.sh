#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/out/roundtrip"

mkdir -p "$OUT"

report_mismatch()
{
    original=$1
    rebuilt=$2
    line=$(cmp -l "$original" "$rebuilt" | sed -n '1p') || true
    if [ -z "$line" ]; then
        printf 'mismatch: files differ in length\n' >&2
        return
    fi
    set -- $line
    pos=$1
    original_byte=$(printf '%02x' "$((8#$2))")
    rebuilt_byte=$(printf '%02x' "$((8#$3))")
    off=$((pos - 1))
    size=$(wc -c < "$original" | tr -d ' ')
    system_start=$((size - 32768))
    if [ "$off" -ge "$system_start" ]; then
        bank=ff
        cpu=$((0x8000 + off - system_start))
    else
        bank_index=$((off / 0x4000))
        bank_offset=$((bank_index * 0x4000))
        bank=$(od -An -tx1 -N1 -j "$bank_offset" "$original" | tr -d ' \n')
        cpu=$((0x4000 + off - bank_offset))
    fi
    printf 'mismatch rom=0x%06x bank=0x%s cpu=0x%04x original=0x%s rebuilt=0x%s\n' \
        "$off" "$bank" "$cpu" "$original_byte" "$rebuilt_byte" >&2
}

for rom in "$ROOT"/roms/*; do
    [ -f "$rom" ] || continue
    base=$(basename "$rom")
    asm="$OUT/$base.asm"
    rebuilt="$OUT/$base.rebuilt"

    "$ROOT/build/apexdis" "$rom" "$asm"
    "$ROOT/build/apexasm" "$rebuilt" "$asm"

    if cmp -s "$rom" "$rebuilt"; then
        printf 'PASS %s\n' "$base"
    else
        printf 'FAIL %s\n' "$base" >&2
        report_mismatch "$rom" "$rebuilt"
        exit 1
    fi
done

forward_rom="$OUT/forward_refs.rom"
"$ROOT/build/apexasm" "$forward_rom" "$ROOT/tests/forward_refs.asm"
actual=$(od -An -tx1 -N5 -j491520 "$forward_rom" | tr -d ' \n')
expected=80041234aa
if [ "$actual" = "$expected" ]; then
    printf 'PASS forward_refs.asm\n'
else
    printf 'FAIL forward_refs.asm: expected %s got %s\n' "$expected" "$actual" >&2
    exit 1
fi

instruction_rom="$OUT/instructions.rom"
"$ROOT/build/apexasm" "$instruction_rom" "$ROOT/tests/instructions.asm"
actual=$(od -An -tx1 -N115 -j491520 "$instruction_rom" | tr -d ' \n')
expected=1a508600b73ff2108e04008e8016bd801826ed7e80198019397f17483184308824a689004c33c43265e61ea6a6eccba780e7e3ae882410ae8c4ca69912341e891f01341637c904127c17481022ffca191d3c7f103f113f8b11c222c833cb44b01748fe1748109c1210bf174821ab16ffa80e12
if [ "$actual" = "$expected" ]; then
    printf 'PASS instructions.asm\n'
else
    printf 'FAIL instructions.asm: expected %s got %s\n' "$expected" "$actual" >&2
    exit 1
fi

puls_rom="$OUT/puls_pc_flow.rom"
puls_asm="$OUT/puls_pc_flow.disasm"
"$ROOT/build/apexasm" "$puls_rom" "$ROOT/tests/puls_pc_flow.asm"
"$ROOT/build/apexdis" "$puls_rom" "$puls_asm"
if grep -q 'PULS PC' "$puls_asm" &&
    grep -q '^; code_to_unclassified bank=0xff cpu=0x8002 rom=0x078002' "$puls_asm" &&
    grep -q '^    .DB 0x86, 0x00' "$puls_asm"; then
    printf 'PASS puls_pc_flow.asm\n'
else
    printf 'FAIL puls_pc_flow.asm\n' >&2
    exit 1
fi

label_flow_rom="$OUT/label_code_flow.rom"
label_flow_asm="$OUT/label_code_flow.disasm"
"$ROOT/build/apexasm" "$label_flow_rom" "$ROOT/tests/label_code_flow.asm"
"$ROOT/build/apexdis" "$label_flow_rom" "$label_flow_asm" "$ROOT/tests/label_code_flow.ini"
if grep -q '^Target:' "$label_flow_asm" &&
    grep -q '^    RTS' "$label_flow_asm" &&
    ! grep -q '\.DB.*0x39' "$label_flow_asm"; then
    printf 'PASS label_code_flow.asm\n'
else
    printf 'FAIL label_code_flow.asm\n' >&2
    exit 1
fi

data_range_rom="$OUT/data_ranges.rom"
data_range_asm="$OUT/data_ranges.disasm"
"$ROOT/build/apexasm" "$data_range_rom" "$ROOT/tests/data_ranges.asm"
"$ROOT/build/apexdis" "$data_range_rom" "$data_range_asm" "$ROOT/tests/data_ranges.ini"
if grep -q '^; kind data' "$data_range_asm" &&
    grep -q '^; data type=bytes length=3' "$data_range_asm" &&
    grep -q '^Bff_A8002:' "$data_range_asm" &&
    grep -q '^    .DB 0x39, 0x86, 0x00' "$data_range_asm" &&
    ! grep -q '^    RTS' "$data_range_asm"; then
    printf 'PASS data_ranges.asm\n'
else
    printf 'FAIL data_ranges.asm\n' >&2
    exit 1
fi

string_fixed_rom="$OUT/string_fixed.rom"
string_fixed_asm="$OUT/string_fixed.disasm"
"$ROOT/build/apexasm" "$string_fixed_rom" "$ROOT/tests/string_fixed.asm"
"$ROOT/build/apexdis" "$string_fixed_rom" "$string_fixed_asm" "$ROOT/tests/string_fixed.ini"
if grep -q '^; data type=string_fixed' "$string_fixed_asm" &&
    grep -q '^    STRING_FIXED "WORLD"' "$string_fixed_asm"; then
    printf 'PASS string_fixed.asm\n'
else
    printf 'FAIL string_fixed.asm\n' >&2
    exit 1
fi

string_lp_rom="$OUT/string_lp.rom"
string_lp_asm="$OUT/string_lp.disasm"
"$ROOT/build/apexasm" "$string_lp_rom" "$ROOT/tests/string_lp.asm"
"$ROOT/build/apexdis" "$string_lp_rom" "$string_lp_asm" "$ROOT/tests/string_lp.ini"
if grep -q '^; data type=string_lp' "$string_lp_asm" &&
    grep -q '^    STRING_LP "HELLO"' "$string_lp_asm"; then
    printf 'PASS string_lp.asm\n'
else
    printf 'FAIL string_lp.asm\n' >&2
    exit 1
fi

if "$ROOT/build/apexdis" "$data_range_rom" "$OUT/config_duplicate_label.disasm" \
    "$ROOT/tests/config_duplicate_label.ini" 2>"$OUT/config_duplicate_label.stderr"; then
    printf 'FAIL config_duplicate_label.ini\n' >&2
    exit 1
elif grep -q "label 'DuplicateName' is defined at more than one address" \
    "$OUT/config_duplicate_label.stderr"; then
    printf 'PASS config_duplicate_label.ini\n'
else
    printf 'FAIL config_duplicate_label.ini\n' >&2
    exit 1
fi

if "$ROOT/build/apexdis" "$data_range_rom" "$OUT/config_class_conflict.disasm" \
    "$ROOT/tests/config_class_conflict.ini" 2>"$OUT/config_class_conflict.stderr"; then
    printf 'FAIL config_class_conflict.ini\n' >&2
    exit 1
elif grep -q "config classifies 0x8002 as both code entry and data" \
    "$OUT/config_class_conflict.stderr"; then
    printf 'PASS config_class_conflict.ini\n'
else
    printf 'FAIL config_class_conflict.ini\n' >&2
    exit 1
fi

inline_truncated_rom="$OUT/inline_truncated.rom"
inline_truncated_asm="$OUT/inline_truncated.disasm"
inline_truncated_err="$OUT/inline_truncated.stderr"
"$ROOT/build/apexasm" "$inline_truncated_rom" "$ROOT/tests/inline_truncated.asm"
"$ROOT/build/apexdis" "$inline_truncated_rom" "$inline_truncated_asm" \
    "$ROOT/tests/inline_truncated.ini" 2>"$inline_truncated_err"
if grep -q '^; WARNING inline_truncated bank=0x20 cpu=0x4004 rom=0x000004 expected=5 available=0 for JSR ENTRY_RESET' "$inline_truncated_asm" &&
    grep -q '^warning: inline data truncated after JSR ENTRY_RESET at bank=0x20 cpu=0x4004 rom=0x000004: expected 5 byte(s), available 0' "$inline_truncated_err"; then
    printf 'PASS inline_truncated.asm\n'
else
    printf 'FAIL inline_truncated.asm\n' >&2
    exit 1
fi

inline_invalid_rom="$OUT/inline_invalid_far.rom"
inline_invalid_asm="$OUT/inline_invalid_far.disasm"
inline_invalid_err="$OUT/inline_invalid_far.stderr"
"$ROOT/build/apexasm" "$inline_invalid_rom" "$ROOT/tests/inline_invalid_far.asm"
"$ROOT/build/apexdis" "$inline_invalid_rom" "$inline_invalid_asm" \
    "$ROOT/tests/inline_invalid_far.ini" 2>"$inline_invalid_err"
if grep -q '^; WARNING inline_far_code_invalid bank=0x20 cpu=0x4004 rom=0x000004 target=0x1234 target_bank=0x7e for JSR ENTRY_RESET' "$inline_invalid_asm" &&
    grep -q '^warning: invalid inline far-code target after JSR ENTRY_RESET at bank=0x20 cpu=0x4004 rom=0x000004: target=0x1234 bank=0x7e' "$inline_invalid_err"; then
    printf 'PASS inline_invalid_far.asm\n'
else
    printf 'FAIL inline_invalid_far.asm\n' >&2
    exit 1
fi

banked_inline_rom="$OUT/banked_inline.rom"
banked_inline_asm="$OUT/banked_inline.disasm"
banked_inline_explain="$OUT/banked_inline.explain.disasm"
banked_inline_rebuilt="$OUT/banked_inline.rebuilt"
"$ROOT/build/apexasm" "$banked_inline_rom" "$ROOT/tests/banked_inline.asm"
"$ROOT/build/apexdis" "$banked_inline_rom" "$banked_inline_asm" \
    "$ROOT/tests/banked_inline.ini"
"$ROOT/build/apexdis" --explain "$banked_inline_rom" "$banked_inline_explain" \
    "$ROOT/tests/banked_inline.ini"
"$ROOT/build/apexasm" "$banked_inline_rebuilt" "$banked_inline_asm"
if cmp -s "$banked_inline_rom" "$banked_inline_rebuilt" &&
    grep -q '^Entry:' "$banked_inline_asm" &&
    grep -q '^    JSR BankInline' "$banked_inline_asm" &&
    grep -q '^        INLINE_BYTE 0x42 ; for JSR BankInline$' "$banked_inline_asm" &&
    grep -q '^; inline params=byte' "$banked_inline_asm" &&
    grep -q '^; explain label source=config_label' "$banked_inline_explain" &&
    grep -q '^; explain kind=code source=config_entry' "$banked_inline_explain" &&
    grep -q '^; explain inline source=config_inline_banked' "$banked_inline_explain"; then
    printf 'PASS banked_inline.asm\n'
else
    printf 'FAIL banked_inline.asm\n' >&2
    if ! cmp -s "$banked_inline_rom" "$banked_inline_rebuilt"; then
        report_mismatch "$banked_inline_rom" "$banked_inline_rebuilt"
    fi
    exit 1
fi

system_banked_inline_rom="$OUT/system_banked_inline.rom"
system_banked_inline_asm="$OUT/system_banked_inline.disasm"
system_banked_inline_rebuilt="$OUT/system_banked_inline.rebuilt"
"$ROOT/build/apexasm" "$system_banked_inline_rom" "$ROOT/tests/system_banked_inline.asm"
"$ROOT/build/apexdis" "$system_banked_inline_rom" "$system_banked_inline_asm" \
    "$ROOT/tests/system_banked_inline.ini"
"$ROOT/build/apexasm" "$system_banked_inline_rebuilt" "$system_banked_inline_asm"
if cmp -s "$system_banked_inline_rom" "$system_banked_inline_rebuilt" &&
    grep -q '^Entry:$' "$system_banked_inline_asm" &&
    grep -q '^Helper:$' "$system_banked_inline_asm" &&
    grep -q '^    JSR Helper$' "$system_banked_inline_asm" &&
    grep -q '^        INLINE_BYTE 0x42 ; for JSR Helper$' "$system_banked_inline_asm" &&
    grep -q '^; inline params=byte$' "$system_banked_inline_asm"; then
    printf 'PASS system_banked_inline.asm\n'
else
    printf 'FAIL system_banked_inline.asm\n' >&2
    if ! cmp -s "$system_banked_inline_rom" "$system_banked_inline_rebuilt"; then
        report_mismatch "$system_banked_inline_rom" "$system_banked_inline_rebuilt"
    fi
    exit 1
fi

classification_conflict_asm="$OUT/classification_conflict.disasm"
classification_conflict_err="$OUT/classification_conflict.stderr"
"$ROOT/build/apexdis" "$system_banked_inline_rom" "$classification_conflict_asm" \
    "$ROOT/tests/classification_conflict.ini" 2>"$classification_conflict_err"
if grep -q '^; WARNING classification_conflict bank=0xff cpu=0x8004' \
        "$classification_conflict_asm" &&
    grep -q 'warning: classification conflict at bank=0xff cpu=0x8004' \
        "$classification_conflict_err" &&
    grep -q 'code_from=' "$classification_conflict_err" &&
    grep -q 'data_from=' "$classification_conflict_err"; then
    printf 'PASS classification_conflict.ini\n'
else
    printf 'FAIL classification_conflict.ini\n' >&2
    exit 1
fi

types_asm="$OUT/types.disasm"
types_rebuilt="$OUT/types.rebuilt"
"$ROOT/build/apexdis" "$system_banked_inline_rom" "$types_asm" "$ROOT/tests/types.ini"
"$ROOT/build/apexasm" "$types_rebuilt" "$types_asm"
if cmp -s "$system_banked_inline_rom" "$types_rebuilt" &&
    grep -q '^MODE_PARAM_TEST_MODE = 0x42$' "$types_asm" &&
    grep -q '^MODE_PARAM_NO_MODE = 0xff$' "$types_asm" &&
    grep -q '^        INLINE_BYTE 0x42 ; for JSR Helper mode_param=test_mode$' "$types_asm"; then
    printf 'PASS types.ini\n'
else
    printf 'FAIL types.ini\n' >&2
    if ! cmp -s "$system_banked_inline_rom" "$types_rebuilt"; then
        report_mismatch "$system_banked_inline_rom" "$types_rebuilt"
    fi
    exit 1
fi

cross_bank_inline_rom="$OUT/cross_bank_inline.rom"
cross_bank_inline_asm="$OUT/cross_bank_inline.disasm"
cross_bank_inline_rebuilt="$OUT/cross_bank_inline.rebuilt"
"$ROOT/build/apexasm" "$cross_bank_inline_rom" "$ROOT/tests/cross_bank_inline.asm"
"$ROOT/build/apexdis" "$cross_bank_inline_rom" "$cross_bank_inline_asm" \
    "$ROOT/tests/cross_bank_inline.ini"
"$ROOT/build/apexasm" "$cross_bank_inline_rebuilt" "$cross_bank_inline_asm"
if cmp -s "$cross_bank_inline_rom" "$cross_bank_inline_rebuilt" &&
    grep -q '^    JSR SysHelper$' "$cross_bank_inline_asm" &&
    grep -q '^        INLINE_BYTE 0x37 ; for JSR SysHelper$' "$cross_bank_inline_asm" &&
    grep -q '^        INLINE_BYTE 0x99 ; for JSR SysHelper$' "$cross_bank_inline_asm"; then
    printf 'PASS cross_bank_inline.asm\n'
else
    printf 'FAIL cross_bank_inline.asm\n' >&2
    if ! cmp -s "$cross_bank_inline_rom" "$cross_bank_inline_rebuilt"; then
        report_mismatch "$cross_bank_inline_rom" "$cross_bank_inline_rebuilt"
    fi
    exit 1
fi

local_reanalysis_far_rom="$OUT/local_reanalysis_far.rom"
"$ROOT/build/apexasm" "$local_reanalysis_far_rom" "$ROOT/tests/local_reanalysis_far.asm"

if "$ROOT/build/project_api_test" "$system_banked_inline_rom" \
    "$ROOT/tests/system_banked_inline.ini" \
    "$banked_inline_rom" "$ROOT/tests/banked_inline.ini" \
    "$local_reanalysis_far_rom" "$ROOT/tests/local_reanalysis_far.ini"; then
    printf 'PASS project_api_test\n'
else
    printf 'FAIL project_api_test\n' >&2
    exit 1
fi

far_tables_rom="$OUT/far_tables.rom"
far_tables_asm="$OUT/far_tables.disasm"
far_tables_rebuilt="$OUT/far_tables.rebuilt"
"$ROOT/build/apexasm" "$far_tables_rom" "$ROOT/tests/far_tables.asm"
"$ROOT/build/apexdis" --xref "$far_tables_rom" "$far_tables_asm" "$ROOT/tests/far_tables.ini"
"$ROOT/build/apexasm" "$far_tables_rebuilt" "$far_tables_asm"
if cmp -s "$far_tables_rom" "$far_tables_rebuilt" &&
    grep -q '^; table rows=1 row_width=3 row_format=far_string' "$far_tables_asm" &&
    grep -q '^; table rows=1 row_width=6 row_format=byte, word, far_data' "$far_tables_asm" &&
    grep -q '^    TABLE_FAR_STRING B21_A4001_STRING_HI, 0x01' "$far_tables_asm" &&
    grep -q '^    .DB 0x55' "$far_tables_asm" &&
    grep -q '^    .DB 0x55 ; 0x4007 |U|' "$far_tables_asm" &&
    grep -q '^    .DW 0x1234' "$far_tables_asm" &&
    grep -q '^    TABLE_FAR_PTR B21_A4004, 0x01' "$far_tables_asm" &&
    grep -q '^; data type=far_code' "$far_tables_asm" &&
    grep -q '^    FAR_CODE B21_A4006, 0x01' "$far_tables_asm" &&
    grep -q '^B21_A4001_STRING_HI:' "$far_tables_asm" &&
    grep -q '^    STRING "HI"' "$far_tables_asm" &&
    grep -q '^    .DB 0xaa, 0xbb ; 0x4004 |..|' "$far_tables_asm" &&
    grep -q '^    TABLE_PTR Bff_A8012' "$far_tables_asm" &&
    grep -q '^Bff_A8012:' "$far_tables_asm" &&
    grep -q '^    .DB 0x41, 0x00 ; 0x8012 |A.|' "$far_tables_asm" &&
    grep -q '^; table_to_data bank=0x20 cpu=0x400d rom=0x00000d' "$far_tables_asm" &&
    grep -q '^; unclassified_to_code bank=0x21 cpu=0x4006 rom=0x004006' "$far_tables_asm" &&
    grep -q '^ENTRY_SWI2:' "$far_tables_asm" &&
    grep -q '^; referenced_by table:B20_A4007 line:0\[B20_A4007\], code:B21_A4006' "$far_tables_asm" &&
    grep -q '^; referenced_by data:B20_A400d' "$far_tables_asm" &&
    grep -q '^    LDX \[B21_A4004\]' "$far_tables_asm" &&
    grep -q '^    JSR Bff_A800b' "$far_tables_asm" &&
    grep -q '^    JSR InlineParam' "$far_tables_asm" &&
    grep -q '^        INLINE_BYTE 0x7a ; for JSR InlineParam$' "$far_tables_asm" &&
    grep -q '^; doc Inline param doc with ; semicolon, # hash, and \\ slash' "$far_tables_asm" &&
    grep -q '^    JSR InlineComplex' "$far_tables_asm" &&
    grep -q '^        INLINE_PTR B21_A4004 ; for JSR InlineComplex' "$far_tables_asm" &&
    grep -q '^        INLINE_FAR_PTR B21_A4004, 0x01 ; for JSR InlineComplex' "$far_tables_asm" &&
    grep -q '^        INLINE_CODE_PTR B21_A4006 ; for JSR InlineComplex' "$far_tables_asm" &&
    grep -q '^        INLINE_WORD 0x1234 ; for JSR InlineComplex' "$far_tables_asm" &&
    grep -q '^; inline params=ptr16_data, far_data, ptr16_code, word, byte\[2\]' "$far_tables_asm" &&
    grep -q '^Bff_A800b:' "$far_tables_asm" &&
    grep -q '^; referenced_by code:ENTRY_RESET' "$far_tables_asm" &&
    grep -q '^; referenced_by code:B21_A4006' "$far_tables_asm" &&
    grep -q '^; XREF INDEX' "$far_tables_asm" &&
    grep -q '^; XREF B21_A4004 bank=0x21 cpu=0x4004' "$far_tables_asm" &&
    grep -q '^;   table:B20_A4007' "$far_tables_asm"; then
    printf 'PASS far_tables.asm\n'
else
    printf 'FAIL far_tables.asm\n' >&2
    if ! cmp -s "$far_tables_rom" "$far_tables_rebuilt"; then
        report_mismatch "$far_tables_rom" "$far_tables_rebuilt"
    fi
    exit 1
fi

opcode_expected="$OUT/opcode_expected.txt"
opcode_actual="$OUT/opcode_actual.txt"
opcode_missing="$OUT/opcode_missing.txt"
opcode_extra="$OUT/opcode_extra.txt"
awk '
function hex(s, i, c, n, v) {
    v = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        n = index("0123456789abcdef", c) - 1
        if (n < 0) {
            n = index("0123456789ABCDEF", c) - 1
        }
        v = v * 16 + n
    }
    return v
}
function add(p, o) { printf "%02x:%02x\n", p, o }
function range(p, first, last, i) { for (i = first; i <= last; i++) add(p, i) }
BEGIN {
    split("00 03 04 06 07 08 09 0a 0c 0d 0e 0f 12 13 16 17 19 1a 1c 1d 1e 1f 39 3a 3b 3c 3d 3f", a)
    for (i in a) add(0, hex(a[i]))
    range(0, 0x20, 0x37)
    split("40 43 44 46 47 48 49 4a 4c 4d 4f 50 53 54 56 57 58 59 5a 5c 5d 5f", b)
    for (i in b) add(0, hex(b[i]))
    split("60 63 64 66 67 68 69 6a 6c 6d 6e 6f 70 73 74 76 77 78 79 7a 7c 7d 7e 7f", c)
    for (i in c) add(0, hex(c[i]))
    for (i = 0x80; i <= 0x8e; i++) if (i != 0x87) add(0, i)
    range(0, 0x90, 0xbf)
    for (i = 0xc0; i <= 0xce; i++) if (i != 0xc7 && i != 0xcd) add(0, i)
    range(0, 0xd0, 0xff)
    range(0x10, 0x20, 0x2f)
    split("3f 83 8c 8e 93 9c 9e 9f a3 ac ae af b3 bc be bf ce de df ee ef fe ff", d)
    for (i in d) add(0x10, hex(d[i]))
    split("3f 83 8c 93 9c a3 ac b3 bc", e)
    for (i in e) add(0x11, hex(e[i]))
}
' | sort -u > "$opcode_expected"
sed -n 's/.*OPER_[A-Z0-9_]*, 0x\([0-9A-Fa-f][0-9A-Fa-f]\), 0x\([0-9A-Fa-f][0-9A-Fa-f]\).*/\1:\2/p' "$ROOT/src/cpu6809.c" |
    tr 'A-F' 'a-f' | sort -u > "$opcode_actual"
comm -23 "$opcode_expected" "$opcode_actual" > "$opcode_missing"
comm -13 "$opcode_expected" "$opcode_actual" > "$opcode_extra"
if [ ! -s "$opcode_missing" ] && [ ! -s "$opcode_extra" ]; then
    printf 'PASS 6809_opcode_coverage\n'
else
    printf 'FAIL 6809_opcode_coverage\n' >&2
    if [ -s "$opcode_missing" ]; then
        printf 'missing:\n' >&2
        cat "$opcode_missing" >&2
    fi
    if [ -s "$opcode_extra" ]; then
        printf 'extra:\n' >&2
        cat "$opcode_extra" >&2
    fi
    exit 1
fi

config_asm="$OUT/addam_h4.config.asm"
config_rebuilt="$OUT/addam_h4.config.rebuilt"
"$ROOT/build/apexdis" "$ROOT/roms/addam_h4.rom" "$config_asm" "$ROOT/tests/addam_inline.ini"
"$ROOT/build/apexasm" "$config_rebuilt" "$config_asm"
if cmp -s "$ROOT/roms/addam_h4.rom" "$config_rebuilt" &&
    grep -q 'FarCall:' "$config_asm" &&
    grep -q 'JSR FarCall' "$config_asm" &&
    grep -q '^Panic:' "$config_asm" &&
    grep -q 'JSR Panic' "$config_asm" &&
    grep -q 'INLINE_BYTE 0x04 ; for JSR Panic' "$config_asm" &&
    grep -q 'JSR Unknown_take_2_bytes_then_far_code_8c97' "$config_asm" &&
    grep -q 'INLINE_FAR_CODE .* ; for JSR Unknown_take_2_bytes_then_far_code_8c97' "$config_asm" &&
    grep -q '^B3b_A4001:' "$config_asm" &&
    grep -q '^    .DW 0x00ab' "$config_asm" &&
    grep -q '^    .DB 0x02' "$config_asm" &&
    grep -q 'TABLE_PTR B3b_A415a' "$config_asm" &&
    grep -q '^B3c_A4001:' "$config_asm" &&
    grep -q '^    .DW 0x01e8' "$config_asm" &&
    grep -q 'TABLE_PTR B3c_A43d4' "$config_asm" &&
    grep -q 'TABLE_PTR B3c_A43d6' "$config_asm" &&
    grep -q '^Bff_A8001:' "$config_asm" &&
    grep -q '^; label bank=0xff cpu=0x8001 rom=0x078001' "$config_asm" &&
    grep -q '^_ROM_BANK_SHADOW = 0x0011' "$config_asm" &&
    grep -q '^_ASIC_ROM_PAGE = 0x3ffc' "$config_asm" &&
    grep -q '^DMD_FRAMEBUFFER_3800 = 0x3800' "$config_asm" &&
    grep -q 'CMPA <_ROM_BANK_SHADOW' "$config_asm" &&
    grep -q 'STA _ASIC_ROM_PAGE' "$config_asm" &&
    grep -q 'LDX #DMD_FRAMEBUFFER_3800' "$config_asm" &&
    grep -q 'JSR \[__SPRINGBOARD\]' "$config_asm" &&
    grep -q '^; kind table' "$config_asm" &&
    grep -q '^; table rows=116 row_width=3 row_format=far_code' "$config_asm" &&
    grep -q '^; doc Headerless dispatcher table containing far-code routine entry pointers\.' "$config_asm" &&
    grep -q '^; code_to_unclassified bank=' "$config_asm" &&
    grep -q '^    TABLE_FAR_CODE Bff_Aedbe' "$config_asm" &&
    grep -q '^    TABLE_FAR_CODE B3d_A7be5' "$config_asm" &&
    grep -q '^Bff_Aedbe:' "$config_asm" &&
    grep -q '^B3d_A7be5:' "$config_asm" &&
    grep -q '^; inline params=far_code' "$config_asm" &&
    grep -q '^; doc WPC far-call helper\.' "$config_asm" &&
    grep -q '^; doc Consumes a far-code pointer from the instruction stream\.' "$config_asm" &&
    grep -q '^; table rows=488 row_width=2 row_format=ptr16_string' "$config_asm" &&
    grep -q '^; doc Classic WPC counted string pointer table\.' "$config_asm" &&
    grep -q '^        INLINE_BYTE 0x78 ; for JSR Unknown_8c4b' "$config_asm" &&
    grep -q '^        INLINE_FAR_CODE B3d_A784b ; for JSR Unknown_8c4b' "$config_asm" &&
    awk '
        /^Bff_Aedc7:/ { in_block = 1 }
        /^Bff_Aedf7:/ { in_block = 0 }
        in_block && /^; code_to_unclassified/ { found = 1 }
        END { exit found ? 1 : 0 }
    ' "$config_asm" &&
    grep -q 'TABLE_PTR B3b_A415c_STRING_INSTALLED' "$config_asm" &&
    grep -q '^B3b_A415c_STRING_INSTALLED:' "$config_asm" &&
    grep -q '^    STRING "INSTALLED"' "$config_asm" &&
    grep -q '^    STRING ' "$config_asm" &&
    grep -q '^    STRING "PRESS #20#\\"ENTER\\"#0# TO"' "$config_asm" &&
    grep -q '^B3b_A5588:' "$config_asm" &&
    grep -q '^ThingAwardFarEntry:' "$config_asm" &&
    grep -q 'INLINE_FAR_CODE ThingAwardFarEntry ; for JSR FarCall' "$config_asm" &&
    grep -q 'INLINE_FAR_CODE .*0x18 ; for JSR FarCall' "$config_asm" &&
    ! grep -q 'JSR 0x8c97' "$config_asm" &&
    ! grep -q 'JSR 0x8990' "$config_asm"; then
    printf 'PASS addam_inline.ini\n'
else
    printf 'FAIL addam_inline.ini\n' >&2
    if ! cmp -s "$ROOT/roms/addam_h4.rom" "$config_rebuilt"; then
        report_mismatch "$ROOT/roms/addam_h4.rom" "$config_rebuilt"
    fi
    exit 1
fi

mkdir -p "$OUT"
if "$ROOT/build/apexdmd_test"; then
    printf 'PASS apexdmd_test\n'
else
    printf 'FAIL apexdmd_test\n' >&2
    exit 1
fi

dmd_fullframe_rom="$OUT/dmd_fullframe.rom"
dmd_fullframe_asm="$OUT/dmd_fullframe.disasm"
dmd_fullframe_rebuilt="$OUT/dmd_fullframe.rebuilt"
"$ROOT/build/apexasm" "$dmd_fullframe_rom" "$ROOT/tests/dmd_fullframe.asm"
"$ROOT/build/apexdis" "$dmd_fullframe_rom" "$dmd_fullframe_asm" \
    "$ROOT/tests/dmd_fullframe.ini"
"$ROOT/build/apexasm" "$dmd_fullframe_rebuilt" "$dmd_fullframe_asm"
if cmp -s "$dmd_fullframe_rom" "$dmd_fullframe_rebuilt" &&
    grep -q '^    TABLE_FAR_DMD_FULLFRAME DmdAsset ; dmd type=fullframe decoder=0x01 consumed=8 width=128 height=32$' \
        "$dmd_fullframe_asm" &&
    grep -q '^    TABLE_PTR_DMD_FULLFRAME DmdAsset ; dmd type=fullframe decoder=0x01 consumed=8 width=128 height=32$' \
        "$dmd_fullframe_asm" &&
    grep -q '^    FAR_DMD_FULLFRAME DmdAsset ; dmd type=fullframe decoder=0x01 consumed=8 width=128 height=32$' \
        "$dmd_fullframe_asm" &&
    grep -q '^; data type=dmd_fullframe$' "$dmd_fullframe_asm" &&
    grep -q '^ ; dmd type=fullframe decoder=0x01 consumed=8 width=128 height=32$' "$dmd_fullframe_asm"; then
    printf 'PASS dmd_fullframe.asm\n'
else
    printf 'FAIL dmd_fullframe.asm\n' >&2
    if ! cmp -s "$dmd_fullframe_rom" "$dmd_fullframe_rebuilt"; then
        report_mismatch "$dmd_fullframe_rom" "$dmd_fullframe_rebuilt"
    fi
    exit 1
fi

apexdmd_table_rom="$OUT/apexdmd_table.rom"
apexdmd_table_ini="$OUT/apexdmd_table.ini"
apexdmd_table_out="$OUT/apexdmd_table_out"
apexdmd_table_summary="$apexdmd_table_out/summary.tsv"
dd if=/dev/zero of="$apexdmd_table_rom" bs=49152 count=1 status=none
printf '\001' | dd of="$apexdmd_table_rom" bs=1 seek=0 conv=notrunc status=none
printf '\001\252\252\000\000\252\000\000' | \
    dd of="$apexdmd_table_rom" bs=1 seek=1 conv=notrunc status=none
printf '\002\252\252\000\377\252\000\377' | \
    dd of="$apexdmd_table_rom" bs=1 seek=514 conv=notrunc status=none
printf '\100\001\001\102\002\001' | \
    dd of="$apexdmd_table_rom" bs=1 seek=16384 conv=notrunc status=none
cat >"$apexdmd_table_ini" <<'EOF'
[tables]
Bff_A8000 = rows[2](far_data)
EOF
rm -rf "$apexdmd_table_out"
apexdmd_table_line0=$(printf '0\tB01_A4001\t0x01\t8\trow000_B01_A4001.pbm\tpair000_001_B01_A4001_B01_A4202.pgm')
apexdmd_table_line1=$(printf '1\tB01_A4202\t0x02\t8\trow001_B01_A4202.pbm\t-')
apexdmd_table_stat1=$(printf '# adjacent_same_bank\t1')
apexdmd_table_stat2=$(printf '# adjacent_same_type\t0')
if "$ROOT/build/apexdmd" --table "$apexdmd_table_rom" "$apexdmd_table_ini" Bff_A8000 \
    "$apexdmd_table_out" &&
    [ -f "$apexdmd_table_out/row000_B01_A4001.pbm" ] &&
    [ -f "$apexdmd_table_out/row001_B01_A4202.pbm" ] &&
    [ -f "$apexdmd_table_out/pair000_001_B01_A4001_B01_A4202.pgm" ] &&
    grep -Fqx "$apexdmd_table_line0" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_line1" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_stat1" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_stat2" "$apexdmd_table_summary"; then
    printf 'PASS apexdmd_table\n'
else
    printf 'FAIL apexdmd_table\n' >&2
    exit 1
fi
