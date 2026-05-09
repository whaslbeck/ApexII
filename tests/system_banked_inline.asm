.ROM_SIZE 524288

.BANK SYSTEM
.ORG 0x8000
Entry:
    JSR Helper
    INLINE_BYTE 0x42
Gap:
    .DB 0x00
Helper:
    RTS
Swi3:
    RTS
Firq:
    RTS
Irq:
    RTS
Swi:
    RTS
Nmi:
    RTS
Reset:
    JSR Entry
    RTS

.ORG 0xfff2
    .DW Swi3
    .DW Swi3
    .DW Firq
    .DW Irq
    .DW Swi
    .DW Nmi
    .DW Reset
