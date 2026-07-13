.ROM_SIZE 524288
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
.ORG 0x5000
UniqueTarget:
    JSR 0x9000    ; paged -> system: only labelled once the outer convergence
    RTS           ; re-scans this routine (reached via the system JSR 0x5000)
.ORG 0x6000
    RTS
.BANK 1
.ORG 0x4000
    BANK_ID 0x21
.ORG 0x6000
    RTS
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR 0x5000
    JSR 0x6000
    RTS
.ORG 0x9000
SysHelper:
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
