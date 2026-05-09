.ROM_SIZE 524288

.BANK 0
.ORG 0x4000
    BANK_ID 0x20
Entry:
    JSR InlineFar
    INLINE_FAR_CODE Target, 0x01
    RTS
InlineFar:
    RTS

.BANK 1
.ORG 0x4000
    BANK_ID 0x21
Target:
    RTS

.BANK SYSTEM
.ORG 0x8000
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
