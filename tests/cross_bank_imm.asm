.ROM_SIZE 524288

; A paged-bank operand must resolve only against its own bank.  Bank 0x20 loads
; #0x5123; that address has a label only in bank 0x21, which must NOT be used.
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
Entry:
    LDX #0x5123
    RTS
.BANK 1
.ORG 0x4000
    BANK_ID 0x21
.ORG 0x5123
Target:
    RTS
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR Entry
    RTS
.ORG 0xfff2
    .DW Reset, Reset, Reset, Reset, Reset, Reset, Reset
