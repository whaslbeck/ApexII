.ROM_SIZE 262144

; Version 2 of patch_base.asm for the rebase test: Check and Helper moved
; +0x40, the paged routine +0x10, so the calls to them changed too.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
    .DB 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12
    .DB 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12
PagedRoutine:
    LDA #0x05
    RTS
.BANK SYSTEM
.ORG 0x8000
Start:
    LDA #0x0a
    STA 0x50
    JSR Check
    BNE Start
    JSR Helper
    .DB 0x42
    LDB #0x01
    RTS
.ORG 0x8140
Check:
    LDA 0x50
    CMPA #0x0a
    RTS
.ORG 0x8160
Helper:
    RTS
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
