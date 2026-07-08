.ROM_SIZE 524288
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
Start:
    LDX #0x5123
    LDB #0x38
    RTS
.BANK 24
.ORG 0x4000
    BANK_ID 0x38
.ORG 0x5123
FarThing:
    RTS
.BANK SYSTEM
.ORG 0x8000
Reset:
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
