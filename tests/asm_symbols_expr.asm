; Assembler regressions: a defined label that looks like a generated
; Bxx_Ayyyy name must win over the generated decoding (FAR_CODE and bank()),
; and +/- chains evaluate left to right.
.ROM_SIZE 262144
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
.BANK SYSTEM
.ORG 0x8000
Bcd_Add16:
    RTS
Probe:
    INLINE_FAR_CODE Bcd_Add16
    LDA #bank(Bcd_Add16)
    LDX #10-3-2
    LDX #0x100-0x10+0x01
.ORG 0xfff2
    .DW Bcd_Add16
    .DW Bcd_Add16
    .DW Bcd_Add16
    .DW Bcd_Add16
    .DW Bcd_Add16
    .DW Bcd_Add16
    .DW Bcd_Add16
