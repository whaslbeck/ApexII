.ROM_SIZE 262144

; Base ROM for the apexpatch data-op tests: a pointer table over a string pool
; (with free space behind it), single strings, a typed value and a data block
; in bank 0x30 behind a far-pointer table.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
DataBlk:
    .DB 0x01, 0x02, 0x03, 0x04
.BANK SYSTEM
.ORG 0x8000
Start:
    LDX #Str2
    LDX #StrTable
    LDU #FarTable
    LDA Lives
    LDX #StrB
    LDX #StrC
    RTS
.ORG 0x8100
StrTable:
    .DW Str1, Str2, Str3
Str1:
    STRING "HELLO"
Str2:
    STRING "WORLD"
Str3:
    STRING "BYE"
.ORG 0x8200
StrB:
    STRING "BYE"
StrC:
    STRING "SHORT"
Lives:
    .DB 0x05
FarTable:
    FAR_PTR DataBlk, 0x30
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
