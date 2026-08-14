.ROM_SIZE 524288

; Exercises the opt-in [options] render switches (min_immediate_symbol,
; hex_index_offsets, reference_counts, instruction_addresses).  Must round-trip
; byte-identically both with the default config and with those options enabled.

.BANK SYSTEM
.ORG 0x8000
START:
    LDX #0x0014       ; small immediate == symbol TEXT_FIELD's value
    LDA <0x14         ; address operand of the same symbol
    LDD 15,U          ; 5-bit indexed offset (renders decimal / opt-in hex)
    LDB 2,X
    JSR SUB
    RTS
SUB:
    LEAX 1,X
    RTS

.ORG 0xfff0
    .DW START, START, START, START, START, START, START
