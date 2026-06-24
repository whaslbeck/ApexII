#ifndef CPU6809_H
#define CPU6809_H

#include <stddef.h>
#include <stdint.h>

typedef int (*Cpu6809ResolveFn)(void *ctx, const char *expr, uint32_t *value);
typedef void (*Cpu6809EmitFn)(void *ctx, uint8_t value);
typedef const char *(*Cpu6809LabelFn)(void *ctx, uint32_t addr);

#define CPU6809_FLOW_STOP 0x01u
#define CPU6809_TARGET_CODE 0x02u
#define CPU6809_CALL 0x04u   /* subroutine call (JSR/BSR/LBSR): pushes a return
                                address, so an inline payload may follow it */

typedef struct {
    size_t size;
    unsigned flags;
    int has_target;
    uint32_t target;
    int has_addr_ref;
    uint32_t addr_ref;
} Cpu6809InstrInfo;

int cpu6809_assemble_line(const char *mnemonic, char *operand, uint32_t pc, int resolve_symbols,
                          Cpu6809ResolveFn resolve, Cpu6809EmitFn emit, void *ctx);
size_t cpu6809_disassemble(const uint8_t *data, size_t len, uint32_t pc, char *out, size_t out_size);
Cpu6809InstrInfo cpu6809_disassemble_info(const uint8_t *data, size_t len, uint32_t pc, char *out,
                                           size_t out_size);
Cpu6809InstrInfo cpu6809_disassemble_info_ex(const uint8_t *data, size_t len, uint32_t pc,
                                              char *out, size_t out_size, Cpu6809LabelFn label,
                                              void *label_ctx);

#endif
