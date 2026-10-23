.ROM_SIZE 262144

; patch_base.asm with the edits of patch_ops.patch.ini applied by hand: the
; reference image apexpatch build must reproduce byte for byte.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
PagedRoutine:
    LDA #0x05
    RTS
.BANK SYSTEM
.ORG 0x8000
Start:
    LDA #0x14
    STA 0x50
    JSR Check
    BNE Start
    JSR NewThing
    NOP
    LDB #0x01
    RTS
.ORG 0x8100
Check:
    LDA 0x50
    NOP
    NOP
    RTS
.ORG 0x8120
Helper:
    RTS
.ORG 0x9000
NewThing:
    LDA #0x01
    JSR Check
    RTS
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
