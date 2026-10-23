.ROM_SIZE 262144

; patch_data.asm with patch_data.patch.ini applied by hand: the reference
; image for apexpatch build (value, string shift / in place / relocate, move).
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
.BANK 1
.ORG 0x4002
    .DB 0x01, 0x02, 0x03, 0x04
.BANK SYSTEM
.ORG 0x8000
Start:
    LDX #0x810e
    LDX #0x8100
    LDU #0x820b
    LDA 0x820a
    LDX #0x8200
    LDX #0x8014
    RTS
.ORG 0x8014
    STRING "A MUCH LONGER TEXT"
.ORG 0x8100
    .DW 0x8106, 0x810e, 0x8114
    STRING "HELLO!!"
    STRING "WORLD"
    STRING "BYE"
.ORG 0x8200
    .DB 0x42, 0x59, 0x00, 0x00
    STRING "SHORT"
    .DB 0x09
    .DB 0x40, 0x02, 0x31
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
