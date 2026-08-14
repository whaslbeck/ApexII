.ROM_SIZE 524288

; Two variable-length inline payloads.  EFF_UNTIL consumes a 0x00-terminated byte
; list (bytes_until(0x00)); EFF_COUNT consumes a leading count byte plus that many
; bytes (counted_bytes).  The disassembler resolves each length from the ROM and
; emits INLINE_BYTES_UNTIL / INLINE_COUNTED_BYTES, which re-assemble byte-for-byte.

.BANK SYSTEM
.ORG 0x8000
START:
    JSR EFF_UNTIL
    .DB 0x05, 0xf0, 0x03, 0x00
    JSR EFF_COUNT
    .DB 0x03, 0xaa, 0xbb, 0xcc
    RTS
EFF_UNTIL:
    LDU ,S
    RTS
EFF_COUNT:
    LDU ,S
    RTS

.ORG 0xfff0
    .DW START, START, START, START, START, START, START, START
