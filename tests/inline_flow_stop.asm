.ROM_SIZE 524288

.BANK SYSTEM
.ORG 0x8000
Main:
    JSR JmpFar
    INLINE_BYTE 0x55
    COMA
    RTS

.ORG 0x8010
JmpFar:
    PULS X
    LDB ,X+
    RTS

.ORG 0xfff2
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
    .DW Main
