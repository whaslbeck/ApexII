.ROM_SIZE 524288

; A code reference to a long string.  The disassembler auto-labels the string
; with its (capped) content; the same capped name must appear at the definition
; and at the LDX reference, or the operand-buffer truncation that used to hit
; long labels would break round-trip.

.BANK SYSTEM
.ORG 0x8000
START:
    LDX #0x8010
    RTS
.ORG 0x8010
    STRING "THIS IS A VERY LONG STRING THAT EXCEEDS FORTY CHARACTERS EASILY YES"
.ORG 0xfff0
    .DW START, START, START, START, START, START, START
