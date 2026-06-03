#include "apex.h"
#include "cpu6809.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    OPER_NONE,
    OPER_IMM8,
    OPER_IMM16,
    OPER_DIRECT,
    OPER_EXT,
    OPER_INDEXED,
    OPER_REL8,
    OPER_REL16,
    OPER_POSTBYTE
} OperandKind;

typedef struct {
    const char *mnemonic;
    OperandKind kind;
    uint8_t prefix;
    uint8_t opcode;
    unsigned flags;
} Opcode;

static uint16_t be16(const uint8_t *p);

static const Opcode opcodes[] = {
    {"NOP", OPER_NONE, 0x00, 0x12, 0},
    {"SYNC", OPER_NONE, 0x00, 0x13, 0},
    {"CLRA", OPER_NONE, 0x00, 0x4f, 0},
    {"CLRB", OPER_NONE, 0x00, 0x5f, 0},
    {"NEGA", OPER_NONE, 0x00, 0x40, 0},
    {"NEGB", OPER_NONE, 0x00, 0x50, 0},
    {"COMA", OPER_NONE, 0x00, 0x43, 0},
    {"COMB", OPER_NONE, 0x00, 0x53, 0},
    {"LSRA", OPER_NONE, 0x00, 0x44, 0},
    {"LSRB", OPER_NONE, 0x00, 0x54, 0},
    {"RORA", OPER_NONE, 0x00, 0x46, 0},
    {"RORB", OPER_NONE, 0x00, 0x56, 0},
    {"ASRA", OPER_NONE, 0x00, 0x47, 0},
    {"ASRB", OPER_NONE, 0x00, 0x57, 0},
    {"ASLA", OPER_NONE, 0x00, 0x48, 0},
    {"LSLA", OPER_NONE, 0x00, 0x48, 0},
    {"ASLB", OPER_NONE, 0x00, 0x58, 0},
    {"LSLB", OPER_NONE, 0x00, 0x58, 0},
    {"ROLA", OPER_NONE, 0x00, 0x49, 0},
    {"ROLB", OPER_NONE, 0x00, 0x59, 0},
    {"DECA", OPER_NONE, 0x00, 0x4a, 0},
    {"DECB", OPER_NONE, 0x00, 0x5a, 0},
    {"INCA", OPER_NONE, 0x00, 0x4c, 0},
    {"INCB", OPER_NONE, 0x00, 0x5c, 0},
    {"TSTA", OPER_NONE, 0x00, 0x4d, 0},
    {"TSTB", OPER_NONE, 0x00, 0x5d, 0},
    {"DAA", OPER_NONE, 0x00, 0x19, 0},
    {"SEX", OPER_NONE, 0x00, 0x1d, 0},
    {"RTS", OPER_NONE, 0x00, 0x39, CPU6809_FLOW_STOP},
    {"ABX", OPER_NONE, 0x00, 0x3a, 0},
    {"RTI", OPER_NONE, 0x00, 0x3b, CPU6809_FLOW_STOP},
    {"MUL", OPER_NONE, 0x00, 0x3d, 0},
    {"SWI", OPER_NONE, 0x00, 0x3f, 0},
    {"SWI2", OPER_NONE, 0x10, 0x3f, 0},
    {"SWI3", OPER_NONE, 0x11, 0x3f, 0},
    {"EXG", OPER_POSTBYTE, 0x00, 0x1e, 0},
    {"TFR", OPER_POSTBYTE, 0x00, 0x1f, 0},
    {"PSHS", OPER_POSTBYTE, 0x00, 0x34, 0},
    {"PULS", OPER_POSTBYTE, 0x00, 0x35, 0},
    {"PSHU", OPER_POSTBYTE, 0x00, 0x36, 0},
    {"PULU", OPER_POSTBYTE, 0x00, 0x37, 0},
    {"LEAX", OPER_INDEXED, 0x00, 0x30, 0},
    {"LEAY", OPER_INDEXED, 0x00, 0x31, 0},
    {"LEAS", OPER_INDEXED, 0x00, 0x32, 0},
    {"LEAU", OPER_INDEXED, 0x00, 0x33, 0},
    {"ORCC", OPER_IMM8, 0x00, 0x1a, 0},
    {"ANDCC", OPER_IMM8, 0x00, 0x1c, 0},
    {"CWAI", OPER_IMM8, 0x00, 0x3c, 0},
    {"SUBA", OPER_IMM8, 0x00, 0x80, 0},
    {"CMPA", OPER_IMM8, 0x00, 0x81, 0},
    {"SBCA", OPER_IMM8, 0x00, 0x82, 0},
    {"ANDA", OPER_IMM8, 0x00, 0x84, 0},
    {"BITA", OPER_IMM8, 0x00, 0x85, 0},
    {"LDA", OPER_IMM8, 0x00, 0x86, 0},
    {"EORA", OPER_IMM8, 0x00, 0x88, 0},
    {"ADCA", OPER_IMM8, 0x00, 0x89, 0},
    {"ORA", OPER_IMM8, 0x00, 0x8a, 0},
    {"ADDA", OPER_IMM8, 0x00, 0x8b, 0},
    {"SUBB", OPER_IMM8, 0x00, 0xc0, 0},
    {"CMPB", OPER_IMM8, 0x00, 0xc1, 0},
    {"SBCB", OPER_IMM8, 0x00, 0xc2, 0},
    {"ANDB", OPER_IMM8, 0x00, 0xc4, 0},
    {"BITB", OPER_IMM8, 0x00, 0xc5, 0},
    {"LDB", OPER_IMM8, 0x00, 0xc6, 0},
    {"EORB", OPER_IMM8, 0x00, 0xc8, 0},
    {"ADCB", OPER_IMM8, 0x00, 0xc9, 0},
    {"ORB", OPER_IMM8, 0x00, 0xca, 0},
    {"ADDB", OPER_IMM8, 0x00, 0xcb, 0},
    {"SUBD", OPER_IMM16, 0x00, 0x83, 0},
    {"CMPU", OPER_IMM16, 0x11, 0x83, 0},
    {"CMPY", OPER_IMM16, 0x10, 0x8c, 0},
    {"CMPS", OPER_IMM16, 0x11, 0x8c, 0},
    {"LDX", OPER_IMM16, 0x00, 0x8e, 0},
    {"LDD", OPER_IMM16, 0x00, 0xcc, 0},
    {"LDU", OPER_IMM16, 0x00, 0xce, 0},
    {"LDY", OPER_IMM16, 0x10, 0x8e, 0},
    {"LDS", OPER_IMM16, 0x10, 0xce, 0},
    {"CMPX", OPER_IMM16, 0x00, 0x8c, 0},
    {"SUBD", OPER_IMM16, 0x00, 0x83, 0},
    {"ADDD", OPER_IMM16, 0x00, 0xc3, 0},
    {"CMPD", OPER_IMM16, 0x10, 0x83, 0},
    {"NEG", OPER_DIRECT, 0x00, 0x00, 0},
    {"COM", OPER_DIRECT, 0x00, 0x03, 0},
    {"LSR", OPER_DIRECT, 0x00, 0x04, 0},
    {"ROR", OPER_DIRECT, 0x00, 0x06, 0},
    {"ASR", OPER_DIRECT, 0x00, 0x07, 0},
    {"ASL", OPER_DIRECT, 0x00, 0x08, 0},
    {"LSL", OPER_DIRECT, 0x00, 0x08, 0},
    {"ROL", OPER_DIRECT, 0x00, 0x09, 0},
    {"DEC", OPER_DIRECT, 0x00, 0x0a, 0},
    {"INC", OPER_DIRECT, 0x00, 0x0c, 0},
    {"TST", OPER_DIRECT, 0x00, 0x0d, 0},
    {"JMP", OPER_DIRECT, 0x00, 0x0e, CPU6809_FLOW_STOP},
    {"CLR", OPER_DIRECT, 0x00, 0x0f, 0},
    {"SUBA", OPER_DIRECT, 0x00, 0x90, 0},
    {"CMPA", OPER_DIRECT, 0x00, 0x91, 0},
    {"SBCA", OPER_DIRECT, 0x00, 0x92, 0},
    {"SUBD", OPER_DIRECT, 0x00, 0x93, 0},
    {"ANDA", OPER_DIRECT, 0x00, 0x94, 0},
    {"BITA", OPER_DIRECT, 0x00, 0x95, 0},
    {"LDA", OPER_DIRECT, 0x00, 0x96, 0},
    {"STA", OPER_DIRECT, 0x00, 0x97, 0},
    {"EORA", OPER_DIRECT, 0x00, 0x98, 0},
    {"ADCA", OPER_DIRECT, 0x00, 0x99, 0},
    {"ORA", OPER_DIRECT, 0x00, 0x9a, 0},
    {"ADDA", OPER_DIRECT, 0x00, 0x9b, 0},
    {"CMPX", OPER_DIRECT, 0x00, 0x9c, 0},
    {"JSR", OPER_DIRECT, 0x00, 0x9d, 0},
    {"LDX", OPER_DIRECT, 0x00, 0x9e, 0},
    {"STX", OPER_DIRECT, 0x00, 0x9f, 0},
    {"SUBB", OPER_DIRECT, 0x00, 0xd0, 0},
    {"CMPB", OPER_DIRECT, 0x00, 0xd1, 0},
    {"SBCB", OPER_DIRECT, 0x00, 0xd2, 0},
    {"ADDD", OPER_DIRECT, 0x00, 0xd3, 0},
    {"ANDB", OPER_DIRECT, 0x00, 0xd4, 0},
    {"BITB", OPER_DIRECT, 0x00, 0xd5, 0},
    {"LDB", OPER_DIRECT, 0x00, 0xd6, 0},
    {"STB", OPER_DIRECT, 0x00, 0xd7, 0},
    {"EORB", OPER_DIRECT, 0x00, 0xd8, 0},
    {"ADCB", OPER_DIRECT, 0x00, 0xd9, 0},
    {"ORB", OPER_DIRECT, 0x00, 0xda, 0},
    {"ADDB", OPER_DIRECT, 0x00, 0xdb, 0},
    {"LDD", OPER_DIRECT, 0x00, 0xdc, 0},
    {"STD", OPER_DIRECT, 0x00, 0xdd, 0},
    {"LDU", OPER_DIRECT, 0x00, 0xde, 0},
    {"STU", OPER_DIRECT, 0x00, 0xdf, 0},
    {"CMPD", OPER_DIRECT, 0x10, 0x93, 0},
    {"CMPY", OPER_DIRECT, 0x10, 0x9c, 0},
    {"LDY", OPER_DIRECT, 0x10, 0x9e, 0},
    {"STY", OPER_DIRECT, 0x10, 0x9f, 0},
    {"LDS", OPER_DIRECT, 0x10, 0xde, 0},
    {"STS", OPER_DIRECT, 0x10, 0xdf, 0},
    {"CMPU", OPER_DIRECT, 0x11, 0x93, 0},
    {"CMPS", OPER_DIRECT, 0x11, 0x9c, 0},
    {"NEG", OPER_INDEXED, 0x00, 0x60, 0},
    {"COM", OPER_INDEXED, 0x00, 0x63, 0},
    {"LSR", OPER_INDEXED, 0x00, 0x64, 0},
    {"ROR", OPER_INDEXED, 0x00, 0x66, 0},
    {"ASR", OPER_INDEXED, 0x00, 0x67, 0},
    {"ASL", OPER_INDEXED, 0x00, 0x68, 0},
    {"LSL", OPER_INDEXED, 0x00, 0x68, 0},
    {"ROL", OPER_INDEXED, 0x00, 0x69, 0},
    {"DEC", OPER_INDEXED, 0x00, 0x6a, 0},
    {"INC", OPER_INDEXED, 0x00, 0x6c, 0},
    {"TST", OPER_INDEXED, 0x00, 0x6d, 0},
    {"JMP", OPER_INDEXED, 0x00, 0x6e, CPU6809_FLOW_STOP},
    {"CLR", OPER_INDEXED, 0x00, 0x6f, 0},
    {"SUBA", OPER_INDEXED, 0x00, 0xa0, 0},
    {"CMPA", OPER_INDEXED, 0x00, 0xa1, 0},
    {"SBCA", OPER_INDEXED, 0x00, 0xa2, 0},
    {"SUBD", OPER_INDEXED, 0x00, 0xa3, 0},
    {"ANDA", OPER_INDEXED, 0x00, 0xa4, 0},
    {"BITA", OPER_INDEXED, 0x00, 0xa5, 0},
    {"LDA", OPER_INDEXED, 0x00, 0xa6, 0},
    {"STA", OPER_INDEXED, 0x00, 0xa7, 0},
    {"EORA", OPER_INDEXED, 0x00, 0xa8, 0},
    {"ADCA", OPER_INDEXED, 0x00, 0xa9, 0},
    {"ORA", OPER_INDEXED, 0x00, 0xaa, 0},
    {"ADDA", OPER_INDEXED, 0x00, 0xab, 0},
    {"CMPX", OPER_INDEXED, 0x00, 0xac, 0},
    {"JSR", OPER_INDEXED, 0x00, 0xad, 0},
    {"LDX", OPER_INDEXED, 0x00, 0xae, 0},
    {"STX", OPER_INDEXED, 0x00, 0xaf, 0},
    {"SUBB", OPER_INDEXED, 0x00, 0xe0, 0},
    {"CMPB", OPER_INDEXED, 0x00, 0xe1, 0},
    {"SBCB", OPER_INDEXED, 0x00, 0xe2, 0},
    {"ADDD", OPER_INDEXED, 0x00, 0xe3, 0},
    {"ANDB", OPER_INDEXED, 0x00, 0xe4, 0},
    {"BITB", OPER_INDEXED, 0x00, 0xe5, 0},
    {"LDB", OPER_INDEXED, 0x00, 0xe6, 0},
    {"STB", OPER_INDEXED, 0x00, 0xe7, 0},
    {"EORB", OPER_INDEXED, 0x00, 0xe8, 0},
    {"ADCB", OPER_INDEXED, 0x00, 0xe9, 0},
    {"ORB", OPER_INDEXED, 0x00, 0xea, 0},
    {"ADDB", OPER_INDEXED, 0x00, 0xeb, 0},
    {"LDD", OPER_INDEXED, 0x00, 0xec, 0},
    {"STD", OPER_INDEXED, 0x00, 0xed, 0},
    {"LDU", OPER_INDEXED, 0x00, 0xee, 0},
    {"STU", OPER_INDEXED, 0x00, 0xef, 0},
    {"CMPD", OPER_INDEXED, 0x10, 0xa3, 0},
    {"CMPY", OPER_INDEXED, 0x10, 0xac, 0},
    {"LDY", OPER_INDEXED, 0x10, 0xae, 0},
    {"STY", OPER_INDEXED, 0x10, 0xaf, 0},
    {"LDS", OPER_INDEXED, 0x10, 0xee, 0},
    {"STS", OPER_INDEXED, 0x10, 0xef, 0},
    {"CMPU", OPER_INDEXED, 0x11, 0xa3, 0},
    {"CMPS", OPER_INDEXED, 0x11, 0xac, 0},
    {"JSR", OPER_EXT, 0x00, 0xbd, CPU6809_TARGET_CODE},
    {"JMP", OPER_EXT, 0x00, 0x7e, CPU6809_FLOW_STOP | CPU6809_TARGET_CODE},
    {"SUBA", OPER_EXT, 0x00, 0xb0, 0},
    {"SBCA", OPER_EXT, 0x00, 0xb2, 0},
    {"ANDA", OPER_EXT, 0x00, 0xb4, 0},
    {"BITA", OPER_EXT, 0x00, 0xb5, 0},
    {"STA", OPER_EXT, 0x00, 0xb7, 0},
    {"EORA", OPER_EXT, 0x00, 0xb8, 0},
    {"ADCA", OPER_EXT, 0x00, 0xb9, 0},
    {"ORA", OPER_EXT, 0x00, 0xba, 0},
    {"ADDA", OPER_EXT, 0x00, 0xbb, 0},
    {"CMPX", OPER_EXT, 0x00, 0xbc, 0},
    {"STB", OPER_EXT, 0x00, 0xf7, 0},
    {"STD", OPER_EXT, 0x00, 0xfd, 0},
    {"STX", OPER_EXT, 0x00, 0xbf, 0},
    {"LDA", OPER_EXT, 0x00, 0xb6, 0},
    {"SUBB", OPER_EXT, 0x00, 0xf0, 0},
    {"CMPB", OPER_EXT, 0x00, 0xf1, 0},
    {"SBCB", OPER_EXT, 0x00, 0xf2, 0},
    {"ADDD", OPER_EXT, 0x00, 0xf3, 0},
    {"ANDB", OPER_EXT, 0x00, 0xf4, 0},
    {"BITB", OPER_EXT, 0x00, 0xf5, 0},
    {"LDB", OPER_EXT, 0x00, 0xf6, 0},
    {"EORB", OPER_EXT, 0x00, 0xf8, 0},
    {"ADCB", OPER_EXT, 0x00, 0xf9, 0},
    {"ORB", OPER_EXT, 0x00, 0xfa, 0},
    {"ADDB", OPER_EXT, 0x00, 0xfb, 0},
    {"LDD", OPER_EXT, 0x00, 0xfc, 0},
    {"LDX", OPER_EXT, 0x00, 0xbe, 0},
    {"LDU", OPER_EXT, 0x00, 0xfe, 0},
    {"STU", OPER_EXT, 0x00, 0xff, 0},
    {"NEG", OPER_EXT, 0x00, 0x70, 0},
    {"COM", OPER_EXT, 0x00, 0x73, 0},
    {"LSR", OPER_EXT, 0x00, 0x74, 0},
    {"ROR", OPER_EXT, 0x00, 0x76, 0},
    {"ASR", OPER_EXT, 0x00, 0x77, 0},
    {"ASL", OPER_EXT, 0x00, 0x78, 0},
    {"LSL", OPER_EXT, 0x00, 0x78, 0},
    {"ROL", OPER_EXT, 0x00, 0x79, 0},
    {"DEC", OPER_EXT, 0x00, 0x7a, 0},
    {"INC", OPER_EXT, 0x00, 0x7c, 0},
    {"CLR", OPER_EXT, 0x00, 0x7f, 0},
    {"TST", OPER_EXT, 0x00, 0x7d, 0},
    {"CMPA", OPER_EXT, 0x00, 0xb1, 0},
    {"SUBD", OPER_EXT, 0x00, 0xb3, 0},
    {"CMPD", OPER_EXT, 0x10, 0xb3, 0},
    {"CMPY", OPER_EXT, 0x10, 0xbc, 0},
    {"LDY", OPER_EXT, 0x10, 0xbe, 0},
    {"STY", OPER_EXT, 0x10, 0xbf, 0},
    {"LDS", OPER_EXT, 0x10, 0xfe, 0},
    {"STS", OPER_EXT, 0x10, 0xff, 0},
    {"CMPU", OPER_EXT, 0x11, 0xb3, 0},
    {"CMPS", OPER_EXT, 0x11, 0xbc, 0},
    {"BRA", OPER_REL8, 0x00, 0x20, CPU6809_FLOW_STOP | CPU6809_TARGET_CODE},
    {"BRN", OPER_REL8, 0x00, 0x21, 0},
    {"BHI", OPER_REL8, 0x00, 0x22, CPU6809_TARGET_CODE},
    {"BLS", OPER_REL8, 0x00, 0x23, CPU6809_TARGET_CODE},
    {"BCC", OPER_REL8, 0x00, 0x24, CPU6809_TARGET_CODE},
    {"BHS", OPER_REL8, 0x00, 0x24, CPU6809_TARGET_CODE},
    {"BCS", OPER_REL8, 0x00, 0x25, CPU6809_TARGET_CODE},
    {"BLO", OPER_REL8, 0x00, 0x25, CPU6809_TARGET_CODE},
    {"BNE", OPER_REL8, 0x00, 0x26, CPU6809_TARGET_CODE},
    {"BEQ", OPER_REL8, 0x00, 0x27, CPU6809_TARGET_CODE},
    {"BVC", OPER_REL8, 0x00, 0x28, CPU6809_TARGET_CODE},
    {"BVS", OPER_REL8, 0x00, 0x29, CPU6809_TARGET_CODE},
    {"BPL", OPER_REL8, 0x00, 0x2a, CPU6809_TARGET_CODE},
    {"BMI", OPER_REL8, 0x00, 0x2b, CPU6809_TARGET_CODE},
    {"BGE", OPER_REL8, 0x00, 0x2c, CPU6809_TARGET_CODE},
    {"BLT", OPER_REL8, 0x00, 0x2d, CPU6809_TARGET_CODE},
    {"BGT", OPER_REL8, 0x00, 0x2e, CPU6809_TARGET_CODE},
    {"BLE", OPER_REL8, 0x00, 0x2f, CPU6809_TARGET_CODE},
    {"BSR", OPER_REL8, 0x00, 0x8d, CPU6809_TARGET_CODE},
    {"LBRA", OPER_REL16, 0x00, 0x16, CPU6809_FLOW_STOP | CPU6809_TARGET_CODE},
    {"LBRA10", OPER_REL16, 0x10, 0x20, CPU6809_FLOW_STOP | CPU6809_TARGET_CODE},
    {"LBRN", OPER_REL16, 0x10, 0x21, 0},
    {"LBHI", OPER_REL16, 0x10, 0x22, CPU6809_TARGET_CODE},
    {"LBLS", OPER_REL16, 0x10, 0x23, CPU6809_TARGET_CODE},
    {"LBCC", OPER_REL16, 0x10, 0x24, CPU6809_TARGET_CODE},
    {"LBHS", OPER_REL16, 0x10, 0x24, CPU6809_TARGET_CODE},
    {"LBCS", OPER_REL16, 0x10, 0x25, CPU6809_TARGET_CODE},
    {"LBLO", OPER_REL16, 0x10, 0x25, CPU6809_TARGET_CODE},
    {"LBNE", OPER_REL16, 0x10, 0x26, CPU6809_TARGET_CODE},
    {"LBEQ", OPER_REL16, 0x10, 0x27, CPU6809_TARGET_CODE},
    {"LBVC", OPER_REL16, 0x10, 0x28, CPU6809_TARGET_CODE},
    {"LBVS", OPER_REL16, 0x10, 0x29, CPU6809_TARGET_CODE},
    {"LBPL", OPER_REL16, 0x10, 0x2a, CPU6809_TARGET_CODE},
    {"LBMI", OPER_REL16, 0x10, 0x2b, CPU6809_TARGET_CODE},
    {"LBGE", OPER_REL16, 0x10, 0x2c, CPU6809_TARGET_CODE},
    {"LBLT", OPER_REL16, 0x10, 0x2d, CPU6809_TARGET_CODE},
    {"LBGT", OPER_REL16, 0x10, 0x2e, CPU6809_TARGET_CODE},
    {"LBLE", OPER_REL16, 0x10, 0x2f, CPU6809_TARGET_CODE},
    {"LBSR", OPER_REL16, 0x00, 0x17, CPU6809_TARGET_CODE},
    {"LBEQ", OPER_REL16, 0x10, 0x27, CPU6809_TARGET_CODE},
    {"LBNE", OPER_REL16, 0x10, 0x26, CPU6809_TARGET_CODE}
};

static int same_mnemonic(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static char *skip_hash(char *s)
{
    s = trim(s);
    if (*s == '#') {
        s++;
    }
    return trim(s);
}

static int parse_operand(void *ctx, Cpu6809ResolveFn resolve, int resolve_symbols, char *operand,
                         uint32_t *value)
{
    operand = trim(operand);
    if (parse_u32(operand, value)) {
        return 1;
    }
    if (!resolve_symbols) {
        *value = 0;
        return 1;
    }
    return resolve(ctx, operand, value);
}

static int parse_direct_operand(void *ctx, Cpu6809ResolveFn resolve, int resolve_symbols,
                                char *operand, uint32_t *value)
{
    operand = trim(operand);
    if (*operand == '<') {
        operand++;
    }
    return parse_operand(ctx, resolve, resolve_symbols, operand, value);
}

static void emit_opcode(const Opcode *op, Cpu6809EmitFn emit, void *ctx)
{
    if (op->prefix) {
        emit(ctx, op->prefix);
    }
    emit(ctx, op->opcode);
}

static size_t indexed_extra_size(uint8_t postbyte)
{
    if ((postbyte & 0x80u) == 0) {
        return 0;
    }
    switch (postbyte & 0x0fu) {
    case 0x08:
    case 0x0c:
        return 1;
    case 0x09:
    case 0x0d:
    case 0x0f:
        return 2;
    default:
        return 0;
    }
}

static char *parse_comma_value(void *ctx, Cpu6809ResolveFn resolve, int resolve_symbols, char *p,
                               uint32_t *value)
{
    char *start;

    p = trim(p);
    if (*p == ',') {
        p++;
    }
    p = trim(p);
    start = p;
    while (*p && *p != ',') {
        p++;
    }
    if (*p == ',') {
        *p++ = '\0';
    }
    start = trim(start);
    if (!parse_operand(ctx, resolve, resolve_symbols, start, value)) {
        die("invalid indexed operand value '%s'", start);
    }
    return p;
}

static const char *index_reg_name(unsigned reg)
{
    static const char *names[] = {"X", "Y", "U", "S"};

    return reg < 4u ? names[reg] : "?";
}

static int parse_index_reg(const char *s, unsigned *reg, const char **end)
{
    char c = (char)toupper((unsigned char)*s);

    if (c == 'X') {
        *reg = 0;
    } else if (c == 'Y') {
        *reg = 1;
    } else if (c == 'U') {
        *reg = 2;
    } else if (c == 'S') {
        *reg = 3;
    } else {
        return 0;
    }
    *end = s + 1;
    return 1;
}

static int parse_signed_number(char *s, int32_t *value)
{
    int negative = 0;
    uint32_t magnitude;

    s = trim(s);
    if (*s == '-') {
        negative = 1;
        s = trim(s + 1);
    } else if (*s == '+') {
        s = trim(s + 1);
    }
    if (!parse_u32(s, &magnitude) || magnitude > 65535u) {
        return 0;
    }
    *value = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    return 1;
}

static size_t indexed_forced_offset_size(char *s)
{
    size_t digits = 0;

    s = trim(s);
    if (*s == '-' || *s == '+') {
        s = trim(s + 1);
    }
    if (*s == '$') {
        s++;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    } else {
        return 0;
    }
    while (isxdigit((unsigned char)s[digits])) {
        digits++;
    }
    if (digits > 2u) {
        return 2;
    }
    return digits > 0u ? 1u : 0u;
}

static int parse_indexed_pretty(void *ctx, Cpu6809ResolveFn resolve, int resolve_symbols,
                                char *operand, uint8_t *postbyte, uint16_t *extra,
                                size_t *extra_size)
{
    char *inner;
    char *comma;
    char *left;
    char *right;
    char *end;
    int indirect = 0;
    unsigned reg;
    int32_t offset;
    size_t forced_size;

    operand = trim(operand);
    if (*operand == '[') {
        size_t len = strlen(operand);
        if (len < 3u || operand[len - 1u] != ']') {
            return 0;
        }
        operand[len - 1u] = '\0';
        operand = trim(operand + 1);
        indirect = 0x10;
    }
    inner = operand;
    comma = strchr(inner, ',');
    if (!comma) {
        uint32_t value;

        if (!indirect || !parse_operand(ctx, resolve, resolve_symbols, inner, &value) ||
            value > 0xffffu) {
            return 0;
        }
        *postbyte = 0x9fu;
        *extra = (uint16_t)value;
        *extra_size = 2;
        return 1;
    }
    *comma = '\0';
    left = trim(inner);
    right = trim(comma + 1);

    if (left[0] == '\0') {
        const char *p = right;
        int predec = 0;

        if (*p == '-') {
            predec++;
            p++;
            if (*p == '-') {
                predec++;
                p++;
            }
        }
        if (!parse_index_reg(p, &reg, (const char **)&end)) {
            return 0;
        }
        if (predec) {
            if (*trim(end) != '\0') {
                return 0;
            }
            *postbyte = (uint8_t)(0x80u | (reg << 5) | indirect | (predec == 1 ? 0x02u : 0x03u));
            *extra_size = 0;
            return 1;
        }
        if (*end == '+') {
            int postinc = 1;
            end++;
            if (*end == '+') {
                postinc++;
                end++;
            }
            if (*trim(end) != '\0') {
                return 0;
            }
            *postbyte = (uint8_t)(0x80u | (reg << 5) | indirect | (postinc == 1 ? 0x00u : 0x01u));
            *extra_size = 0;
            return 1;
        }
        if (*trim(end) != '\0') {
            return 0;
        }
        *postbyte = (uint8_t)(0x84u | (reg << 5) | indirect);
        *extra_size = 0;
        return 1;
    }

    if (strlen(left) == 1u) {
        char c = (char)toupper((unsigned char)left[0]);
        if ((c == 'A' || c == 'B' || c == 'D') &&
            parse_index_reg(right, &reg, (const char **)&end) && *trim(end) == '\0') {
            *postbyte = (uint8_t)(0x80u | (reg << 5) | indirect |
                                  (c == 'A' ? 0x06u : c == 'B' ? 0x05u : 0x0bu));
            *extra_size = 0;
            return 1;
        }
    }
    if (!parse_signed_number(left, &offset)) {
        return 0;
    }
    forced_size = indexed_forced_offset_size(left);
    if (toupper((unsigned char)right[0]) == 'P' && toupper((unsigned char)right[1]) == 'C' &&
        *trim(right + 2) == '\0') {
        if (forced_size == 2u && offset >= -32768 && offset <= 65535) {
            *postbyte = (uint8_t)(0x8du | indirect);
            *extra = (uint16_t)offset;
            *extra_size = 2;
        } else if ((forced_size == 0u || forced_size == 1u) && offset >= -128 && offset <= 255) {
            *postbyte = (uint8_t)(0x8cu | indirect);
            *extra = (uint16_t)((uint8_t)offset);
            *extra_size = 1;
        } else if (forced_size == 0u && offset >= -32768 && offset <= 65535) {
            *postbyte = (uint8_t)(0x8du | indirect);
            *extra = (uint16_t)offset;
            *extra_size = 2;
        } else {
            return 0;
        }
        return 1;
    }
    if (!parse_index_reg(right, &reg, (const char **)&end) || *trim(end) != '\0') {
        return 0;
    }
    if (!indirect && forced_size == 0u && offset >= -16 && offset <= 15) {
        *postbyte = (uint8_t)((reg << 5) | ((uint8_t)offset & 0x1fu));
        *extra_size = 0;
    } else if ((forced_size == 0u || forced_size == 1u) && offset >= -128 && offset <= 255) {
        *postbyte = (uint8_t)(0x88u | (reg << 5) | indirect);
        *extra = (uint16_t)((uint8_t)offset);
        *extra_size = 1;
    } else if ((forced_size == 0u || forced_size == 2u) && offset >= -32768 && offset <= 65535) {
        *postbyte = (uint8_t)(0x89u | (reg << 5) | indirect);
        *extra = (uint16_t)offset;
        *extra_size = 2;
    } else {
        return 0;
    }
    return 1;
}

static void format_indexed_operand(char *out, size_t out_size, uint8_t postbyte, const uint8_t *extra)
{
    unsigned reg = (postbyte >> 5) & 0x03u;
    int indirect = (postbyte & 0x80u) != 0 && (postbyte & 0x10u) != 0;
    unsigned mode = postbyte & 0x0fu;
    char body[64];

    if ((postbyte & 0x80u) == 0) {
        int offset = postbyte & 0x1f;
        if (offset & 0x10) {
            offset -= 0x20;
        }
        snprintf(body, sizeof(body), "%d,%s", offset, index_reg_name(reg));
    } else if (mode == 0x00) {
        snprintf(body, sizeof(body), ",%s+", index_reg_name(reg));
    } else if (mode == 0x01) {
        snprintf(body, sizeof(body), ",%s++", index_reg_name(reg));
    } else if (mode == 0x02) {
        snprintf(body, sizeof(body), ",-%s", index_reg_name(reg));
    } else if (mode == 0x03) {
        snprintf(body, sizeof(body), ",--%s", index_reg_name(reg));
    } else if (mode == 0x04) {
        snprintf(body, sizeof(body), ",%s", index_reg_name(reg));
    } else if (mode == 0x05 || mode == 0x06 || mode == 0x0b) {
        snprintf(body, sizeof(body), "%c,%s", mode == 0x05 ? 'B' : mode == 0x06 ? 'A' : 'D',
                 index_reg_name(reg));
    } else if (mode == 0x08) {
        snprintf(body, sizeof(body), "0x%02x,%s", extra[0], index_reg_name(reg));
    } else if (mode == 0x09) {
        snprintf(body, sizeof(body), "0x%04x,%s", be16(extra), index_reg_name(reg));
    } else if (mode == 0x0c && reg == 0u) {
        snprintf(body, sizeof(body), "0x%02x,PC", extra[0]);
    } else if (mode == 0x0d && reg == 0u) {
        snprintf(body, sizeof(body), "0x%04x,PC", be16(extra));
    } else {
        size_t extra_len = indexed_extra_size(postbyte);
        if (extra_len == 0) {
            snprintf(out, out_size, ",0x%02x", postbyte);
        } else if (extra_len == 1) {
            snprintf(out, out_size, ",0x%02x,0x%02x", postbyte, extra[0]);
        } else {
            snprintf(out, out_size, ",0x%02x,0x%04x", postbyte, be16(extra));
        }
        return;
    }

    if (indirect) {
        snprintf(out, out_size, "[%s]", body);
    } else {
        snprintf(out, out_size, "%s", body);
    }
}

static const char *postbyte_reg_name(unsigned code)
{
    switch (code) {
    case 0x0:
        return "D";
    case 0x1:
        return "X";
    case 0x2:
        return "Y";
    case 0x3:
        return "U";
    case 0x4:
        return "S";
    case 0x5:
        return "PC";
    case 0x8:
        return "A";
    case 0x9:
        return "B";
    case 0xa:
        return "CC";
    case 0xb:
        return "DP";
    default:
        return NULL;
    }
}

static int parse_postbyte_reg(const char *s, unsigned *code)
{
    char name[8];
    size_t i;

    s = trim((char *)s);
    for (i = 0; i + 1u < sizeof(name) && s[i]; i++) {
        name[i] = (char)toupper((unsigned char)s[i]);
    }
    name[i] = '\0';

    if (strcmp(name, "D") == 0) {
        *code = 0x0;
    } else if (strcmp(name, "X") == 0) {
        *code = 0x1;
    } else if (strcmp(name, "Y") == 0) {
        *code = 0x2;
    } else if (strcmp(name, "U") == 0) {
        *code = 0x3;
    } else if (strcmp(name, "S") == 0) {
        *code = 0x4;
    } else if (strcmp(name, "PC") == 0) {
        *code = 0x5;
    } else if (strcmp(name, "A") == 0) {
        *code = 0x8;
    } else if (strcmp(name, "B") == 0) {
        *code = 0x9;
    } else if (strcmp(name, "CC") == 0) {
        *code = 0xa;
    } else if (strcmp(name, "DP") == 0) {
        *code = 0xb;
    } else {
        return 0;
    }
    return 1;
}

static const char *stack_reg_name(const char *mnemonic, unsigned bit)
{
    static const char *base_names[] = {"CC", "A", "B", "DP", "X", "Y", "U", "PC"};

    if (bit == 6u && toupper((unsigned char)mnemonic[3]) == 'U') {
        return "S";
    }
    return bit < 8u ? base_names[bit] : NULL;
}

static int parse_stack_reg(const char *mnemonic, const char *s, unsigned *bit)
{
    unsigned i;

    for (i = 0; i < 8u; i++) {
        if (same_mnemonic(trim((char *)s), stack_reg_name(mnemonic, i))) {
            *bit = i;
            return 1;
        }
    }
    return 0;
}

static int parse_postbyte_pretty(const char *mnemonic, char *operand, uint8_t *postbyte)
{
    if (same_mnemonic(mnemonic, "EXG") || same_mnemonic(mnemonic, "TFR")) {
        char *comma = strchr(operand, ',');
        unsigned left;
        unsigned right;

        if (!comma) {
            return 0;
        }
        *comma = '\0';
        if (!parse_postbyte_reg(operand, &left) || !parse_postbyte_reg(comma + 1, &right)) {
            return 0;
        }
        *postbyte = (uint8_t)((left << 4) | right);
        return 1;
    }

    if (same_mnemonic(mnemonic, "PSHS") || same_mnemonic(mnemonic, "PULS") ||
        same_mnemonic(mnemonic, "PSHU") || same_mnemonic(mnemonic, "PULU")) {
        char *p = operand;
        uint8_t mask = 0;

        while (*trim(p)) {
            char *part = trim(p);
            char *comma = strchr(part, ',');
            unsigned bit;

            if (comma) {
                *comma = '\0';
                p = comma + 1;
            } else {
                p = part + strlen(part);
            }
            if (!parse_stack_reg(mnemonic, part, &bit)) {
                return 0;
            }
            mask |= (uint8_t)(1u << bit);
        }
        if (mask == 0) {
            return 0;
        }
        *postbyte = mask;
        return 1;
    }

    return 0;
}

static void append_reg_name(char *out, size_t out_size, int *first, const char *name)
{
    size_t len = strlen(out);

    snprintf(out + len, out_size > len ? out_size - len : 0, "%s%s", *first ? "" : ",", name);
    *first = 0;
}

static int format_postbyte_operand(const char *mnemonic, uint8_t postbyte, char *out, size_t out_size)
{
    if (same_mnemonic(mnemonic, "EXG") || same_mnemonic(mnemonic, "TFR")) {
        const char *left = postbyte_reg_name((postbyte >> 4) & 0x0fu);
        const char *right = postbyte_reg_name(postbyte & 0x0fu);

        if (!left || !right) {
            return 0;
        }
        snprintf(out, out_size, "%s,%s", left, right);
        return 1;
    }

    if (same_mnemonic(mnemonic, "PSHS") || same_mnemonic(mnemonic, "PULS") ||
        same_mnemonic(mnemonic, "PSHU") || same_mnemonic(mnemonic, "PULU")) {
        unsigned bit;
        int first = 1;

        if (postbyte == 0) {
            return 0;
        }
        out[0] = '\0';
        for (bit = 0; bit < 8u; bit++) {
            if (postbyte & (1u << bit)) {
                append_reg_name(out, out_size, &first, stack_reg_name(mnemonic, bit));
            }
        }
        return 1;
    }

    return 0;
}

static int postbyte_flow_stop(const char *mnemonic, uint8_t postbyte)
{
    return (same_mnemonic(mnemonic, "PULS") || same_mnemonic(mnemonic, "PULU")) &&
           (postbyte & 0x80u) != 0;
}

int cpu6809_assemble_line(const char *mnemonic, char *operand, uint32_t pc, int resolve_symbols,
                          Cpu6809ResolveFn resolve, Cpu6809EmitFn emit, void *ctx)
{
    size_t i;

    for (i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
        const Opcode *op = &opcodes[i];
        uint32_t value = 0;

        if (!same_mnemonic(mnemonic, op->mnemonic)) {
            continue;
        }
        switch (op->kind) {
        case OPER_NONE:
            if (*trim(operand) != '\0') {
                continue;
            }
            emit_opcode(op, emit, ctx);
            return 1;
        case OPER_IMM8:
            if (*trim(operand) != '#') {
                continue;
            }
            if (!parse_operand(ctx, resolve, resolve_symbols, skip_hash(operand), &value) ||
                value > 0xffu) {
                die("invalid %s immediate operand", mnemonic);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_IMM16:
            if (*trim(operand) != '#') {
                continue;
            }
            if (!parse_operand(ctx, resolve, resolve_symbols, skip_hash(operand), &value) ||
                value > 0xffffu) {
                die("invalid %s immediate operand", mnemonic);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)(value >> 8));
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_DIRECT:
            if (*trim(operand) != '<') {
                continue;
            }
            if (!parse_direct_operand(ctx, resolve, resolve_symbols, operand, &value) ||
                value > 0xffu) {
                die("invalid %s direct operand", mnemonic);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_EXT:
            if (*trim(operand) == '#') {
                continue;
            }
            if (!parse_operand(ctx, resolve, resolve_symbols, operand, &value) || value > 0xffffu) {
                die("invalid %s extended operand '%s'", mnemonic, operand);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)(value >> 8));
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_INDEXED:
            operand = trim(operand);
            {
                char pretty_operand[128];
                uint8_t postbyte;
                uint16_t extra_value = 0;
                size_t extra_size = 0;

                if (snprintf(pretty_operand, sizeof(pretty_operand), "%s", operand) <
                        (int)sizeof(pretty_operand) &&
                    parse_indexed_pretty(ctx, resolve, resolve_symbols, pretty_operand, &postbyte,
                                         &extra_value, &extra_size)) {
                    emit_opcode(op, emit, ctx);
                    emit(ctx, postbyte);
                    if (extra_size == 1u) {
                        emit(ctx, (uint8_t)extra_value);
                    } else if (extra_size == 2u) {
                        emit(ctx, (uint8_t)(extra_value >> 8));
                        emit(ctx, (uint8_t)extra_value);
                    }
                    return 1;
                }
            }
            if (*operand != ',') {
                continue;
            }
            operand = parse_comma_value(ctx, resolve, resolve_symbols, operand, &value);
            if (value > 0xffu) {
                die("invalid %s indexed postbyte operand", mnemonic);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)value);
            switch (indexed_extra_size((uint8_t)value)) {
            case 0:
                if (*trim(operand) != '\0') {
                    die("unexpected extra indexed operand for %s", mnemonic);
                }
                break;
            case 1:
                operand = parse_comma_value(ctx, resolve, resolve_symbols, operand, &value);
                if (value > 0xffu) {
                    die("invalid %s indexed 8-bit extra operand", mnemonic);
                }
                emit(ctx, (uint8_t)value);
                if (*trim(operand) != '\0') {
                    die("unexpected extra indexed operand for %s", mnemonic);
                }
                break;
            case 2:
                operand = parse_comma_value(ctx, resolve, resolve_symbols, operand, &value);
                if (value > 0xffffu) {
                    die("invalid %s indexed 16-bit extra operand", mnemonic);
                }
                emit(ctx, (uint8_t)(value >> 8));
                emit(ctx, (uint8_t)value);
                if (*trim(operand) != '\0') {
                    die("unexpected extra indexed operand for %s", mnemonic);
                }
                break;
            }
            return 1;
        case OPER_REL8:
            if (!parse_operand(ctx, resolve, resolve_symbols, operand, &value)) {
                die("invalid %s branch operand", mnemonic);
            }
            if (resolve_symbols) {
                int32_t delta = (int32_t)value - (int32_t)(pc + 2u);
                if (delta < -128 || delta > 127) {
                    die("%s target out of 8-bit branch range at 0x%04x: target 0x%04x delta %ld",
                        mnemonic, (unsigned)pc, (unsigned)value, (long)delta);
                }
                value = (uint8_t)delta;
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_REL16:
            if (!parse_operand(ctx, resolve, resolve_symbols, operand, &value)) {
                die("invalid %s branch operand", mnemonic);
            }
            if (resolve_symbols) {
                uint32_t next_pc = pc + (op->prefix ? 2u : 1u) + 2u;
                /* 6809 is a 16-bit CPU: the entire 64 KB address space is
                   reachable with a 16-bit relative offset.  Compute the
                   offset with 16-bit modular arithmetic so cross-bank branches
                   (e.g. paged bank → system bank) assemble correctly. */
                value = (uint32_t)(uint16_t)((uint32_t)value - next_pc);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)(value >> 8));
            emit(ctx, (uint8_t)value);
            return 1;
        case OPER_POSTBYTE:
            {
                char pretty_operand[128];
                uint8_t postbyte;

                if (snprintf(pretty_operand, sizeof(pretty_operand), "%s", trim(operand)) <
                        (int)sizeof(pretty_operand) &&
                    parse_postbyte_pretty(mnemonic, pretty_operand, &postbyte)) {
                    emit_opcode(op, emit, ctx);
                    emit(ctx, postbyte);
                    return 1;
                }
            }
            if (!parse_operand(ctx, resolve, resolve_symbols, operand, &value) || value > 0xffu) {
                die("invalid %s postbyte operand", mnemonic);
            }
            emit_opcode(op, emit, ctx);
            emit(ctx, (uint8_t)value);
            return 1;
        }
    }
    return 0;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void format_addr(char *out, size_t out_size, Cpu6809LabelFn label, void *label_ctx,
                        uint32_t addr)
{
    const char *name = label ? label(label_ctx, addr) : NULL;

    if (name) {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "0x%04x", (unsigned)addr & 0xffffu);
    }
}

static void format_direct_addr(char *out, size_t out_size, Cpu6809LabelFn label, void *label_ctx,
                               uint8_t addr)
{
    const char *name = label ? label(label_ctx, addr) : NULL;

    if (name) {
        snprintf(out, out_size, "<%s", name);
    } else {
        snprintf(out, out_size, "<0x%02x", addr);
    }
}

static void format_indexed_raw(char *out, size_t out_size, uint8_t postbyte, const uint8_t *extra,
                               Cpu6809LabelFn label, void *label_ctx)
{
    size_t extra_len = indexed_extra_size(postbyte);

    if (extra_len == 0) {
        snprintf(out, out_size, ",0x%02x", postbyte);
    } else if (extra_len == 1) {
        snprintf(out, out_size, ",0x%02x,0x%02x", postbyte, extra[0]);
    } else {
        char value[64];

        format_addr(value, sizeof(value), label, label_ctx, be16(extra));
        if (postbyte == 0x9fu) {
            snprintf(out, out_size, "[%s]", value);
        } else {
            snprintf(out, out_size, ",0x%02x,%s", postbyte, value);
        }
    }
}

Cpu6809InstrInfo cpu6809_disassemble_info_ex(const uint8_t *data, size_t len, uint32_t pc,
                                              char *out, size_t out_size, Cpu6809LabelFn label,
                                              void *label_ctx)
{
    size_t i;
    uint8_t prefix = 0;
    uint8_t opcode;
    Cpu6809InstrInfo info;

    info.size = 0;
    info.flags = 0;
    info.has_target = 0;
    info.target = 0;
    info.has_addr_ref = 0;
    info.addr_ref = 0;

    if (len == 0) {
        return info;
    }
    opcode = data[0];
    if (opcode == 0x10 || opcode == 0x11) {
        if (len < 2) {
            return info;
        }
        prefix = opcode;
        opcode = data[1];
    }

    for (i = 0; i < sizeof(opcodes) / sizeof(opcodes[0]); i++) {
        const Opcode *op = &opcodes[i];
        size_t op_len = op->prefix ? 2u : 1u;

        if (op->prefix != prefix || op->opcode != opcode) {
            continue;
        }
        info.flags = op->flags;
        switch (op->kind) {
        case OPER_NONE:
            snprintf(out, out_size, "%s", op->mnemonic);
            info.size = op_len;
            return info;
        case OPER_IMM8:
            if (len < op_len + 1u) {
                info.size = 0;
                return info;
            }
            snprintf(out, out_size, "%s #0x%02x", op->mnemonic, data[op_len]);
            info.size = op_len + 1u;
            return info;
        case OPER_IMM16:
            if (len < op_len + 2u) {
                info.size = 0;
                return info;
            }
            {
                char value[64];
                uint16_t imm = be16(data + op_len);

                /* Track as address reference so that labels at this address
                   accumulate incoming refs (e.g. LDX #TABLE_ADDR). The range
                   checks in collect_code_targets filter out non-ROM values. */
                info.has_addr_ref = 1;
                info.addr_ref     = imm;
                format_addr(value, sizeof(value), label, label_ctx, imm);
                snprintf(out, out_size, "%s #%s", op->mnemonic, value);
            }
            info.size = op_len + 2u;
            return info;
        case OPER_DIRECT:
            if (len < op_len + 1u) {
                info.size = 0;
                return info;
            }
            {
                char target[64];

                format_direct_addr(target, sizeof(target), label, label_ctx, data[op_len]);
                snprintf(out, out_size, "%s %s", op->mnemonic, target);
            }
            info.size = op_len + 1u;
            return info;
        case OPER_EXT:
            if (len < op_len + 2u) {
                info.size = 0;
                return info;
            }
            {
                char target[64];
                uint16_t addr = be16(data + op_len);
                info.has_addr_ref = 1;
                info.addr_ref = addr;
                if (op->flags & CPU6809_TARGET_CODE) {
                    info.has_target = 1;
                    info.target = addr;
                }
                format_addr(target, sizeof(target), label, label_ctx, addr);
                snprintf(out, out_size, "%s %s", op->mnemonic, target);
            }
            info.size = op_len + 2u;
            return info;
        case OPER_INDEXED:
            if (len < op_len + 1u) {
                info.size = 0;
                return info;
            }
            {
                size_t extra = indexed_extra_size(data[op_len]);

                if (len < op_len + 1u + extra) {
                    info.size = 0;
                    return info;
                }
                char indexed[80];
                uint8_t postbyte = data[op_len];

                if ((postbyte & 0x8fu) == 0x8fu) {
                    format_indexed_raw(indexed, sizeof(indexed), postbyte, data + op_len + 1u,
                                       label, label_ctx);
                } else {
                    format_indexed_operand(indexed, sizeof(indexed), postbyte,
                                           data + op_len + 1u);
                }
                snprintf(out, out_size, "%s %s", op->mnemonic, indexed);
                info.size = op_len + 1u + extra;
            }
            return info;
        case OPER_REL8:
            if (len < op_len + 1u) {
                info.size = 0;
                return info;
            }
            {
                char target[64];
                uint16_t addr = (uint16_t)(pc + op_len + 1u + (int8_t)data[op_len]);
                info.has_target = 1;
                info.target = addr;
                format_addr(target, sizeof(target), label, label_ctx, addr);
                snprintf(out, out_size, "%s %s", op->mnemonic, target);
            }
            info.size = op_len + 1u;
            return info;
        case OPER_REL16:
            if (len < op_len + 2u) {
                info.size = 0;
                return info;
            }
            {
                char target[64];
                uint16_t addr = (uint16_t)(pc + op_len + 2u + (int16_t)be16(data + op_len));
                info.has_target = 1;
                info.target = addr;
                format_addr(target, sizeof(target), label, label_ctx, addr);
                snprintf(out, out_size, "%s %s", op->mnemonic, target);
            }
            info.size = op_len + 2u;
            return info;
        case OPER_POSTBYTE:
            if (len < op_len + 1u) {
                info.size = 0;
                return info;
            }
            {
                char postbyte[80];

                if (format_postbyte_operand(op->mnemonic, data[op_len], postbyte,
                                            sizeof(postbyte))) {
                    snprintf(out, out_size, "%s %s", op->mnemonic, postbyte);
                } else {
                    snprintf(out, out_size, "%s 0x%02x", op->mnemonic, data[op_len]);
                }
                if (postbyte_flow_stop(op->mnemonic, data[op_len])) {
                    info.flags |= CPU6809_FLOW_STOP;
                }
            }
            info.size = op_len + 1u;
            return info;
        }
    }
    return info;
}

Cpu6809InstrInfo cpu6809_disassemble_info(const uint8_t *data, size_t len, uint32_t pc, char *out,
                                           size_t out_size)
{
    return cpu6809_disassemble_info_ex(data, len, pc, out, out_size, NULL, NULL);
}

size_t cpu6809_disassemble(const uint8_t *data, size_t len, uint32_t pc, char *out, size_t out_size)
{
    Cpu6809InstrInfo info = cpu6809_disassemble_info(data, len, pc, out, out_size);
    return info.size;
}
