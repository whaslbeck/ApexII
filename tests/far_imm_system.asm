; [far_imm] on a system-bank instruction: the system-bank code pass must honour
; it like the paged passes do (it used to get no exclusions/literals/far_imms).
.ROM_SIZE 524288
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
.BANK 24
.ORG 0x4000
    BANK_ID 0x38
.ORG 0x5123
FarThing:
    RTS
.BANK SYSTEM
.ORG 0x8000
Reset:
    LDX #0x5123
    LDB #0x38
    RTS
.ORG 0xfff2
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
    .DW Reset
