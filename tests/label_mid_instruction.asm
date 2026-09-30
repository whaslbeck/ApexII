; A [labels] entry that falls inside an inline payload (here the second byte
; after JSR Helper) cannot get a definition line, but operands still use its
; name: it must be emitted as an equate so the listing reassembles.
.ROM_SIZE 524288
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR Helper
    .DB 0x11, 0x22
    LDX #0x8004
    RTS
Helper:
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
