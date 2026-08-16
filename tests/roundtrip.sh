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
    grep -qF '    STRING_FIXED "WOR\tD"' "$string_fixed_asm"; then
    printf 'PASS string_fixed.asm\n'
else
    printf 'FAIL string_fixed.asm\n' >&2
    exit 1
fi

# A string may contain control bytes 0x0a (newline), 0x09 (tab) and 0x07 (BELL):
# they must be emitted with \n / \t / \a escapes and survive a full
# assemble→disassemble→assemble byte roundtrip.
string_nl_rom="$OUT/string_newline.rom"
string_nl_asm="$OUT/string_newline.disasm"
string_nl_rom2="$OUT/string_newline.rebuilt"
"$ROOT/build/apexasm" "$string_nl_rom" "$ROOT/tests/string_newline.asm"
"$ROOT/build/apexdis" "$string_nl_rom" "$string_nl_asm" "$ROOT/tests/string_newline.ini"
"$ROOT/build/apexasm" "$string_nl_rom2" "$string_nl_asm"
if grep -qF '    STRING "\nA\tB\aC"' "$string_nl_asm" &&
    cmp -s "$string_nl_rom" "$string_nl_rom2"; then
    printf 'PASS string_newline.asm\n'
else
    printf 'FAIL string_newline.asm\n' >&2
    exit 1
fi

# A classified string[N] (STRING_FIXED) range may contain carriage returns
# (0x0d): they must emit with the \r escape rather than making the whole range
# fall back to raw .DB, and must survive a full assemble→disassemble→assemble
# byte roundtrip.  Regression for CR/LF-heavy strings rendering as .DB.
string_cr_rom="$OUT/string_cr.rom"
string_cr_asm="$OUT/string_cr.disasm"
string_cr_rom2="$OUT/string_cr.rebuilt"
"$ROOT/build/apexasm" "$string_cr_rom" "$ROOT/tests/string_cr.asm"
"$ROOT/build/apexdis" "$string_cr_rom" "$string_cr_asm" "$ROOT/tests/string_cr.ini"
"$ROOT/build/apexasm" "$string_cr_rom2" "$string_cr_asm"
if grep -qF '    STRING_FIXED "\r\nA\r\nB"' "$string_cr_asm" &&
    cmp -s "$string_cr_rom" "$string_cr_rom2"; then
    printf 'PASS string_cr.asm\n'
else
    printf 'FAIL string_cr.asm\n' >&2
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

# apexini check must also catch the static [entries]<->[data] conflict.
if "$ROOT/build/apexini" check "$ROOT/tests/config_class_conflict.ini" \
    2>"$OUT/apexini_check_conflict.stderr"; then
    printf 'FAIL apexini_check_conflict\n' >&2
    exit 1
elif grep -q "config classifies 0x8002 as both code entry and data" \
    "$OUT/apexini_check_conflict.stderr"; then
    printf 'PASS apexini_check_conflict\n'
else
    printf 'FAIL apexini_check_conflict\n' >&2
    exit 1
fi

# apexini merge conflict handling and --override.
printf '[symbols]\n_SHADOW = 0x0011\n' > "$OUT/merge_a.ini"
printf '[symbols]\n_SHADOW = 0x0013\n' > "$OUT/merge_b.ini"
if "$ROOT/build/apexini" merge "$OUT/merge_out.ini" "$OUT/merge_a.ini" \
    "$OUT/merge_b.ini" 2>/dev/null; then
    printf 'FAIL apexini_merge_conflict\n' >&2
    exit 1
else
    printf 'PASS apexini_merge_conflict\n'
fi
if "$ROOT/build/apexini" merge --override "$OUT/merge_out.ini" "$OUT/merge_a.ini" \
    "$OUT/merge_b.ini" >/dev/null 2>&1 &&
    grep -q '_SHADOW = 0x0013' "$OUT/merge_out.ini"; then
    printf 'PASS apexini_merge_override\n'
else
    printf 'FAIL apexini_merge_override\n' >&2
    exit 1
fi

# apexini coverage --bank restricts output to one bank plus totals.
if "$ROOT/build/apexini" coverage "$data_range_rom" "$ROOT/tests/data_ranges.ini" \
    --bank 0xff >"$OUT/coverage_bank.out" 2>/dev/null &&
    grep -q '^0xff' "$OUT/coverage_bank.out" &&
    ! grep -q '^total' "$OUT/coverage_bank.out"; then
    printf 'PASS apexini_coverage_bank\n'
else
    printf 'FAIL apexini_coverage_bank\n' >&2
    exit 1
fi

# Opt-in render switches: default config and options-enabled config must BOTH
# round-trip byte-identically, and the options must have the documented effect.
opt_rom="$OUT/opt_features.rom"
"$ROOT/build/apexasm" "$opt_rom" "$ROOT/tests/opt_features.asm"
"$ROOT/build/apexdis" "$opt_rom" "$OUT/opt_default.asm" "$ROOT/tests/opt_features.ini" \
    2>/dev/null
"$ROOT/build/apexdis" "$opt_rom" "$OUT/opt_on.asm" "$ROOT/tests/opt_features_on.ini" \
    2>/dev/null
"$ROOT/build/apexasm" "$OUT/opt_default_rt.rom" "$OUT/opt_default.asm" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/opt_on_rt.rom" "$OUT/opt_on.asm" 2>/dev/null
if cmp -s "$opt_rom" "$OUT/opt_default_rt.rom" &&
    cmp -s "$opt_rom" "$OUT/opt_on_rt.rom"; then
    printf 'PASS opt_features_roundtrip\n'
else
    printf 'FAIL opt_features_roundtrip\n' >&2
    exit 1
fi
# Default resolves the small symbol in the immediate; the option leaves it raw
# while the address operand still resolves.
if grep -q 'LDX #TEXT_FIELD' "$OUT/opt_default.asm" &&
    grep -q 'LDX #0x0014' "$OUT/opt_on.asm" &&
    grep -q 'LDA <TEXT_FIELD' "$OUT/opt_on.asm"; then
    printf 'PASS opt_min_immediate_symbol\n'
else
    printf 'FAIL opt_min_immediate_symbol\n' >&2
    exit 1
fi
# 5-bit index offset: decimal by default, minimal hex with the option.
if grep -q 'LDD 15,U' "$OUT/opt_default.asm" &&
    grep -q 'LDD 0xf,U' "$OUT/opt_on.asm"; then
    printf 'PASS opt_hex_index_offsets\n'
else
    printf 'FAIL opt_hex_index_offsets\n' >&2
    exit 1
fi
# reference_counts and instruction_addresses annotations appear only with the option.
if ! grep -q 'referenced_by (' "$OUT/opt_default.asm" &&
    grep -q 'referenced_by (' "$OUT/opt_on.asm" &&
    ! grep -q '^    ; addr B' "$OUT/opt_default.asm" &&
    grep -q '^    ; addr B' "$OUT/opt_on.asm"; then
    printf 'PASS opt_reference_counts_and_addr\n'
else
    printf 'FAIL opt_reference_counts_and_addr\n' >&2
    exit 1
fi

# far_code_allow_null: default warns on a 0x0000 inline far-code pointer; the
# option suppresses the warning.  Both configs round-trip byte-identically.
fcn_rom="$OUT/far_code_null.rom"
"$ROOT/build/apexasm" "$fcn_rom" "$ROOT/tests/far_code_null.asm"
"$ROOT/build/apexdis" "$fcn_rom" "$OUT/fcn_default.asm" "$ROOT/tests/far_code_null.ini" \
    2>/dev/null
"$ROOT/build/apexdis" "$fcn_rom" "$OUT/fcn_ok.asm" "$ROOT/tests/far_code_null_ok.ini" \
    2>/dev/null
"$ROOT/build/apexasm" "$OUT/fcn_default_rt.rom" "$OUT/fcn_default.asm" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/fcn_ok_rt.rom" "$OUT/fcn_ok.asm" 2>/dev/null
if grep -q 'WARNING inline_far_code_invalid' "$OUT/fcn_default.asm" &&
    ! grep -q 'WARNING inline_far_code_invalid' "$OUT/fcn_ok.asm" &&
    grep -q 'INLINE_FAR_CODE' "$OUT/fcn_ok.asm" &&
    cmp -s "$fcn_rom" "$OUT/fcn_default_rt.rom" &&
    cmp -s "$fcn_rom" "$OUT/fcn_ok_rt.rom"; then
    printf 'PASS far_code_allow_null\n'
else
    printf 'FAIL far_code_allow_null\n' >&2
    exit 1
fi

# Long generated string label: the capped name must be used identically at the
# definition and the operand reference (no operand-buffer truncation), so it
# round-trips.  Also assert the label stayed within the 63-char operand buffer.
lsl_rom="$OUT/long_string_label.rom"
"$ROOT/build/apexasm" "$lsl_rom" "$ROOT/tests/long_string_label.asm"
"$ROOT/build/apexdis" "$lsl_rom" "$OUT/lsl.asm" "$ROOT/tests/long_string_label.ini" \
    2>/dev/null
"$ROOT/build/apexasm" "$OUT/lsl_rt.rom" "$OUT/lsl.asm" 2>/dev/null
lsl_maxlen=$(grep -oE '\bB[0-9a-f]{2}_A[0-9a-f]{4}_STRING_[A-Z0-9_]*' "$OUT/lsl.asm" \
    | awk '{ if (length > m) m = length } END { print m + 0 }')
if cmp -s "$lsl_rom" "$OUT/lsl_rt.rom" && [ "$lsl_maxlen" -gt 0 ] && [ "$lsl_maxlen" -le 63 ]; then
    printf 'PASS long_string_label\n'
else
    printf 'FAIL long_string_label (maxlen=%s)\n' "$lsl_maxlen" >&2
    exit 1
fi

# report_code_in_data: a JSR hidden in a [data] range is flagged only with the
# option, is silent by default, and neither config changes the round-trip.
cid_rom="$OUT/code_in_data.rom"
"$ROOT/build/apexasm" "$cid_rom" "$ROOT/tests/code_in_data.asm"
"$ROOT/build/apexdis" "$cid_rom" "$OUT/cid_default.asm" "$ROOT/tests/code_in_data.ini" \
    2>/dev/null
"$ROOT/build/apexdis" "$cid_rom" "$OUT/cid_on.asm" "$ROOT/tests/code_in_data_on.ini" \
    2>/dev/null
"$ROOT/build/apexasm" "$OUT/cid_default_rt.rom" "$OUT/cid_default.asm" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/cid_on_rt.rom" "$OUT/cid_on.asm" 2>/dev/null
if ! grep -q 'code_in_data bank' "$OUT/cid_default.asm" &&
    grep -q 'WARNING code_in_data bank=0xff cpu=0x8001 .* target=Bff_A8010 name=ROUTINE' \
        "$OUT/cid_on.asm" &&
    cmp -s "$cid_rom" "$OUT/cid_default_rt.rom" &&
    cmp -s "$cid_rom" "$OUT/cid_on_rt.rom"; then
    printf 'PASS report_code_in_data\n'
else
    printf 'FAIL report_code_in_data\n' >&2
    exit 1
fi

# check_inline_length: no warning when the [inline] length matches the routine's
# stack fixup, a warning when it disagrees.  The correct config round-trips.
il_rom="$OUT/inline_length.rom"
"$ROOT/build/apexasm" "$il_rom" "$ROOT/tests/inline_length.asm"
"$ROOT/build/apexdis" "$il_rom" "$OUT/il_ok.asm" "$ROOT/tests/inline_length_ok.ini" 2>/dev/null
"$ROOT/build/apexdis" "$il_rom" "$OUT/il_bad.asm" "$ROOT/tests/inline_length_bad.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/il_ok_rt.rom" "$OUT/il_ok.asm" 2>/dev/null
"$ROOT/build/apexdis" "$il_rom" "$OUT/il_ack.asm" "$ROOT/tests/inline_length_ack.ini" 2>/dev/null
if ! grep -q 'inline_length_mismatch' "$OUT/il_ok.asm" &&
    grep -q 'WARNING inline_length_mismatch bank=0xff cpu=0x8009 configured=3 stack_adjust=5' \
        "$OUT/il_bad.asm" &&
    cmp -s "$il_rom" "$OUT/il_ok_rt.rom" &&
    grep -q 'WARNING_ACK inline_length_mismatch bank=0xff cpu=0x8009' "$OUT/il_ack.asm" &&
    ! grep -q '; WARNING inline_length_mismatch' "$OUT/il_ack.asm"; then
    printf 'PASS check_inline_length\n'
else
    printf 'FAIL check_inline_length\n' >&2
    exit 1
fi

# Variable-length inline payloads (bytes_until / counted_bytes): the disassembler
# resolves each length from the ROM, emits the new pseudo-ops, and re-assembles
# byte-for-byte.  `apexini normalize` must preserve the field kinds.
iv_rom="$OUT/inline_variable.rom"
"$ROOT/build/apexasm" "$iv_rom" "$ROOT/tests/inline_variable.asm"
"$ROOT/build/apexdis" "$iv_rom" "$OUT/iv.asm" "$ROOT/tests/inline_variable.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/iv_rt.rom" "$OUT/iv.asm" 2>/dev/null
"$ROOT/build/apexini" normalize "$ROOT/tests/inline_variable.ini" "$OUT/iv_norm.ini" 2>/dev/null
if grep -q 'INLINE_BYTES_UNTIL 0x05, 0xf0, 0x03, 0x00' "$OUT/iv.asm" &&
    grep -q 'INLINE_COUNTED_BYTES 0x03, 0xaa, 0xbb, 0xcc' "$OUT/iv.asm" &&
    cmp -s "$iv_rom" "$OUT/iv_rt.rom" &&
    grep -q 'bytes_until(0x00)' "$OUT/iv_norm.ini" &&
    grep -q 'counted_bytes' "$OUT/iv_norm.ini"; then
    printf 'PASS inline_variable\n'
else
    printf 'FAIL inline_variable\n' >&2
    exit 1
fi

# A [labels] entry inside a bytes[N] data range must be emitted as a definition
# (the raw-bytes emitter breaks the row there) so a reference to it re-assembles.
lid_rom="$OUT/label_in_data.rom"
"$ROOT/build/apexasm" "$lid_rom" "$ROOT/tests/label_in_data.asm"
"$ROOT/build/apexdis" "$lid_rom" "$OUT/lid.asm" "$ROOT/tests/label_in_data.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/lid_rt.rom" "$OUT/lid.asm" 2>/dev/null
if grep -q '^MyBlock:' "$OUT/lid.asm" &&
    grep -q 'LDX #MyBlock' "$OUT/lid.asm" &&
    cmp -s "$lid_rom" "$OUT/lid_rt.rom"; then
    printf 'PASS label_in_data\n'
else
    printf 'FAIL label_in_data\n' >&2
    exit 1
fi

# Symbol block lengths: an address inside a length>1 [symbols] block resolves to
# NAME+offset (exact hit = bare name); it round-trips (apexasm evaluates SYM+n)
# and normalize preserves the length.  Immediates obey min_immediate_symbol.
sb_rom="$OUT/symbol_block.rom"
"$ROOT/build/apexasm" "$sb_rom" "$ROOT/tests/symbol_block.asm"
"$ROOT/build/apexdis" "$sb_rom" "$OUT/sb.asm" "$ROOT/tests/symbol_block.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/sb_rt.rom" "$OUT/sb.asm" 2>/dev/null
"$ROOT/build/apexini" normalize "$ROOT/tests/symbol_block.ini" "$OUT/sb_norm.ini" 2>/dev/null
printf '[options]\nmin_immediate_symbol = 0x200\n\n[symbols]\nScore_P1 = 0x0150, 4\nBuf = 0x0200, 0x10\n\n[entries]\n0x8000 = code\n' \
    > "$OUT/sb_minimm.ini"
"$ROOT/build/apexdis" "$sb_rom" "$OUT/sb_mi.asm" "$OUT/sb_minimm.ini" 2>/dev/null
if grep -q 'LDB Score_P1+2' "$OUT/sb.asm" &&
    grep -q 'STA Score_P1+3' "$OUT/sb.asm" &&
    grep -q 'LDX #Score_P1' "$OUT/sb.asm" &&
    grep -q 'LDU Buf+5' "$OUT/sb.asm" &&
    cmp -s "$sb_rom" "$OUT/sb_rt.rom" &&
    grep -q 'Score_P1 = 0x0150, 4' "$OUT/sb_norm.ini" &&
    grep -q 'LDB Score_P1+2' "$OUT/sb_mi.asm" &&
    grep -q 'LDX #0x0150' "$OUT/sb_mi.asm"; then
    printf 'PASS symbol_block\n'
else
    printf 'FAIL symbol_block\n' >&2
    exit 1
fi

# Multi-plane DMD frames: dmd_fullframe[N] consumes N planes; the config survives
# normalize; and report_dmd_short flags a frame classified with too few planes.
dmdmp_rom="$OUT/dmd_multiplane.rom"
"$ROOT/build/apexasm" "$dmdmp_rom" "$ROOT/tests/dmd_multiplane.asm"
"$ROOT/build/apexdis" "$dmdmp_rom" "$OUT/dmdmp.asm" "$ROOT/tests/dmd_multiplane.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/dmdmp_rt.rom" "$OUT/dmdmp.asm" 2>/dev/null
"$ROOT/build/apexini" normalize "$ROOT/tests/dmd_multiplane.ini" "$OUT/dmdmp_norm.ini" 2>/dev/null
"$ROOT/build/apexdis" "$dmdmp_rom" "$OUT/dmdmp_short.asm" "$ROOT/tests/dmd_multiplane_short.ini" \
    2>/dev/null
if grep -q 'planes=2 decoder=0x01 consumed=16' "$OUT/dmdmp.asm" &&
    cmp -s "$dmdmp_rom" "$OUT/dmdmp_rt.rom" &&
    grep -q 'dmd_fullframe\[2\]' "$OUT/dmdmp_norm.ini" &&
    grep -q 'WARNING dmd_decode_short bank=0xff cpu=0x8000 planes=1 decoded=8 suggest_planes=2 full=16' \
        "$OUT/dmdmp_short.asm"; then
    printf 'PASS dmd_multiplane\n'
else
    printf 'FAIL dmd_multiplane\n' >&2
    exit 1
fi

# Regression: the ANALYSIS pass must consume a variable payload whole — a payload
# byte pair that looks like an address (0x80 0x30) must NOT plant a spurious
# label/ref at 0x8030 (the Dr. Who B31_A64f4 failure).
ivr_rom="$OUT/inline_var_ref.rom"
"$ROOT/build/apexasm" "$ivr_rom" "$ROOT/tests/inline_var_ref.asm"
"$ROOT/build/apexdis" "$ivr_rom" "$OUT/ivr.asm" "$ROOT/tests/inline_var_ref.ini" 2>/dev/null
"$ROOT/build/apexasm" "$OUT/ivr_rt.rom" "$OUT/ivr.asm" 2>/dev/null
if grep -q 'INLINE_BYTES_UNTIL 0x80, 0x30, 0x80, 0x30, 0x00' "$OUT/ivr.asm" &&
    ! grep -q '_A8030:' "$OUT/ivr.asm" &&
    cmp -s "$ivr_rom" "$OUT/ivr_rt.rom"; then
    printf 'PASS inline_variable_analysis_consume\n'
else
    printf 'FAIL inline_variable_analysis_consume\n' >&2
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

# Acking a warning ([ack_warnings]) turns it into "; WARNING_ACK ...", removes
# the active "; WARNING" and suppresses the stderr line; apexini preserves it.
ack_asm="$OUT/ack_warning.disasm"
ack_err="$OUT/ack_warning.stderr"
"$ROOT/build/apexdis" "$inline_invalid_rom" "$ack_asm" \
    "$ROOT/tests/ack_warning.ini" 2>"$ack_err"
if grep -q '^; WARNING_ACK inline_far_code_invalid bank=0x20 cpu=0x4004' "$ack_asm" &&
    ! grep -qE '^; WARNING ' "$ack_asm" &&
    ! grep -q '^warning:' "$ack_err" &&
    "$ROOT/build/apexini" check "$ROOT/tests/ack_warning.ini" | grep -q 'ack_warnings=1'; then
    printf 'PASS ack_warning.ini\n'
else
    printf 'FAIL ack_warning.ini\n' >&2
    exit 1
fi

# [far_imm]: a split far pointer (LDX #addr + LDB #bank) with the paired form
# "far_code 0x38 B20_A4004".  The address load resolves to B38_A5123 (seeded as
# code in bank 0x38) and the bank load renders #bank(B38_A5123); both reference
# the same label and reassemble byte-identically.
far_imm_rom="$OUT/far_imm.rom"
far_imm_asm="$OUT/far_imm.disasm"
far_imm_rebuilt="$OUT/far_imm.rebuilt"
"$ROOT/build/apexasm" "$far_imm_rom" "$ROOT/tests/far_imm.asm"
"$ROOT/build/apexdis" "$far_imm_rom" "$far_imm_asm" "$ROOT/tests/far_imm.ini"
"$ROOT/build/apexasm" "$far_imm_rebuilt" "$far_imm_asm"
if cmp -s "$far_imm_rom" "$far_imm_rebuilt" &&
    grep -q '^    LDX #B38_A5123' "$far_imm_asm" &&
    grep -q '^    LDB #bank(B38_A5123)' "$far_imm_asm" &&
    grep -q '^B38_A5123:' "$far_imm_asm" &&
    "$ROOT/build/apexini" check "$ROOT/tests/far_imm.ini" | grep -q 'far_imm=1'; then
    printf 'PASS far_imm.asm\n'
else
    printf 'FAIL far_imm.asm\n' >&2
    exit 1
fi

# System-bank absolute JSR into the paged window: bank-ambiguous in general, but
# auto-resolves when exactly one paged bank has content at the offset; otherwise
# it renders "; paged (bank-ambiguous)".  Comment must not affect the roundtrip.
spc_rom="$OUT/sys_paged_call.rom"
spc_asm="$OUT/sys_paged_call.disasm"
spc_rebuilt="$OUT/sys_paged_call.rebuilt"
"$ROOT/build/apexasm" "$spc_rom" "$ROOT/tests/sys_paged_call.asm"
"$ROOT/build/apexdis" "$spc_rom" "$spc_asm" "$ROOT/tests/sys_paged_call.ini"
"$ROOT/build/apexasm" "$spc_rebuilt" "$spc_asm"
if cmp -s "$spc_rom" "$spc_rebuilt" &&
    grep -q '^    JSR B20_A5000' "$spc_asm" &&
    grep -qE '^    JSR 0x6000 +; paged \(bank-ambiguous\)' "$spc_asm" &&
    grep -q '^    JSR Bff_A9000' "$spc_asm"; then
    printf 'PASS sys_paged_call.asm\n'
else
    printf 'FAIL sys_paged_call.asm\n' >&2
    exit 1
fi

# 256 KB ROM support (14 paged banks, bank-id base 0x30, ids 0x30..0x3d).
# Assemble -> disassemble (via the config/apex_project path) -> assemble, and
# verify labels resolve at the 0x30 base and the ROM round-trips byte-identical.
r256_rom="$OUT/rom256k.rom"
r256_asm="$OUT/rom256k.disasm"
r256_rebuilt="$OUT/rom256k.rebuilt"
"$ROOT/build/apexasm" "$r256_rom" "$ROOT/tests/rom256k.asm"
"$ROOT/build/apexdis" "$r256_rom" "$r256_asm" "$ROOT/tests/rom256k.ini"
"$ROOT/build/apexasm" "$r256_rebuilt" "$r256_asm"
if [ "$(wc -c < "$r256_rom")" -eq 262144 ] &&
    cmp -s "$r256_rom" "$r256_rebuilt" &&
    grep -q '^BankStart:' "$r256_asm" &&
    grep -q '^LastBank:' "$r256_asm" &&
    grep -q '^    JSR BankStart' "$r256_asm"; then
    printf 'PASS rom256k.asm\n'
else
    printf 'FAIL rom256k.asm\n' >&2
    exit 1
fi

# Label name that collides with the generated Bxx_Ayyyy form ("Bcd_Add16" =>
# B'cd'_A'dd16'): the assembler must resolve the DEFINED symbol (0xa651), not
# decode the pattern (0xdd16). Assemble -> disassemble -> assemble byte-identical.
lpc_rom="$OUT/label_pattern_collision.rom"
lpc_asm="$OUT/label_pattern_collision.disasm"
lpc_rebuilt="$OUT/label_pattern_collision.rebuilt"
lpc_err="$OUT/label_pattern_collision.stderr"
"$ROOT/build/apexasm" "$lpc_rom" "$ROOT/tests/label_pattern_collision.asm"
"$ROOT/build/apexdis" "$lpc_rom" "$lpc_asm" "$ROOT/tests/label_pattern_collision.ini" 2>"$lpc_err"
"$ROOT/build/apexasm" "$lpc_rebuilt" "$lpc_asm"
if cmp -s "$lpc_rom" "$lpc_rebuilt" &&
    grep -q '^    JSR Bcd_Add16' "$lpc_asm" &&
    grep -q '^Bcd_Add16:' "$lpc_asm" &&
    grep -q '^; WARNING label_name_collision .* name=Bcd_Add16 decodes=0xdd16' "$lpc_asm" &&
    grep -q "label 'Bcd_Add16'" "$lpc_err"; then
    printf 'PASS label_pattern_collision.asm\n'
else
    printf 'FAIL label_pattern_collision.asm\n' >&2
    exit 1
fi

# bcd[N] data type: a BCD range renders as "BCD <digits>" and reassembles.
bcd_rom="$OUT/bcd.rom"
bcd_asm="$OUT/bcd.disasm"
bcd_rebuilt="$OUT/bcd.rebuilt"
"$ROOT/build/apexasm" "$bcd_rom" "$ROOT/tests/bcd.asm"
"$ROOT/build/apexdis" "$bcd_rom" "$bcd_asm" "$ROOT/tests/bcd.ini"
"$ROOT/build/apexasm" "$bcd_rebuilt" "$bcd_asm"
if cmp -s "$bcd_rom" "$bcd_rebuilt" &&
    grep -q '^    BCD 0001234500$' "$bcd_asm"; then
    printf 'PASS bcd.asm\n'
else
    printf 'FAIL bcd.asm\n' >&2
    exit 1
fi

# nvram-maps JSON import/export: import a JSON RAM map into [symbols]/[docs],
# verify the ini parses, then export it back and confirm the locations survive.
nv_ini="$OUT/nvram_sample.ini"
nv_json="$OUT/nvram_sample.out.json"
nv_merged="$OUT/nvram_sample.merged.json"
"$ROOT/build/apexini" nvram-import "$ROOT/tests/nvram_sample.json" "$nv_ini" >/dev/null
"$ROOT/build/apexini" nvram-export "$nv_ini" "$nv_json" >/dev/null
# zero-loss merge: re-export against the original as template — the bcd encoding
# and the nested "high_scores" array must survive, with the apex name applied.
"$ROOT/build/apexini" nvram-export "$nv_ini" "$nv_merged" "$ROOT/tests/nvram_sample.json" >/dev/null
# in-place update: output == template must preserve fields, not truncate the file.
nv_inplace="$OUT/nvram_inplace.json"
cp "$ROOT/tests/nvram_sample.json" "$nv_inplace"
"$ROOT/build/apexini" nvram-export "$nv_ini" "$nv_inplace" "$nv_inplace" >/dev/null
if grep -q '^BallsPerGame = 0x1a00$' "$nv_ini" &&
    grep -q '^Free_Play = 0x1a01$' "$nv_ini" &&
    grep -q '^GC_Score = 0x1b10$' "$nv_ini" &&
    grep -q '^0x1a00 = "Balls Per Game"$' "$nv_ini" &&
    "$ROOT/build/apexini" check "$nv_ini" >/dev/null &&
    grep -q '"BallsPerGame"' "$nv_json" &&
    grep -q '"start": "0x1b10"' "$nv_json" &&
    grep -q '"encoding": "bcd"' "$nv_merged" &&
    grep -q '"high_scores"' "$nv_merged" &&
    grep -q '"short_label": "GC_Score"' "$nv_merged" &&
    grep -q '"encoding": "bcd"' "$nv_inplace" &&
    grep -q '"BallsPerGame"' "$nv_inplace"; then
    printf 'PASS nvram_maps.json\n'
else
    printf 'FAIL nvram_maps.json\n' >&2
    exit 1
fi

# nvram JSON hardening: deeply-nested input is rejected (not a stack-overflow
# crash), and non-integer numbers survive a zero-loss round-trip verbatim.
nv_deep="$OUT/nvram_deep.json"
python3 -c "open('$nv_deep','w').write('{\"_x\":' + '['*4000 + ']'*4000 + '}')"
deep_rc=0
"$ROOT/build/apexini" nvram-import "$nv_deep" "$OUT/nvram_deep.ini" >"$OUT/nvram_deep.err" 2>&1 || deep_rc=$?
nv_prec="$OUT/nvram_prec.json"
printf '{ "_metadata": { "version": 1.0 },\n  "a": { "01": { "start": "0x100", "label": "x", "scale": 0.123456789 } } }\n' > "$nv_prec"
"$ROOT/build/apexini" nvram-import "$nv_prec" "$OUT/nvram_prec.ini" >/dev/null
"$ROOT/build/apexini" nvram-export "$OUT/nvram_prec.ini" "$OUT/nvram_prec.out.json" "$nv_prec" >/dev/null
if [ "$deep_rc" -ne 139 ] &&
    grep -q 'nesting too deep' "$OUT/nvram_deep.err" &&
    grep -q '"scale": 0.123456789' "$OUT/nvram_prec.out.json"; then
    printf 'PASS nvram_hardening\n'
else
    printf 'FAIL nvram_hardening (deep_rc=%s)\n' "$deep_rc" >&2
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
    grep -q '^LEVEL_LOW = 0x0003$' "$types_asm" &&
    grep -q '^LEVEL_HIGH = 0x0103$' "$types_asm" &&
    grep -q '^        INLINE_BYTE 0x42 ; for JSR Helper mode_param=test_mode$' "$types_asm"; then
    printf 'PASS types.ini\n'
else
    printf 'FAIL types.ini\n' >&2
    if ! cmp -s "$system_banked_inline_rom" "$types_rebuilt"; then
        report_mismatch "$system_banked_inline_rom" "$types_rebuilt"
    fi
    exit 1
fi

# ---- Large [types] enum is not truncated (was capped at an 8 KB buffer) ----
big_types_ini="$OUT/big_types.ini"
big_types_asm="$OUT/big_types.disasm"
{
    printf '[types]\nbigt:word =\n'
    i=0
    while [ "$i" -lt 700 ]; do
        printf '\t0x%04x:val_%d\n' "$i" "$i"
        i=$((i + 1))
    done
} >"$big_types_ini"
"$ROOT/build/apexdis" "$system_banked_inline_rom" "$big_types_asm" "$big_types_ini"
if grep -q '^BIGT_VAL_0 = 0x0000$' "$big_types_asm" &&
    grep -q '^BIGT_VAL_699 = 0x02bb$' "$big_types_asm"; then
    printf 'PASS big_types.ini\n'
else
    printf 'FAIL big_types.ini\n' >&2
    exit 1
fi

# ---- inline payloads apply to calls, not branches ----
inline_branch_rom="$OUT/inline_branch.rom"
inline_branch_asm="$OUT/inline_branch.disasm"
inline_branch_rebuilt="$OUT/inline_branch.rebuilt"
"$ROOT/build/apexasm" "$inline_branch_rom" "$ROOT/tests/inline_branch.asm"
"$ROOT/build/apexdis" "$inline_branch_rom" "$inline_branch_asm" "$ROOT/tests/inline_branch.ini"
"$ROOT/build/apexasm" "$inline_branch_rebuilt" "$inline_branch_asm"
# JSR to Helper consumes the inline byte; the BCC to the same Helper must not,
# so the NOP after the branch decodes as code (no INLINE_BYTE after BCC).
if cmp -s "$inline_branch_rom" "$inline_branch_rebuilt" &&
    grep -q '^        INLINE_BYTE 0x42 ; for JSR Helper$' "$inline_branch_asm" &&
    grep -q '^    NOP$' "$inline_branch_asm" &&
    ! grep -q 'for BCC' "$inline_branch_asm"; then
    printf 'PASS inline_branch.asm\n'
else
    printf 'FAIL inline_branch.asm\n' >&2
    if ! cmp -s "$inline_branch_rom" "$inline_branch_rebuilt"; then
        report_mismatch "$inline_branch_rom" "$inline_branch_rebuilt"
    fi
    exit 1
fi

# ---- paged-bank operand resolves only against its own bank ----
xbank_imm_rom="$OUT/cross_bank_imm.rom"
xbank_imm_asm="$OUT/cross_bank_imm.disasm"
"$ROOT/build/apexasm" "$xbank_imm_rom" "$ROOT/tests/cross_bank_imm.asm"
"$ROOT/build/apexdis" "$xbank_imm_rom" "$xbank_imm_asm" "$ROOT/tests/cross_bank_imm.ini"
# The LDX #0x5123 in bank 0x20 must stay a literal, not borrow bank 0x21's label.
if grep -q '^    LDX #0x5123$' "$xbank_imm_asm" &&
    ! grep -q 'LDX #TargetInBankB' "$xbank_imm_asm"; then
    printf 'PASS cross_bank_imm.asm\n'
else
    printf 'FAIL cross_bank_imm.asm\n' >&2
    exit 1
fi

# ---- [literals]: a marked immediate renders raw instead of resolving to a label ----
# Reuse the cross-bank ROM. Place a same-bank label at 0x5123 so LDX #0x5123 would
# normally resolve to it, then verify [literals] suppresses that resolution.
lit_off_ini="$OUT/literal_off.ini"
lit_on_ini="$OUT/literal_on.ini"
lit_off_asm="$OUT/literal_off.disasm"
lit_on_asm="$OUT/literal_on.disasm"
cat >"$lit_off_ini" <<'EOF'
[labels]
B20_A4001 = Entry
B20_A5123 = SameBankTgt
[entries]
B20_A4001 = code
EOF
cat >"$lit_on_ini" <<'EOF'
[labels]
B20_A4001 = Entry
B20_A5123 = SameBankTgt
[entries]
B20_A4001 = code
[literals]
B20_A4001 = literal
EOF
"$ROOT/build/apexdis" "$xbank_imm_rom" "$lit_off_asm" "$lit_off_ini"
"$ROOT/build/apexdis" "$xbank_imm_rom" "$lit_on_asm" "$lit_on_ini"
if grep -q '^    LDX #SameBankTgt$' "$lit_off_asm" &&
    grep -q '^    LDX #0x5123$' "$lit_on_asm" &&
    ! grep -q 'LDX #SameBankTgt' "$lit_on_asm"; then
    printf 'PASS literals operand suppression\n'
else
    printf 'FAIL literals operand suppression\n' >&2
    exit 1
fi
# apexini must parse, count and round-trip [literals].
lit_norm_ini="$OUT/literal_norm.ini"
"$ROOT/build/apexini" merge "$lit_norm_ini" "$lit_on_ini" >/dev/null
if grep -q '^\[literals\]$' "$lit_norm_ini" &&
    grep -q '^B20_A4001 = literal$' "$lit_norm_ini"; then
    printf 'PASS literals config roundtrip\n'
else
    printf 'FAIL literals config roundtrip\n' >&2
    exit 1
fi

# ---- RAM/ASIC docs render at their symbol equate or in a no-symbol block ----
ram_docs_ini="$OUT/ram_docs.ini"
ram_docs_asm="$OUT/ram_docs.disasm"
cat >"$ram_docs_ini" <<'EOF'
[symbols]
Score_P1 = 0x0150
[docs]
0x0150 = Player 1 score, 4-byte packed BCD.
0x00a3 = scratch flag, no symbol.
EOF
"$ROOT/build/apexdis" "$system_banked_inline_rom" "$ram_docs_asm" "$ram_docs_ini"
if grep -q '^; doc Player 1 score, 4-byte packed BCD\.$' "$ram_docs_asm" &&
    grep -q '^Score_P1 = 0x0150$' "$ram_docs_asm" &&
    grep -q '^; 0x00a3:$' "$ram_docs_asm" &&
    grep -q '^; doc scratch flag, no symbol\.$' "$ram_docs_asm"; then
    printf 'PASS ram_docs.ini\n'
else
    printf 'FAIL ram_docs.ini\n' >&2
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

scoped_dmd_rom="$OUT/scoped_dmd.rom"
"$ROOT/build/apexasm" "$scoped_dmd_rom" "$ROOT/tests/scoped_dmd.asm"

if "$ROOT/build/project_api_test" "$system_banked_inline_rom" \
    "$ROOT/tests/system_banked_inline.ini" \
    "$banked_inline_rom" "$ROOT/tests/banked_inline.ini" \
    "$local_reanalysis_far_rom" "$ROOT/tests/local_reanalysis_far.ini" \
    "$scoped_dmd_rom" "$ROOT/tests/scoped_dmd.ini"; then
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
    grep -q '^    TABLE_FAR_STRING B21_A4001_STRING_HI$' "$far_tables_asm" &&
    grep -q '^    .DB 0x55' "$far_tables_asm" &&
    grep -q '^    .DB 0x55 ; 0x4007 |U|' "$far_tables_asm" &&
    grep -q '^    .DW 0x1234' "$far_tables_asm" &&
    grep -q '^    TABLE_FAR_PTR B21_A4004$' "$far_tables_asm" &&
    grep -q '^; data type=far_code' "$far_tables_asm" &&
    grep -q '^    FAR_CODE B21_A4006$' "$far_tables_asm" &&
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
    grep -q '^        INLINE_FAR_PTR B21_A4004 ; for JSR InlineComplex' "$far_tables_asm" &&
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
    # A far byte naming no real bank (0x18) is left as an honest raw-bank label,
    # not aliased to some other page (phantom mapping removed).
    grep -q '^        INLINE_FAR_CODE B18_A4001 ; for JSR FarCall$' "$config_asm" &&
    ! grep -q 'INLINE_FAR_CODE .*, 0x18 ; for JSR FarCall' "$config_asm" &&
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

if "$ROOT/build/apexsprite_test"; then
    printf 'PASS apexsprite_test\n'
else
    printf 'FAIL apexsprite_test\n' >&2
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
# Far-pointer bank byte is the computed bank id for the single paged bank of a
# 48 KB ROM (banks=1 -> base = 0x3e - 1 = 0x3d).
printf '\100\001\075\102\002\075' | \
    dd of="$apexdmd_table_rom" bs=1 seek=16384 conv=notrunc status=none
cat >"$apexdmd_table_ini" <<'EOF'
[tables]
Bff_A8000 = rows[2](far_data)
EOF
rm -rf "$apexdmd_table_out"
apexdmd_table_line0=$(printf '0\tB3d_A4001\t0x01\t8\trow000_B3d_A4001.pbm\tpair000_001_B3d_A4001_B3d_A4202.pgm')
apexdmd_table_line1=$(printf '1\tB3d_A4202\t0x02\t8\trow001_B3d_A4202.pbm\t-')
apexdmd_table_stat1=$(printf '# adjacent_same_bank\t1')
apexdmd_table_stat2=$(printf '# adjacent_same_type\t0')
if "$ROOT/build/apexdmd" --table "$apexdmd_table_rom" "$apexdmd_table_ini" Bff_A8000 \
    "$apexdmd_table_out" &&
    [ -f "$apexdmd_table_out/row000_B3d_A4001.pbm" ] &&
    [ -f "$apexdmd_table_out/row001_B3d_A4202.pbm" ] &&
    [ -f "$apexdmd_table_out/pair000_001_B3d_A4001_B3d_A4202.pgm" ] &&
    grep -Fqx "$apexdmd_table_line0" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_line1" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_stat1" "$apexdmd_table_summary" &&
    grep -Fqx "$apexdmd_table_stat2" "$apexdmd_table_summary"; then
    printf 'PASS apexdmd_table\n'
else
    printf 'FAIL apexdmd_table\n' >&2
    exit 1
fi

# ---- ROM compare (apexcompare) -------------------------------------------
cmp_base_rom="$OUT/compare_base.rom"
cmp_mod_rom="$OUT/compare_mod.rom"
cmp_out="$OUT/compare_out.txt"
"$ROOT/build/apexasm" "$cmp_base_rom" "$ROOT/tests/compare_base.asm"
"$ROOT/build/apexasm" "$cmp_mod_rom"  "$ROOT/tests/compare_mod.asm"
"$ROOT/build/apexcompare" "$cmp_base_rom" "$cmp_mod_rom" \
    "$ROOT/tests/compare_base.ini" --show-identical >"$cmp_out" 2>&1
if grep -Eq 'identical +code +Bff_A8040 .*R_id'              "$cmp_out" &&
   grep -Eq 'changed +code +Bff_A8060 .*R_chg .*operands'    "$cmp_out" &&
   grep -Eq 'moved +code +Bff_A8080 +-> +Bff_A80c0 .*R_mv'   "$cmp_out" &&
   grep -Eq 'removed +code +Bff_A80a0 .*R_rm'                "$cmp_out" &&
   grep -Eq 'added +code +-- +-> +Bff_A80e0'                 "$cmp_out" &&
   grep -Eq 'changed +string +Bff_A8100'                     "$cmp_out"; then
    printf 'PASS apexcompare\n'
else
    printf 'FAIL apexcompare\n' >&2
    cat "$cmp_out" >&2
    exit 1
fi

# self-compare must report no differences
"$ROOT/build/apexcompare" "$cmp_base_rom" "$cmp_base_rom" \
    "$ROOT/tests/compare_base.ini" >"$cmp_out" 2>&1
if head -1 "$cmp_out" | grep -Eq '0 moved, 0 changed, 0 removed, 0 added'; then
    printf 'PASS apexcompare_self\n'
else
    printf 'FAIL apexcompare_self\n' >&2
    cat "$cmp_out" >&2
    exit 1
fi

# ---- inline flow_stop (tail-call helper stops code decoding) --------------
fs_rom="$OUT/inline_flow_stop.rom"
fs_asm="$OUT/inline_flow_stop.disasm"
fs_rom2="$OUT/inline_flow_stop.rebuilt"
"$ROOT/build/apexasm" "$fs_rom" "$ROOT/tests/inline_flow_stop.asm"
"$ROOT/build/apexdis" "$fs_rom" "$fs_asm" "$ROOT/tests/inline_flow_stop.ini"
"$ROOT/build/apexasm" "$fs_rom2" "$fs_asm"
# flow_stop must end the routine after the inline byte: the COMA (0x43) at 0x8004
# becomes data, and "flow_stop" survives a config round-trip via apexini.
"$ROOT/build/apexini" normalize "$ROOT/tests/inline_flow_stop.ini" "$OUT/inline_flow_stop.norm.ini"
if cmp -s "$fs_rom" "$fs_rom2" &&
   grep -Eq '^    \.DB 0x43' "$fs_asm" &&
   ! grep -Eq '^    COMA' "$fs_asm" &&
   grep -Eq 'byte, flow_stop' "$OUT/inline_flow_stop.norm.ini"; then
    printf 'PASS inline_flow_stop\n'
else
    printf 'FAIL inline_flow_stop\n' >&2
    report_mismatch "$fs_rom" "$fs_rom2" 2>/dev/null || true
    exit 1
fi

# ---- sprite pointer table auto-classifies its image targets (with no-header
#      height carried on the ptr16_sprite(H) field) ------------------------
spt_rom="$OUT/sprite_table.rom"
spt_asm="$OUT/sprite_table.disasm"
spt_rom2="$OUT/sprite_table.rebuilt"
spt_norm="$OUT/sprite_table.norm.ini"
"$ROOT/build/apexasm" "$spt_rom" "$ROOT/tests/sprite_table.asm"
"$ROOT/build/apexdis" "$spt_rom" "$spt_asm" "$ROOT/tests/sprite_table.ini"
"$ROOT/build/apexasm" "$spt_rom2" "$spt_asm"
"$ROOT/build/apexini" normalize "$ROOT/tests/sprite_table.ini" "$spt_norm" >/dev/null 2>&1
if cmp -s "$spt_rom" "$spt_rom2" &&
   grep -Eq 'data type=sprite_noheader\[3\]' "$spt_asm" &&
   grep -Eq '; sprite hdr=0x00 enc=mono' "$spt_asm" &&
   grep -Eq 'rows\[2\]\(ptr16_sprite\(3\)\)' "$spt_norm"; then
    printf 'PASS sprite_table\n'
else
    printf 'FAIL sprite_table\n' >&2
    report_mismatch "$spt_rom" "$spt_rom2" 2>/dev/null || true
    exit 1
fi

# The image classification must also happen through an incremental (scoped)
# re-analysis, not only the full analysis.
if "$ROOT/build/sprite_scope_test" "$spt_rom"; then
    printf 'PASS sprite_scope_test\n'
else
    printf 'FAIL sprite_scope_test\n' >&2
    exit 1
fi

# A sprite data range that fails to decode (here: a 0xFF-fill region whose
# "width" byte is 0xFF > 128) must emit a diagnostic instead of silently
# mis-aligning, and still round-trip byte-identically.
spt_inv_ini="$OUT/sprite_invalid.ini"
spt_inv_asm="$OUT/sprite_invalid.disasm"
spt_inv_rom2="$OUT/sprite_invalid.rebuilt"
cat >"$spt_inv_ini" <<'EOF'
[labels]
Bff_A8000 = TablePtr
[data]
Bff_A8040 = sprite
EOF
"$ROOT/build/apexdis" "$spt_rom" "$spt_inv_asm" "$spt_inv_ini"
"$ROOT/build/apexasm" "$spt_inv_rom2" "$spt_inv_asm"
if grep -Eq '^; WARNING sprite_invalid bank=0xff cpu=0x8040' "$spt_inv_asm" &&
   cmp -s "$spt_rom" "$spt_inv_rom2"; then
    printf 'PASS sprite_invalid\n'
else
    printf 'FAIL sprite_invalid\n' >&2
    exit 1
fi
