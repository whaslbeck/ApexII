.ROM_SIZE 524288
.BANK SYSTEM
.ORG 0x8000
Reset:
    RTS
.ORG 0x8010
DefaultScore:
    BCD 0001234500
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
