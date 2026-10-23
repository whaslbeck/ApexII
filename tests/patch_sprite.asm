.ROM_SIZE 262144

; Base ROM for the apexpatch sprite test: a mono sprite, a bicolor-direct
; (0xFF) and a bicolor-indirect (0xFE) sprite, and a no-header image (height
; from the table), all in the system bank behind a 16-bit pointer table.
.BANK 0
.ORG 0x4000
    BANK_ID 0x30
.BANK SYSTEM
.ORG 0x8000
Start:
    LDX #SpriteTable
    RTS
SpriteTable:
    .DW SprMono, SprDirect, SprIndirect, SprNoHdr
SprMono:                        ; 8x2, y 1, x 2
    .DB 0x00, 0x01, 0x02, 0x02, 0x08
    .DB 0x81, 0x42
SprDirect:                      ; 8x2: plane 1, plane 0
    .DB 0xff, 0x00, 0x00, 0x02, 0x08
    .DB 0xf0, 0x0f
    .DB 0x0f, 0xf0
SprIndirect:                    ; 8x2: pointer to plane 1, plane 0
    .DB 0xfe, 0x00, 0x00, 0x02, 0x08
    .DW Plane1
    .DB 0x3c, 0x3c
SprNoHdr:                       ; width 8, height 2 from the table
    .DB 0x08, 0x18, 0x18
Plane1:
    .DB 0xc3, 0xc3
.ORG 0xfff2
    .DW Start, Start, Start, Start, Start, Start, Start
