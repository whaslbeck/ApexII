.ROM_SIZE 524288

; Bank 0x20: unused filler (kept so the ROM has several paged banks).
.BANK 0
.ORG 0x4000
    BANK_ID 0x20
    FILL_TO_BANK_END

; Bank 0x21: a DMD frame and a sprite, classified via [data] in scoped_dmd.ini.
; Their generated data labels have no inbound reference, so they must be pinned
; to survive a bank-scoped re-analysis of an unrelated bank.
.BANK 1
.ORG 0x4000
    BANK_ID 0x21
DmdFrame:
    .DB 0x01, 0xaa, 0xaa, 0x00, 0x00, 0xaa, 0x00, 0x00
.ORG 0x4100
Sprite:
    .DB 0x00, 0x00, 0x00, 0x08, 0x08
    .DB 0x18, 0x24, 0x42, 0x81, 0x81, 0x42, 0x24, 0x18
    FILL_TO_BANK_END

; Bank 0x22: an unrelated spot the test classifies, triggering a bank-scoped
; re-analysis of bank 0x22 only.
.BANK 2
.ORG 0x4000
    BANK_ID 0x22
MarkHere:
    .DB 0x11, 0x22, 0x33, 0x44
    FILL_TO_BANK_END

.BANK SYSTEM
.ORG 0x8000
    FILL_TO_BANK_END
