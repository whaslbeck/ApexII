.ROM_SIZE 524288

.BANK 0
.ORG 0x4000
    BANK_ID 0x20
Entry:
    JSR BankInline
    INLINE_BYTE 0x42
    RTS
BankInline:
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
    RTS

.ORG 0xfff2
    .DW Swi3
    .DW Swi3
    .DW Firq
    .DW Irq
    .DW Swi
    .DW Nmi
    .DW Reset
