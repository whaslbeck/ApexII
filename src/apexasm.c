#include "apex.h"
#include "cpu6809.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    uint32_t value;
    int has_far_bank;
    uint8_t far_bank;
} Symbol;

typedef struct {
    uint8_t *data;
    uint8_t *written;
    size_t size;
    size_t paged_size;
    int bank;
    int has_far_bank;
    uint8_t far_bank;
    int pass;
    uint32_t org;
    unsigned fill_seen[64];
    Symbol *symbols;
    size_t symbol_count;
    size_t symbol_cap;
} AsmState;

static int parse_value_or_symbol(const AsmState *st, const char *text, uint32_t *value);

static void usage(void)
{
    die("usage: apexasm <output-rom> <input-asm> [input-asm ...]");
}

static int starts_with_word(const char *s, const char *word)
{
    size_t n = strlen(word);
    return strncmp(s, word, n) == 0 && (s[n] == '\0' || isspace((unsigned char)s[n]));
}

static void strip_comment(char *line)
{
    int in_string = 0;
    int escaped = 0;
    char *p;

    for (p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (in_string && *p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && *p == ';') {
            *p = '\0';
            return;
        }
    }
}

static char *find_label_colon(char *line)
{
    int in_string = 0;
    int escaped = 0;
    char *p;

    for (p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (in_string && *p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && *p == ':') {
            return p;
        }
    }
    return NULL;
}

static char *find_unquoted_char(char *line, char needle)
{
    int in_string = 0;
    int escaped = 0;
    char *p;

    for (p = line; *p; p++) {
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (in_string && *p == '\\') {
            escaped = 1;
            continue;
        }
        if (*p == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && *p == needle) {
            return p;
        }
    }
    return NULL;
}

static char *xstrdup_local(const char *s)
{
    size_t len = strlen(s) + 1;
    char *copy = xmalloc(len);
    memcpy(copy, s, len);
    return copy;
}

static int valid_label_name(const char *s)
{
    if (!isalpha((unsigned char)*s) && *s != '_') {
        return 0;
    }
    for (s++; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_') {
            return 0;
        }
    }
    return 1;
}

static int reserved_label_name(const char *s)
{
    static const char *reserved[] = {
        "BANK_ID", "FILL_TO_BANK_END", "INLINE_BYTE", "INLINE_WORD", "INLINE_PTR",
        "INLINE_STRING_PTR", "INLINE_CODE_PTR", "INLINE_TABLE_PTR", "INLINE_FAR_CODE",
        "INLINE_FAR_STRING", "INLINE_FAR_PTR", "INLINE_FAR_TABLE",
        "INLINE_PTR_DMD_FULLFRAME", "INLINE_FAR_DMD_FULLFRAME",
        "INLINE_PTR_SPRITE", "INLINE_FAR_SPRITE",
        "TABLE_PTR", "TABLE_FAR_CODE", "TABLE_FAR_STRING", "TABLE_FAR_PTR",
        "TABLE_FAR_TABLE", "TABLE_PTR_DMD_FULLFRAME", "TABLE_FAR_DMD_FULLFRAME",
        "TABLE_PTR_SPRITE", "TABLE_FAR_SPRITE",
        "FAR_CODE", "FAR_STRING", "FAR_PTR", "FAR_TABLE", "FAR_DMD_FULLFRAME", "FAR_SPRITE",
        "STRING", "LDA", "LDB", "LDD", "LDX", "LDY", "LDU", "LDS", "STA", "STB",
        "STD", "STX", "STY", "STU", "STS", "JSR", "JMP", "RTS", "RTI", "BRA",
        "BSR", "PULS", "PULU", "PSHS", "PSHU", NULL
    };
    size_t i;

    for (i = 0; reserved[i]; i++) {
        if (strcmp(s, reserved[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_symbol_ex(AsmState *st, const char *name, uint32_t value, int has_far_bank,
                          uint8_t far_bank)
{
    size_t i;

    if (!valid_label_name(name)) {
        die("invalid label name '%s'", name);
    }
    if (reserved_label_name(name)) {
        die("label name '%s' collides with assembler syntax", name);
    }
    for (i = 0; i < st->symbol_count; i++) {
        if (strcmp(st->symbols[i].name, name) == 0) {
            if (st->symbols[i].value != value) {
                die("label '%s' is defined more than once", name);
            }
            if (has_far_bank && st->symbols[i].has_far_bank &&
                st->symbols[i].far_bank != far_bank) {
                die("label '%s' is defined with conflicting banks", name);
            }
            if (has_far_bank) {
                st->symbols[i].has_far_bank = 1;
                st->symbols[i].far_bank = far_bank;
            }
            return;
        }
    }
    if (st->symbol_count == st->symbol_cap) {
        size_t new_cap = st->symbol_cap == 0 ? 128 : st->symbol_cap * 2;
        Symbol *new_symbols = realloc(st->symbols, new_cap * sizeof(st->symbols[0]));
        if (!new_symbols) {
            die("out of memory");
        }
        st->symbols = new_symbols;
        st->symbol_cap = new_cap;
    }
    st->symbols[st->symbol_count].name = xstrdup_local(name);
    st->symbols[st->symbol_count].value = value;
    st->symbols[st->symbol_count].has_far_bank = has_far_bank;
    st->symbols[st->symbol_count].far_bank = far_bank;
    st->symbol_count++;
}

static void add_symbol(AsmState *st, const char *name, uint32_t value)
{
    add_symbol_ex(st, name, value, 0, 0);
}

static int lookup_symbol(const AsmState *st, const char *name, uint32_t *value)
{
    size_t i;

    for (i = 0; i < st->symbol_count; i++) {
        if (strcmp(st->symbols[i].name, name) == 0) {
            *value = st->symbols[i].value;
            return 1;
        }
    }
    return 0;
}

static int lookup_symbol_far_bank(const AsmState *st, const char *name, uint32_t *value,
                                  uint32_t *bank)
{
    size_t i;

    for (i = 0; i < st->symbol_count; i++) {
        if (strcmp(st->symbols[i].name, name) == 0 && st->symbols[i].has_far_bank) {
            *value = st->symbols[i].value;
            *bank = st->symbols[i].far_bank;
            return 1;
        }
    }
    return 0;
}

static void init_rom(AsmState *st, size_t size)
{
    if (size != 512u * 1024u && size != 1024u * 1024u) {
        die("unsupported .ROM_SIZE %lu", (unsigned long)size);
    }
    if (st->size != 0 && st->size != size) {
        die("conflicting .ROM_SIZE values");
    }
    st->size = size;
    st->paged_size = size - APEX_SYSTEM_SIZE;
    if (st->pass == 1 || st->data) {
        return;
    }
    st->data = xmalloc(size);
    st->written = xmalloc(size);
    memset(st->data, 0xff, size);
    memset(st->written, 0, size);
}

static size_t current_offset(const AsmState *st)
{
    if (st->size == 0) {
        die(".ROM_SIZE must appear before emitted bytes");
    }
    if (st->bank < 0) {
        die(".BANK must appear before emitted bytes");
    }
    if (st->bank == 0x100) {
        if (st->org < APEX_SYSTEM_ORG || st->org > 0x10000u) {
            die("system .ORG 0x%04x is outside 0x8000-0xffff", st->org);
        }
        return st->paged_size + (size_t)(st->org - APEX_SYSTEM_ORG);
    }
    if ((size_t)st->bank >= st->paged_size / APEX_BANK_SIZE) {
        die("bank 0x%02x is outside ROM size", st->bank);
    }
    if (st->org < APEX_PAGED_ORG || st->org > 0x8000u) {
        die("paged .ORG 0x%04x is outside 0x4000-0x7fff", st->org);
    }
    return (size_t)st->bank * APEX_BANK_SIZE + (size_t)(st->org - APEX_PAGED_ORG);
}

static uint32_t current_limit(const AsmState *st)
{
    return st->bank == 0x100 ? 0x10000u : 0x8000u;
}

static void emit_byte(AsmState *st, uint8_t value)
{
    size_t off = current_offset(st);
    if (off >= st->size || st->org >= current_limit(st)) {
        die("write past end of bank");
    }
    if (st->pass == 2) {
        st->data[off] = value;
        st->written[off] = 1;
    }
    st->org++;
}

static int resolve_for_cpu(void *ctx, const char *expr, uint32_t *value)
{
    return parse_value_or_symbol((const AsmState *)ctx, expr, value);
}

static void emit_for_cpu(void *ctx, uint8_t value)
{
    emit_byte((AsmState *)ctx, value);
}

static void parse_db(AsmState *st, char *args)
{
    char *p = args;

    while (*p) {
        char *start;
        char saved;
        uint32_t value;

        while (isspace((unsigned char)*p) || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p && *p != ',') {
            p++;
        }
        saved = *p;
        *p = '\0';
        start = trim(start);
        if (!parse_u32(start, &value) || value > 0xffu) {
            die("invalid .DB byte '%s'", start);
        }
        emit_byte(st, (uint8_t)value);
        if (saved == '\0') {
            break;
        }
        *p++ = saved;
    }
}

static int parse_value_or_symbol(const AsmState *st, const char *text, uint32_t *value)
{
    char expr[256];
    char *op;
    uint32_t left;
    uint32_t right;

    if (parse_u32(text, value)) {
        return 1;
    }
    if (snprintf(expr, sizeof(expr), "%s", text) < (int)sizeof(expr)) {
        op = find_unquoted_char(expr, '+');
        if (!op) {
            op = find_unquoted_char(expr, '-');
        }
        if (op && op != expr) {
            char operation = *op;

            *op = '\0';
            if (parse_value_or_symbol(st, trim(expr), &left) &&
                parse_value_or_symbol(st, trim(op + 1), &right)) {
                *value = operation == '+' ? left + right : left - right;
                return 1;
            }
        }
    }
    if (strlen(text) == 9 && text[0] == 'B' && text[3] == '_' && text[4] == 'A') {
        uint32_t bank;
        char addr_text[7];
        char bank_text[5];
        bank_text[0] = '0';
        bank_text[1] = 'x';
        bank_text[2] = text[1];
        bank_text[3] = text[2];
        bank_text[4] = '\0';
        addr_text[0] = '0';
        addr_text[1] = 'x';
        memcpy(addr_text + 2, text + 5, 4);
        addr_text[6] = '\0';
        if (!parse_u32(addr_text, value) || !parse_u32(bank_text, &bank)) {
            return 0;
        }
        (void)bank;
        return 1;
    }
    return lookup_symbol(st, text, value);
}

static int parse_generated_bank_label(const char *text, uint32_t *addr, uint32_t *bank)
{
    char bank_text[5];
    char addr_text[7];

    if (strlen(text) != 9 || text[0] != 'B' || text[3] != '_' || text[4] != 'A') {
        return 0;
    }
    bank_text[0] = '0';
    bank_text[1] = 'x';
    bank_text[2] = text[1];
    bank_text[3] = text[2];
    bank_text[4] = '\0';
    addr_text[0] = '0';
    addr_text[1] = 'x';
    memcpy(addr_text + 2, text + 5, 4);
    addr_text[6] = '\0';
    return parse_u32(addr_text, addr) && parse_u32(bank_text, bank);
}

static void parse_dw(AsmState *st, char *args)
{
    char *p = args;

    while (*p) {
        char *start;
        char saved;
        uint32_t value;

        while (isspace((unsigned char)*p) || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p && *p != ',') {
            p++;
        }
        saved = *p;
        *p = '\0';
        start = trim(start);
        if (st->pass == 1) {
            value = 0;
        } else if (!parse_value_or_symbol(st, start, &value) || value > 0xffffu) {
            die("invalid .DW word '%s'", start);
        }
        emit_byte(st, (uint8_t)(value >> 8));
        emit_byte(st, (uint8_t)value);
        if (saved == '\0') {
            break;
        }
        *p++ = saved;
    }
}

static void parse_table_ptr(AsmState *st, char *args)
{
    uint32_t value;

    args = trim(args);
    if (st->pass == 1) {
        value = 0;
    } else if (!parse_value_or_symbol(st, args, &value) || value > 0xffffu) {
        die("invalid TABLE_PTR '%s'", args);
    }
    emit_byte(st, (uint8_t)(value >> 8));
    emit_byte(st, (uint8_t)value);
}

static void parse_string(AsmState *st, char *args)
{
    char *p = trim(args);

    if (*p != '"') {
        die("invalid STRING '%s'", args);
    }
    p++;
    while (*p) {
        unsigned char ch = 0;

        if (*p == '"') {
            p++;
            if (*trim(p) != '\0') {
                die("unexpected trailing text after STRING");
            }
            emit_byte(st, 0x00);
            return;
        }
        if (*p == '\\') {
            p++;
            if (*p == '"' || *p == '\\') {
                ch = (unsigned char)*p++;
            } else if (*p == 'n') {
                ch = 0x0au; p++;
            } else if (*p == 'a') {
                ch = 0x07u; p++;
            } else {
                die("invalid STRING escape");
            }
        } else {
            ch = (unsigned char)*p++;
        }
        if (ch != 0x0au && ch != 0x07u && (ch < 0x20u || ch > 0x7fu)) {
            die("STRING byte out of supported ASCII range");
        }
        emit_byte(st, ch);
    }
    die("unterminated STRING");
}

static void parse_string_fixed(AsmState *st, char *args)
{
    char *p = trim(args);

    if (*p != '"') {
        die("invalid STRING_FIXED '%s'", args);
    }
    p++;
    while (*p) {
        unsigned char ch = 0;

        if (*p == '"') {
            p++;
            if (*trim(p) != '\0') {
                die("unexpected trailing text after STRING_FIXED");
            }
            return;
        }
        if (*p == '\\') {
            p++;
            if (*p == '"' || *p == '\\') {
                ch = (unsigned char)*p++;
            } else if (*p == 'n') {
                ch = 0x0au; p++;
            } else if (*p == 'a') {
                ch = 0x07u; p++;
            } else {
                die("invalid STRING_FIXED escape");
            }
        } else {
            ch = (unsigned char)*p++;
        }
        if (ch != 0x0au && ch != 0x07u && (ch < 0x20u || ch > 0x7fu)) {
            die("STRING_FIXED byte out of supported ASCII range");
        }
        emit_byte(st, ch);
    }
    die("unterminated STRING_FIXED");
}

static void parse_string_lp(AsmState *st, char *args)
{
    char *p = trim(args);
    uint8_t buf[256];
    size_t len = 0;

    if (*p != '"') {
        die("invalid STRING_LP '%s'", args);
    }
    p++;
    while (*p) {
        unsigned char ch = 0;

        if (*p == '"') {
            p++;
            if (*trim(p) != '\0') {
                die("unexpected trailing text after STRING_LP");
            }
            emit_byte(st, (uint8_t)len);
            { size_t i; for (i = 0; i < len; i++) emit_byte(st, buf[i]); }
            return;
        }
        if (*p == '\\') {
            p++;
            if (*p == '"' || *p == '\\') {
                ch = (unsigned char)*p++;
            } else if (*p == 'n') {
                ch = 0x0au; p++;
            } else if (*p == 'a') {
                ch = 0x07u; p++;
            } else {
                die("invalid STRING_LP escape");
            }
        } else {
            ch = (unsigned char)*p++;
        }
        if (ch != 0x0au && ch != 0x07u && (ch < 0x20u || ch > 0x7fu)) {
            die("STRING_LP byte out of supported ASCII range");
        }
        if (len >= 255u) {
            die("STRING_LP too long (max 255 bytes)");
        }
        buf[len++] = ch;
    }
    die("unterminated STRING_LP");
}

static void parse_inline_far_code(AsmState *st, char *args)
{
    char *first;
    char *second;
    uint32_t addr;
    uint32_t bank;

    first = trim(args);
    second = strchr(first, ',');
    if (second) {
        *second = '\0';
        second = trim(second + 1);
    }
    first = trim(first);
    if (second) {
        if (st->pass == 1) {
            addr = 0;
            bank = 0;
        } else if (!parse_value_or_symbol(st, first, &addr) ||
                   !parse_value_or_symbol(st, second, &bank)) {
            die("invalid INLINE_FAR_CODE '%s, %s'", first, second);
        }
    } else if (parse_generated_bank_label(first, &addr, &bank)) {
        /* parsed generated far label */
    } else if (lookup_symbol_far_bank(st, first, &addr, &bank)) {
        /* parsed user far label */
    } else {
        if (st->pass == 1) {
            addr = 0;
            bank = 0;
        } else {
            die("invalid INLINE_FAR_CODE '%s'", first);
        }
    }
    if (addr > 0xffffu || bank > 0xffu) {
        die("INLINE_FAR_CODE operand out of range");
    }
    emit_byte(st, (uint8_t)(addr >> 8));
    emit_byte(st, (uint8_t)addr);
    emit_byte(st, (uint8_t)bank);
}

static void parse_table_far_code(AsmState *st, char *args)
{
    parse_inline_far_code(st, args);
}

static void fill_to_bank_end(AsmState *st)
{
    unsigned index;
    uint32_t limit = current_limit(st);

    if (st->bank < 0) {
        die("FILL_TO_BANK_END requires .BANK");
    }
    index = st->bank == 0x100 ? 63u : (unsigned)st->bank;
    if (st->pass == 2) {
        if (index >= sizeof(st->fill_seen) / sizeof(st->fill_seen[0])) {
            die("bank index too large for fill tracking");
        }
        if (st->fill_seen[index]) {
            die("FILL_TO_BANK_END used more than once for bank");
        }
        st->fill_seen[index] = 1;
    }
    while (st->org < limit) {
        emit_byte(st, 0xff);
    }
}

static int parse_equate(AsmState *st, char *line)
{
    char *eq = find_unquoted_char(line, '=');
    char *name;
    char *expr;
    uint32_t value;

    if (!eq) {
        return 0;
    }
    *eq = '\0';
    name = trim(line);
    expr = trim(eq + 1);
    if (!parse_value_or_symbol(st, expr, &value)) {
        if (st->pass == 1) {
            return 1;
        }
        die("invalid equate '%s = %s'", name, expr);
    }
    add_symbol(st, name, value);
    return 1;
}

static void parse_line(AsmState *st, char *line)
{
    char *s;
    char *colon;
    char *space;
    uint32_t value;

    strip_comment(line);
    s = trim(line);
    if (*s == '\0') {
        return;
    }

    colon = find_label_colon(s);
    if (colon) {
        *colon = '\0';
        add_symbol_ex(st, trim(s), st->org, st->has_far_bank, st->far_bank);
        s = trim(colon + 1);
        if (*s == '\0') {
            return;
        }
    } else {
        if (parse_equate(st, s)) {
            return;
        }
    }

    if (starts_with_word(s, ".ROM_SIZE")) {
        s = trim(s + strlen(".ROM_SIZE"));
        if (!parse_u32(s, &value)) {
            die("invalid .ROM_SIZE");
        }
        init_rom(st, value);
    } else if (starts_with_word(s, ".BANK")) {
        s = trim(s + strlen(".BANK"));
        if (strcmp(s, "SYSTEM") == 0) {
            st->bank = 0x100;
            st->has_far_bank = 1;
            st->far_bank = 0xffu;
        } else {
            if (!parse_u32(s, &value) || value > 0xffu) {
                die("invalid .BANK '%s'", s);
            }
            st->bank = (int)value;
            /* A user label's far bank is the canonical (computed) bank id for
               this page, mirroring the disassembler: base = 0x3e - paged_pages
               (0x20 for 512 KB, 0x00 for 1 MB).  The BANK_ID byte is just the
               emitted marker and no longer defines bank identity. */
            if (st->paged_size > 0) {
                size_t banks = st->paged_size / APEX_BANK_SIZE;
                st->has_far_bank = 1;
                st->far_bank = (uint8_t)((0x3eu - banks) + value);
            } else {
                st->has_far_bank = 0;
            }
        }
    } else if (starts_with_word(s, ".ORG")) {
        s = trim(s + strlen(".ORG"));
        if (!parse_u32(s, &value) || value > 0x10000u) {
            die("invalid .ORG '%s'", s);
        }
        st->org = value;
    } else if (starts_with_word(s, "BANK_ID")) {
        s = trim(s + strlen("BANK_ID"));
        if (!parse_u32(s, &value) || value > 0xffu) {
            die("invalid BANK_ID '%s'", s);
        }
        /* BANK_ID only emits the marker byte; bank identity comes from .BANK. */
        emit_byte(st, (uint8_t)value);
    } else if (starts_with_word(s, "INLINE_BYTE")) {
        s = trim(s + strlen("INLINE_BYTE"));
        if (!parse_u32(s, &value) || value > 0xffu) {
            die("invalid INLINE_BYTE '%s'", s);
        }
        emit_byte(st, (uint8_t)value);
    } else if (starts_with_word(s, "INLINE_WORD")) {
        s = trim(s + strlen("INLINE_WORD"));
        if (!parse_u32(s, &value) || value > 0xffffu) {
            die("invalid INLINE_WORD '%s'", s);
        }
        emit_byte(st, (uint8_t)(value >> 8));
        emit_byte(st, (uint8_t)value);
    } else if (starts_with_word(s, "INLINE_STRING_PTR")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_STRING_PTR")));
    } else if (starts_with_word(s, "INLINE_CODE_PTR")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_CODE_PTR")));
    } else if (starts_with_word(s, "INLINE_TABLE_PTR")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_TABLE_PTR")));
    } else if (starts_with_word(s, "INLINE_PTR_DMD_FULLFRAME")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_PTR_DMD_FULLFRAME")));
    } else if (starts_with_word(s, "INLINE_PTR_SPRITE")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_PTR_SPRITE")));
    } else if (starts_with_word(s, "INLINE_PTR")) {
        parse_table_ptr(st, trim(s + strlen("INLINE_PTR")));
    } else if (starts_with_word(s, "INLINE_FAR_CODE")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_CODE")));
    } else if (starts_with_word(s, "INLINE_FAR_STRING")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_STRING")));
    } else if (starts_with_word(s, "INLINE_FAR_TABLE")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_TABLE")));
    } else if (starts_with_word(s, "INLINE_FAR_DMD_FULLFRAME")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_DMD_FULLFRAME")));
    } else if (starts_with_word(s, "INLINE_FAR_SPRITE")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_SPRITE")));
    } else if (starts_with_word(s, "INLINE_FAR_PTR")) {
        parse_inline_far_code(st, trim(s + strlen("INLINE_FAR_PTR")));
    } else if (starts_with_word(s, "FAR_STRING")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_STRING")));
    } else if (starts_with_word(s, "FAR_TABLE")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_TABLE")));
    } else if (starts_with_word(s, "FAR_PTR")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_PTR")));
    } else if (starts_with_word(s, "FAR_CODE")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_CODE")));
    } else if (starts_with_word(s, "FAR_DMD_FULLFRAME")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_DMD_FULLFRAME")));
    } else if (starts_with_word(s, "FAR_SPRITE")) {
        parse_inline_far_code(st, trim(s + strlen("FAR_SPRITE")));
    } else if (starts_with_word(s, "TABLE_PTR_SPRITE")) {
        parse_table_ptr(st, trim(s + strlen("TABLE_PTR_SPRITE")));
    } else if (starts_with_word(s, "TABLE_PTR")) {
        parse_table_ptr(st, trim(s + strlen("TABLE_PTR")));
    } else if (starts_with_word(s, "TABLE_PTR_DMD_FULLFRAME")) {
        parse_table_ptr(st, trim(s + strlen("TABLE_PTR_DMD_FULLFRAME")));
    } else if (starts_with_word(s, "TABLE_FAR_STRING")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_STRING")));
    } else if (starts_with_word(s, "TABLE_FAR_TABLE")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_TABLE")));
    } else if (starts_with_word(s, "TABLE_FAR_DMD_FULLFRAME")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_DMD_FULLFRAME")));
    } else if (starts_with_word(s, "TABLE_FAR_SPRITE")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_SPRITE")));
    } else if (starts_with_word(s, "TABLE_FAR_PTR")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_PTR")));
    } else if (starts_with_word(s, "TABLE_FAR_CODE")) {
        parse_table_far_code(st, trim(s + strlen("TABLE_FAR_CODE")));
    } else if (starts_with_word(s, "STRING_FIXED")) {
        parse_string_fixed(st, trim(s + strlen("STRING_FIXED")));
    } else if (starts_with_word(s, "STRING_LP")) {
        parse_string_lp(st, trim(s + strlen("STRING_LP")));
    } else if (starts_with_word(s, "STRING")) {
        parse_string(st, trim(s + strlen("STRING")));
    } else if (starts_with_word(s, ".DB")) {
        parse_db(st, trim(s + strlen(".DB")));
    } else if (starts_with_word(s, ".DW")) {
        parse_dw(st, trim(s + strlen(".DW")));
    } else if (strcmp(s, "FILL_TO_BANK_END") == 0) {
        fill_to_bank_end(st);
    } else {
        char saved;
        space = s;
        while (*space && !isspace((unsigned char)*space)) {
            space++;
        }
        saved = *space;
        *space = '\0';
        if (!cpu6809_assemble_line(s, saved ? trim(space + 1) : space, st->org, st->pass == 2,
                                   resolve_for_cpu, emit_for_cpu, st)) {
            die("unsupported assembly line: %s%s%s", s, saved ? " " : "",
                saved ? trim(space + 1) : "");
        }
        *space = saved;
    }
}

static void assemble_file(AsmState *st, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    unsigned long lineno = 0;

    if (!f) {
        die("failed to open %s", path);
    }
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        /* If fgets filled the buffer without reaching a newline the logical
           line is longer than our buffer.  Consume and discard the rest of
           the physical line before attempting to parse what we have: the
           partial remainder would otherwise be fed back as the next line
           without its leading ';', causing a spurious parse error on long
           "; referenced_by ..." xref comments. */
        if (line[0] != '\0' && strchr(line, '\n') == NULL && !feof(f)) {
            /* We already have a valid comment prefix in `line`; parse it. */
            parse_line(st, line);
            /* Drain the rest of the physical line without parsing it. */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
            lineno++; /* counts as one logical source line */
        } else {
            parse_line(st, line);
        }
    }
    if (ferror(f)) {
        die("failed to read %s", path);
    }
    fclose(f);
    (void)lineno;
}

static void reset_location(AsmState *st, int pass)
{
    st->bank = -1;
    st->has_far_bank = 0;
    st->org = 0;
    st->pass = pass;
    memset(st->fill_seen, 0, sizeof(st->fill_seen));
}

int main(int argc, char **argv)
{
    AsmState st;
    int i;

    if (argc < 3) {
        usage();
    }
    memset(&st, 0, sizeof(st));

    reset_location(&st, 1);
    for (i = 2; i < argc; i++) {
        assemble_file(&st, argv[i]);
    }
    if (st.size == 0) {
        die("no .ROM_SIZE was assembled");
    }

    reset_location(&st, 2);
    for (i = 2; i < argc; i++) {
        assemble_file(&st, argv[i]);
    }
    if (!st.data) {
        die("second pass did not allocate ROM");
    }
    write_file(argv[1], st.data, st.size);
    for (i = 0; (size_t)i < st.symbol_count; i++) {
        free(st.symbols[i].name);
    }
    free(st.symbols);
    free(st.data);
    free(st.written);
    return 0;
}
