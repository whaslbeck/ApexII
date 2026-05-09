.ROM_SIZE 524288

.BANK 0
.ORG 0x4000
    BANK_ID 0x20
FarStringTable:
    .DW 0x0001
    .DB 0x03
    TABLE_FAR_STRING B21_A4001, 0x01
FarDataRows:
    .DB 0x55
    .DW 0x1234
    TABLE_FAR_PTR B21_A4004, 0x01
FarCodePtr:
    FAR_CODE B21_A4006, 0x01

.BANK 1
.ORG 0x4000
    BANK_ID 0x21
TargetString:
    STRING "HI"
TargetData:
    .DB 0xaa, 0xbb
TargetCode:
    LDU TargetData
    LDX [TargetData]
    ; Deliberately conflicts with the far_data table reference; data classification must win.
    JSR TargetData
    JSR SystemFromPaged
    JSR InlineParam
    INLINE_BYTE 0x7a
    JSR InlineComplex
    INLINE_PTR TargetData
    INLINE_FAR_PTR B21_A4004, 0x01
    INLINE_CODE_PTR TargetCode
    INLINE_WORD 0x1234
    INLINE_BYTE 0x56
    INLINE_BYTE 0x78
    RTS

.BANK SYSTEM
.ORG 0x8000
Swi3:
    RTS
Swi2:
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
    JSR Handler
    RTS
Handler:
    RTS
SystemFromPaged:
    RTS
InlineParam:
    RTS
InlineComplex:
    RTS

.ORG 0x8010
SystemDataTable:
    TABLE_PTR SystemData
SystemData:
    .DB 0x41, 0x00

.ORG 0xfff2
    .DW Swi3
    .DW Swi3
    .DW Firq
    .DW Irq
    .DW Swi
    .DW Nmi
    .DW Reset
