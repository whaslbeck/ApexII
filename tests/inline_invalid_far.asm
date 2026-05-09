.ROM_SIZE 524288

.BANK 0
.ORG 0x4000
    BANK_ID 0x20
BankStart:
    JSR InlineConsumer
    INLINE_FAR_CODE 0x1234, 0x7e
    RTS

.BANK SYSTEM
.ORG 0x8000
InlineConsumer:
    RTS

.ORG 0xfffe
    .DW InlineConsumer
