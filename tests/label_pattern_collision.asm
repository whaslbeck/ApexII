.ROM_SIZE 524288
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR Bcd_Add16
    RTS
.ORG 0xa651
Bcd_Add16:
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
