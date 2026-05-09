#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <input> <output.c> <symbol>" >&2
    exit 1
fi

input=$1
output=$2
symbol=$3
tmp="$output.tmp"
size=$(wc -c < "$input" | tr -d ' ')

{
    printf '/* Generated from %s. Do not edit. */\n' "$input"
    printf '\n'
    printf 'const unsigned char %s[] = {\n' "$symbol"
    od -An -tx1 -v "$input" | awk '
        {
            for (i = 1; i <= NF; i++) {
                printf "    0x%s,\n", $i
            }
        }
    '
    printf '};\n'
    printf '\n'
    printf 'const unsigned int %s_len = %s;\n' "$symbol" "$size"
} > "$tmp"

mv "$tmp" "$output"
