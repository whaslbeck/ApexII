.ROM_SIZE 524288

; A named RAM block (Score_P1, 4 bytes) and a hex-length one (Buf, 0x10 bytes).
; Address operands inside a block resolve to NAME+offset; an exact hit is the bare
; name.  All of it re-assembles byte-for-byte (apexasm evaluates SYM+n).

.BANK SYSTEM
.ORG 0x8000
START:
    LDA <0x50           ; not in any block -> stays raw
    LDB 0x0152          ; Score_P1+2
    LDX #0x0150         ; immediate, exact -> #Score_P1
    STA 0x0153          ; Score_P1+3
    LDU 0x0205          ; Buf+5
    RTS

.ORG 0xfff0
    .DW START, START, START, START, START, START, START, START
