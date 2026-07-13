.ROM_SIZE 262144
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
BankStart:
    LDX #0x5000
    RTS
.BANK 13
.ORG 0x4000
    BANK_ID 0x3d
.ORG 0x4100
LastBank:
    RTS
.BANK SYSTEM
.ORG 0x8000
Reset:
    JSR BankStart
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
