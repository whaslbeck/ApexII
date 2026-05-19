#include "apex.h"
#include "apex_analysis.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* forward declaration — defined later; also exposed in header for apexdis.c */

size_t last_non_ff(const uint8_t *data, size_t len)
{
    size_t n = len;

    while (n > 0 && data[n - 1] == 0xff) {
        n--;
    }
    return n;
}

uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int generated_label_name(const char *name)
{
    return name && strlen(name) == 9 && name[0] == 'B' && name[3] == '_' && name[4] == 'A';
}

int generated_string_label_name(const char *name)
{
    return name && strlen(name) > 16 && name[0] == 'B' && name[3] == '_' && name[4] == 'A' &&
           strncmp(name + 9, "_STRING_", 8) == 0;
}

int generated_any_label_name(const char *name)
{
    return generated_label_name(name) || generated_string_label_name(name);
}

void collect_vectors(const uint8_t *system, VectorInfo *vectors, size_t count)
{
    static const char *vector_names[] = {
        "VECTOR_SWI3", "VECTOR_SWI2", "VECTOR_FIRQ", "VECTOR_IRQ",
        "VECTOR_SWI", "VECTOR_NMI", "VECTOR_RESET"
    };
    static const char *entry_names[] = {
        "ENTRY_SWI3", "ENTRY_SWI2", "ENTRY_FIRQ", "ENTRY_IRQ",
        "ENTRY_SWI", "ENTRY_NMI", "ENTRY_RESET"
    };
    size_t i;

    for (i = 0; i < count; i++) {
        vectors[i].vector_name = vector_names[i];
        vectors[i].entry_name = entry_names[i];
        vectors[i].vector_addr = 0xfff2u + (uint32_t)(i * 2u);
        vectors[i].target_addr = read_be16(system + (vectors[i].vector_addr - APEX_SYSTEM_ORG));
    }
}

Label *add_label(LabelSet *set, uint32_t addr, const char *name, int is_code)
{
    size_t i;
    Label *first_at_addr = NULL;

    for (i = 0; i < set->count; i++) {
        if (set->items[i].addr == addr) {
            if (!first_at_addr) {
                first_at_addr = &set->items[i];
            }
            if (!name || strcmp(set->items[i].name, name) == 0) {
                if (is_code && !set->items[i].is_data) {
                    set->items[i].is_code = 1;
                } else if (is_code && set->items[i].is_data) {
                    set->items[i].is_conflict = 1;
                }
                return &set->items[i];
            }
            if (is_code && !set->items[i].is_data) {
                set->items[i].is_code = 1;
            } else if (is_code && set->items[i].is_data) {
                set->items[i].is_conflict = 1;
            }
        }
    }
    if (first_at_addr && (!name || generated_any_label_name(name))) {
        return first_at_addr;
    }
    if (set->count == set->cap) {
        size_t new_cap = set->cap == 0 ? 32 : set->cap * 2;
        Label *new_items = realloc(set->items, new_cap * sizeof(set->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        set->items = new_items;
        set->cap = new_cap;
    }
    set->items[set->count].addr = addr;
    set->items[set->count].name = name;
    set->items[set->count].is_code = is_code;
    set->items[set->count].is_data = 0;
    set->items[set->count].is_string = 0;
    set->items[set->count].is_conflict = 0;
    set->items[set->count].scanned = 0;
    set->items[set->count].is_explicit_entry = 0;
    set->items[set->count].reached_by_flow = 0;
    set->items[set->count].explain = NULL;
    set->items[set->count].kind_explain = NULL;
    set->sorted = 0;
    set->count++;
    return &set->items[set->count - 1];
}

void explain_label(Label *label, const char *source)
{
    if (label && source && !label->explain) {
        label->explain = source;
    }
}

void explain_label_kind(Label *label, const char *source)
{
    if (label && source) {
        label->kind_explain = source;
    }
}

void mark_label_data(Label *label)
{
    if (!label) {
        return;
    }
    if (label->is_code) {
        label->is_conflict = 1;
    }
    label->is_data = 1;
    label->is_code = 0;
    label->scanned = 0;
}

void add_reference(ReferenceSet *refs, uint8_t bank, uint32_t addr, uint8_t source_bank,
                   uint32_t source_addr, const char *kind, const char *source)
{
    if (!refs || !source) {
        return;
    }
    if (refs->count == refs->cap) {
        size_t new_cap = refs->cap == 0 ? 64 : refs->cap * 2;
        Reference *new_items = realloc(refs->items, new_cap * sizeof(refs->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        refs->items = new_items;
        refs->cap = new_cap;
    }
    refs->items[refs->count].bank = bank;
    refs->items[refs->count].addr = addr;
    refs->items[refs->count].source_bank = source_bank;
    refs->items[refs->count].source_addr = source_addr;
    refs->items[refs->count].kind = kind;
    refs->items[refs->count].source = source;
    refs->items[refs->count].row_index = -1;
    refs->items[refs->count].row_cpu_addr = 0;
    refs->sorted = 0;
    refs->count++;
}

void add_table_row_reference(ReferenceSet *refs, uint8_t bank, uint32_t addr,
                              uint8_t source_bank, uint32_t source_addr, const char *source,
                              int row_index, uint32_t row_cpu_addr)
{
    if (!refs || !source) {
        return;
    }
    if (refs->count == refs->cap) {
        size_t new_cap = refs->cap == 0 ? 64 : refs->cap * 2;
        Reference *new_items = realloc(refs->items, new_cap * sizeof(refs->items[0]));

        if (!new_items) {
            die("out of memory");
        }
        refs->items = new_items;
        refs->cap = new_cap;
    }
    refs->items[refs->count].bank = bank;
    refs->items[refs->count].addr = addr;
    refs->items[refs->count].source_bank = source_bank;
    refs->items[refs->count].source_addr = source_addr;
    refs->items[refs->count].kind = "table";
    refs->items[refs->count].source = source;
    refs->items[refs->count].row_index = row_index;
    refs->items[refs->count].row_cpu_addr = row_cpu_addr;
    refs->sorted = 0;
    refs->count++;
}

size_t remove_references_from_source_range(ReferenceSet *refs, uint8_t source_bank,
                                           uint32_t source_start, uint32_t source_end)
{
    size_t read_idx;
    size_t write_idx = 0;
    size_t removed = 0;

    if (!refs) {
        return 0;
    }
    for (read_idx = 0; read_idx < refs->count; read_idx++) {
        Reference *ref = &refs->items[read_idx];

        if (ref->source_bank == source_bank && ref->source_addr >= source_start &&
            ref->source_addr <= source_end) {
            removed++;
            continue;
        }
        if (write_idx != read_idx) {
            refs->items[write_idx] = *ref;
        }
        write_idx++;
    }
    refs->count = write_idx;
    return removed;
}

static int label_has_inbound_ref(uint8_t bank, uint32_t addr, const ReferenceSet *refs)
{
    size_t i;

    if (!refs) {
        return 0;
    }
    for (i = 0; i < refs->count; i++) {
        if (refs->items[i].bank == bank && refs->items[i].addr == addr) {
            return 1;
        }
    }
    return 0;
}

static int label_is_pinned(const Label *label)
{
    const char *source;

    if (!label) {
        return 1;
    }
    if (!generated_any_label_name(label->name)) {
        return 1;
    }
    source = label->kind_explain ? label->kind_explain : label->explain;
    if (!source) {
        return 0;
    }
    return strcmp(source, "config_entry") == 0 || strcmp(source, "config_data") == 0 ||
           strcmp(source, "config_table") == 0 || strcmp(source, "config_label") == 0 ||
           strcmp(source, "config_label_entry") == 0 || strcmp(source, "vector_entry") == 0 ||
           strcmp(source, "vector_table") == 0;
}

size_t prune_unreferenced_generated_labels(LabelSet *labels, uint8_t bank,
                                           const ReferenceSet *refs)
{
    size_t read_idx;
    size_t write_idx = 0;
    size_t removed = 0;

    if (!labels) {
        return 0;
    }
    for (read_idx = 0; read_idx < labels->count; read_idx++) {
        Label *label = &labels->items[read_idx];

        if (!label_is_pinned(label) && !label_has_inbound_ref(bank, label->addr, refs)) {
            removed++;
            continue;
        }
        if (write_idx != read_idx) {
            labels->items[write_idx] = *label;
        }
        write_idx++;
    }
    labels->count = write_idx;
    return removed;
}

const char *make_generated_label(uint32_t addr)
{
    char *name = xmalloc(16);

    snprintf(name, 16, "Bff_A%04x", (unsigned)addr & 0xffffu);
    return name;
}

const InlineSignature *inline_signature_for(const InlineSignatures *sigs, uint8_t bank,
                                            uint32_t addr)
{
    size_t lo = 0, hi = sigs->count;
    const InlineSignature *unbanked = NULL;
    uint8_t callee_bank = (addr >= APEX_SYSTEM_ORG) ? 0xffu : bank;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (sigs->items[mid].addr < addr) {
            lo = mid + 1;
        } else if (sigs->items[mid].addr > addr) {
            hi = mid;
        } else {
            lo = mid;
            break;
        }
    }
    while (lo < sigs->count && sigs->items[lo].addr == addr) {
        if (sigs->items[lo].has_bank && sigs->items[lo].bank == callee_bank) {
            return &sigs->items[lo];
        }
        if (!sigs->items[lo].has_bank) {
            unbanked = &sigs->items[lo];
        }
        lo++;
    }
    return unbanked;
}

const char *make_bank_label(uint8_t bank, uint16_t addr)
{
    char *name = xmalloc(16);

    snprintf(name, 16, "B%02x_A%04x", bank, addr);
    return name;
}

const char *make_string_label(const char *base, const uint8_t *data, size_t len)
{
    size_t text_len = len > 0 ? len - 1u : 0;
    size_t suffix_cap = text_len == 0 ? 5u : text_len;
    size_t cap = strlen(base) + strlen("_STRING_") + suffix_cap + 1u;
    char *name = xmalloc(cap);
    size_t out = 0;
    size_t i;

    out += (size_t)snprintf(name + out, cap - out, "%s_STRING_", base);
    if (text_len == 0) {
        snprintf(name + out, cap - out, "EMPTY");
        return name;
    }
    for (i = 0; i < text_len && out + 1u < cap; i++) {
        unsigned char ch = data[i];

        name[out++] = isalnum(ch) ? (char)toupper(ch) : '_';
    }
    name[out] = '\0';
    return name;
}

void apply_string_content_labels(LabelSet *labels, const uint8_t *data, size_t used,
                                 uint32_t base_addr)
{
    size_t i;

    for (i = 0; i < labels->count; i++) {
        Label *label = &labels->items[i];
        size_t pos;
        size_t string_len;

        if (!label->is_string || !generated_label_name(label->name) || label->addr < base_addr ||
            label->addr >= base_addr + used) {
            continue;
        }
        pos = (size_t)(label->addr - base_addr);
        string_len = valid_string_len(data + pos, used - pos);
        if (string_len == 0) {
            continue;
        }
        label->name = make_string_label(label->name, data + pos, string_len);
    }
}

int bank_index_for_id(const uint8_t *paged_rom, size_t banks, uint8_t bank_id)
{
    size_t i;

    for (i = 0; i < banks; i++) {
        if (paged_rom[i * APEX_BANK_SIZE] == bank_id) {
            return (int)i;
        }
    }
    return -1;
}

int bank_index_for_far_ref(const uint8_t *paged_rom, size_t banks, uint8_t bank)
{
    int bank_index = bank_index_for_id(paged_rom, banks, bank);

    if (bank_index >= 0) {
        return bank_index;
    }
    if (bank < banks) {
        return bank;
    }
    return -1;
}

uint8_t bank_id_for_index(const uint8_t *paged_rom, int bank_index)
{
    return paged_rom[(size_t)bank_index * APEX_BANK_SIZE];
}

void validate_config_classification(const ConfigEntries *entries, const TableDefs *tables,
                                    const DataRanges *data_ranges)
{
    size_t i;
    size_t j;

    for (i = 0; i < entries->count; i++) {
        for (j = 0; j < data_ranges->count; j++) {
            if (entries->items[i].has_bank == 1 &&
                entries->items[i].bank == data_ranges->items[j].bank &&
                entries->items[i].addr == data_ranges->items[j].addr) {
                die("config classifies B%02x_A%04x as both code entry and data",
                    entries->items[i].bank, (unsigned)entries->items[i].addr & 0xffffu);
            }
            if (!entries->items[i].has_bank && data_ranges->items[j].bank == 0xffu &&
                entries->items[i].addr == data_ranges->items[j].addr) {
                die("config classifies 0x%04x as both code entry and data",
                    (unsigned)entries->items[i].addr & 0xffffu);
            }
        }
    }
    for (i = 0; i < tables->count; i++) {
        for (j = 0; j < data_ranges->count; j++) {
            if (tables->items[i].bank == data_ranges->items[j].bank &&
                tables->items[i].addr == data_ranges->items[j].addr) {
                die("config classifies B%02x_A%04x as both table and data",
                    tables->items[i].bank, (unsigned)tables->items[i].addr & 0xffffu);
            }
        }
    }
}

size_t labels_at(uint32_t addr, const Label *labels, size_t label_count, int sorted)
{
    size_t count = 0;
    size_t i;

    if (sorted) {
        i = label_lower_bound(labels, label_count, addr);
        while (i < label_count && labels[i].addr == addr) { count++; i++; }
        return count;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].addr == addr) {
            count++;
        }
    }
    return count;
}

int code_label_at(uint32_t addr, const Label *labels, size_t label_count, int sorted)
{
    size_t i;

    if (sorted) {
        i = label_lower_bound(labels, label_count, addr);
        while (i < label_count && labels[i].addr == addr) {
            if (labels[i].is_code) return 1;
            i++;
        }
        return 0;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].addr == addr && labels[i].is_code) {
            return 1;
        }
    }
    return 0;
}

const DataRange *data_range_at(uint8_t bank, uint32_t addr, const DataRanges *ranges)
{
    size_t lo, hi;

    if (!ranges) {
        return NULL;
    }
    lo = 0;
    hi = ranges->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const DataRange *r = &ranges->items[mid];

        if (r->bank < bank || (r->bank == bank && r->addr < addr)) {
            lo = mid + 1;
        } else if (r->bank > bank || (r->bank == bank && r->addr > addr)) {
            hi = mid;
        } else {
            return r;
        }
    }
    return NULL;
}

const char *config_doc_at(const ConfigDocs *docs, uint8_t bank, uint32_t addr)
{
    size_t i;

    if (!docs) {
        return NULL;
    }
    for (i = 0; i < docs->count; i++) {
        if (docs->items[i].has_bank && docs->items[i].bank == bank &&
            docs->items[i].addr == addr) {
            return docs->items[i].text;
        }
    }
    if (bank == 0xffu) {
        for (i = 0; i < docs->count; i++) {
            if (!docs->items[i].has_bank && docs->items[i].addr == addr) {
                return docs->items[i].text;
            }
        }
    }
    return NULL;
}

int string_label_at(uint32_t addr, const Label *labels, size_t label_count, int sorted)
{
    size_t i;

    if (sorted) {
        i = label_lower_bound(labels, label_count, addr);
        while (i < label_count && labels[i].addr == addr) {
            if (labels[i].is_string) return 1;
            i++;
        }
        return 0;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].addr == addr && labels[i].is_string) {
            return 1;
        }
    }
    return 0;
}

const char *label_name_at(uint32_t addr, const Label *labels, size_t label_count, int sorted)
{
    size_t i;
    const char *generated = NULL;

    if (sorted) {
        i = label_lower_bound(labels, label_count, addr);
        while (i < label_count && labels[i].addr == addr) {
            if (!generated_label_name(labels[i].name)) return labels[i].name;
            if (!generated) generated = labels[i].name;
            i++;
        }
        return generated;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].addr == addr) {
            if (!generated_label_name(labels[i].name)) {
                return labels[i].name;
            }
            if (!generated) {
                generated = labels[i].name;
            }
        }
    }
    return generated;
}

const char *symbol_name_at(uint32_t addr, const ConfigSymbols *symbols)
{
    size_t i;

    if (!symbols) {
        return NULL;
    }
    for (i = 0; i < symbols->count; i++) {
        if (symbols->items[i].value == addr) {
            return symbols->items[i].name;
        }
    }
    return NULL;
}

int label_between(uint32_t start, uint32_t end, const Label *labels, size_t label_count,
                  int sorted)
{
    size_t i;

    if (sorted) {
        i = label_lower_bound(labels, label_count, start + 1);
        return i < label_count && labels[i].addr < end;
    }
    for (i = 0; i < label_count; i++) {
        if (labels[i].addr > start && labels[i].addr < end) {
            return 1;
        }
    }
    return 0;
}

const char *lookup_label_for_cpu(void *ctx, uint32_t addr)
{
    const LabelLookup *lookup = (const LabelLookup *)ctx;
    const char *name;

    name = symbol_name_at(addr, lookup->symbols);
    if (name) {
        return name;
    }
    name = label_name_at(addr, lookup->labels, lookup->label_count, lookup->sorted);
    if (name) {
        return name;
    }
    return label_name_at(addr, lookup->extra_labels, lookup->extra_label_count, lookup->sorted);
}

const TableDef *table_def_at(uint8_t bank, uint32_t addr, const TableDefs *tables)
{
    size_t lo = 0, hi = tables->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const TableDef *t = &tables->items[mid];

        if (t->bank < bank || (t->bank == bank && t->addr < addr)) {
            lo = mid + 1;
        } else if (t->bank > bank || (t->bank == bank && t->addr > addr)) {
            hi = mid;
        } else {
            return t;
        }
    }
    return NULL;
}

int in_system_addr(uint32_t addr)
{
    return addr >= APEX_SYSTEM_ORG && addr <= 0xffffu;
}

int scannable_code_addr(uint32_t addr, size_t used)
{
    return addr >= APEX_SYSTEM_ORG && addr < APEX_SYSTEM_ORG + used && addr < 0xfff2u;
}

uint32_t detect_inline_dispatcher(const uint8_t *data, size_t used, const VectorInfo *vectors,
                                  size_t vector_count)
{
    uint32_t addr;
    size_t pos;
    size_t i;

    if (vector_count == 0) {
        return 0;
    }
    addr = vectors[0].target_addr;
    if (!scannable_code_addr(addr, used)) {
        return 0;
    }
    pos = (size_t)(addr - APEX_SYSTEM_ORG);
    for (i = 0; i < 8 && pos < used; i++) {
        char inst[64];
        Cpu6809InstrInfo info =
            cpu6809_disassemble_info(data + pos, used - pos, APEX_SYSTEM_ORG + (uint32_t)pos,
                                     inst, sizeof(inst));

        (void)inst;
        if (info.size == 0) {
            return 0;
        }
        if (info.has_target && pos + info.size < used && data[pos + info.size] == 0x04) {
            return info.target;
        }
        pos += info.size;
        if (info.flags & CPU6809_FLOW_STOP) {
            return 0;
        }
    }
    return 0;
}

unsigned inline_bytes_consumed(const Cpu6809InstrInfo *info, const InlineSignatures *sigs,
                               uint8_t current_bank, size_t pos, size_t used)
{
    const InlineSignature *sig;

    if (!info->has_target) {
        return 0;
    }
    sig = inline_signature_for(sigs, current_bank, info->target);
    if (!sig || pos + info->size + sig->length > used) {
        return 0;
    }
    return sig->length;
}

int valid_far_code_target(uint16_t addr, uint8_t bank, const uint8_t *paged_rom, size_t banks)
{
    if (bank == 0xffu) {
        return in_system_addr(addr);
    }
    return bank_index_for_far_ref(paged_rom, banks, bank) >= 0 && addr >= APEX_PAGED_ORG &&
           addr < 0x8000u;
}

void apply_inline_far_label(TableFieldKind kind, const uint8_t *paged_rom, size_t banks,
                            LabelSet *bank_labels, LabelSet *system_labels, uint16_t addr,
                            uint8_t bank)
{
    int bank_index;
    int is_code = kind == TABLE_FAR_CODE;
    int is_string = kind == TABLE_FAR_STRING;
    const char *source = kind == TABLE_FAR_CODE   ? "inline_far_code_ref" :
                         kind == TABLE_FAR_STRING ? "inline_far_string_ref" :
                         kind == TABLE_FAR_DMD_FULLFRAME ?
                             "inline_far_dmd_fullframe_ref" :
                             "inline_far_data_ref";
    Label *label;

    if (bank == 0xffu && in_system_addr(addr)) {
        label = add_label(system_labels, addr, make_generated_label(addr), is_code);
        explain_label(label, source);
        explain_label_kind(label, source);
        if (!is_code) {
            mark_label_data(label);
        }
        if (is_string) {
            label->is_string = 1;
        }
        return;
    }
    bank_index = bank_index_for_far_ref(paged_rom, banks, bank);
    if (bank_index >= 0 && addr >= APEX_PAGED_ORG && addr < 0x8000u) {
        label = add_label(&bank_labels[bank_index], addr,
                          make_bank_label(bank_id_for_index(paged_rom, bank_index), addr),
                          is_code);
        explain_label(label, source);
        explain_label_kind(label, source);
        if (!is_code) {
            mark_label_data(label);
        }
        if (is_string) {
            label->is_string = 1;
        }
    }
}

void apply_inline_ptr16_label(TableFieldKind kind, LabelSet *labels, uint8_t bank, uint16_t ptr)
{
    Label *target;
    const char *source = kind == TABLE_PTR16_CODE   ? "inline_ptr16_code_ref" :
                         kind == TABLE_PTR16_STRING ? "inline_ptr16_string_ref" :
                         kind == TABLE_PTR16_DMD_FULLFRAME ?
                             "inline_ptr16_dmd_fullframe_ref" :
                             "inline_ptr16_data_ref";

    if (ptr < APEX_PAGED_ORG || ptr >= 0x8000u) {
        return;
    }
    target = add_label(labels, ptr, make_bank_label(bank, ptr), kind == TABLE_PTR16_CODE);
    explain_label(target, source);
    explain_label_kind(target, source);
    if (kind != TABLE_PTR16_CODE) {
        mark_label_data(target);
    }
    if (kind == TABLE_PTR16_STRING) {
        target->is_string = 1;
    }
}

void apply_system_ptr16_label(TableFieldKind kind, LabelSet *system_labels, uint16_t ptr,
                              const char *source_prefix)
{
    int is_code = kind == TABLE_PTR16_CODE;
    int is_string = kind == TABLE_PTR16_STRING;
    const char *source = source_prefix;
    Label *target;

    if (!in_system_addr(ptr)) {
        return;
    }
    if (strcmp(source_prefix, "inline") == 0) {
        source = is_code ? "inline_ptr16_code_ref" :
                 is_string ? "inline_ptr16_string_ref" :
                 kind == TABLE_PTR16_DMD_FULLFRAME ? "inline_ptr16_dmd_fullframe_ref" :
                             "inline_ptr16_data_ref";
    } else if (strcmp(source_prefix, "table") == 0) {
        source = is_code ? "table_ptr16_code_ref" :
                 is_string ? "table_ptr16_string_ref" :
                 kind == TABLE_PTR16_DMD_FULLFRAME ? "table_ptr16_dmd_fullframe_ref" :
                             "table_ptr16_data_ref";
    }
    target = add_label(system_labels, ptr, make_generated_label(ptr), is_code);
    explain_label(target, source);
    explain_label_kind(target, source);
    if (!is_code) {
        mark_label_data(target);
    }
    if (is_string) {
        target->is_string = 1;
    }
}

void collect_inline_refs(const InlineSignature *sig, const uint8_t *data, size_t used,
                         size_t *pos, uint8_t current_bank, const uint8_t *paged_rom,
                         size_t banks, LabelSet *bank_labels, LabelSet *system_labels,
                         const char *source, uint32_t source_addr, ReferenceSet *refs)
{
    size_t i;

    for (i = 0; i < sig->schema.count; i++) {
        size_t n;

        for (n = 0; n < sig->schema.items[i].count; n++) {
            TableFieldKind kind = sig->schema.items[i].kind;

            if (kind == TABLE_BYTE) {
                (*pos)++;
            } else if (kind == TABLE_WORD) {
                *pos += 2u;
            } else if (table_kind_is_far(kind)) {
                uint16_t addr;
                uint8_t bank;
                int bank_index;

                if (*pos + 3u > used) {
                    return;
                }
                addr = read_be16(data + *pos);
                bank = data[*pos + 2u];
                if (kind != TABLE_FAR_CODE || valid_far_code_target(addr, bank, paged_rom, banks)) {
                    apply_inline_far_label(kind, paged_rom, banks, bank_labels, system_labels,
                                           addr, bank);
                    if (bank == 0xffu && in_system_addr(addr)) {
                        add_reference(refs, 0xff, addr, current_bank, source_addr, "code",
                                      source);
                    } else {
                        bank_index = bank_index_for_far_ref(paged_rom, banks, bank);
                        if (bank_index >= 0 && addr >= APEX_PAGED_ORG && addr < 0x8000u) {
                            add_reference(refs, bank_id_for_index(paged_rom, bank_index), addr,
                                          current_bank, source_addr, "code", source);
                        }
                    }
                }
                *pos += 3u;
            } else {
                uint16_t ptr;

                if (*pos + 2u > used) {
                    return;
                }
                ptr = read_be16(data + *pos);
                if (ptr >= APEX_PAGED_ORG && ptr < 0x8000u) {
                    int bank_index = bank_index_for_id(paged_rom, banks, current_bank);

                    if (bank_index >= 0) {
                        apply_inline_ptr16_label(kind, &bank_labels[bank_index], current_bank, ptr);
                        add_reference(refs, current_bank, ptr, current_bank, source_addr, "code",
                                      source);
                    }
                } else if (in_system_addr(ptr)) {
                    apply_system_ptr16_label(kind, system_labels, ptr, "inline");
                    add_reference(refs, 0xff, ptr, current_bank, source_addr, "code", source);
                }
                *pos += 2u;
            }
        }
    }
}

static int addr_ref_excluded(const ConfigEntries *excl, uint8_t bank, uint32_t addr)
{
    size_t i;

    if (!excl) {
        return 0;
    }
    for (i = 0; i < excl->count; i++) {
        if (excl->items[i].addr == addr &&
            (!excl->items[i].has_bank || excl->items[i].bank == bank)) {
            return 1;
        }
    }
    return 0;
}

void collect_code_targets(const uint8_t *data, size_t used, uint32_t base_addr, LabelSet *labels,
                          const InlineSignatures *inline_sigs, const uint8_t *paged_rom,
                          size_t banks, LabelSet *bank_labels, LabelSet *system_labels,
                          const DataRanges *data_ranges, uint8_t current_bank,
                          ReferenceSet *refs, const ConfigEntries *ref_exclusions)
{
    size_t i = 0;

    while (i < labels->count) {
        Label *label = &labels->items[i++];
        uint32_t addr = label->addr;
        const char *source = label_name_at(addr, labels->items, labels->count, 0);
        size_t pos;

        if (!label->is_code || label->scanned || addr < base_addr ||
            addr >= base_addr + used ||
            (base_addr == APEX_SYSTEM_ORG && addr >= 0xfff2u)) {
            continue;
        }
        label->scanned = 1;
        pos = (size_t)(addr - base_addr);
        while (pos < used) {
            char inst[64];
            uint32_t instr_addr = base_addr + (uint32_t)pos;
            Cpu6809InstrInfo info =
                cpu6809_disassemble_info(data + pos, used - pos, instr_addr, inst, sizeof(inst));

            (void)inst;
            if (pos > (size_t)(addr - base_addr) &&
                data_range_at(current_bank, base_addr + (uint32_t)pos, data_ranges)) {
                /* Code flow reaches a data-classified region. If there is a
                   user-defined label at this address (not auto-generated), mark it
                   as conflicted so the disassembler emits a WARNING. */
                uint32_t cont_addr = base_addr + (uint32_t)pos;
                size_t j;
                for (j = 0; j < labels->count; j++) {
                    if (labels->items[j].addr == cont_addr &&
                        labels->items[j].name &&
                        !generated_any_label_name(labels->items[j].name)) {
                        labels->items[j].is_conflict = 1;
                        explain_label_kind(&labels->items[j], "code_flow");
                        break;
                    }
                }
                break;
            }
            if (info.size == 0) {
                break;
            }
            if (info.has_target) {
                if (info.target >= base_addr && info.target < base_addr + used) {
                    if (!label_name_at(info.target, labels->items, labels->count, 0)) {
                        Label *target;
                        if (base_addr == APEX_SYSTEM_ORG) {
                            target = add_label(labels, info.target,
                                               make_generated_label(info.target), 1);
                        } else {
                            target = add_label(labels, info.target,
                                               make_bank_label(data[0], info.target), 1);
                        }
                        explain_label(target, "code_flow");
                        explain_label_kind(target, "code_flow");
                    } else if (!data_range_at(current_bank, info.target, data_ranges)) {
                        /* A user-defined label exists at the branch target but may not
                           be marked as code yet. Promote it so the convergence loop
                           will scan it on the next pass. */
                        {
                            size_t j;
                            for (j = 0; j < labels->count; j++) {
                                if (labels->items[j].addr == info.target &&
                                    labels->items[j].is_explicit_entry) {
                                    labels->items[j].reached_by_flow = 1;
                                    break;
                                }
                            }
                        }
                        add_label(labels, info.target, NULL, 1);
                    }
                } else if (base_addr != APEX_SYSTEM_ORG && in_system_addr(info.target)) {
                    Label *target =
                        add_label(system_labels, info.target, make_generated_label(info.target), 1);

                    explain_label(target, "code_flow");
                    explain_label_kind(target, "code_flow");
                }
                if (info.target >= base_addr && info.target < base_addr + used) {
                    add_reference(refs, current_bank, info.target, current_bank, instr_addr, "code",
                                  source);
                } else if (base_addr != APEX_SYSTEM_ORG && in_system_addr(info.target)) {
                    add_reference(refs, 0xff, info.target, current_bank, instr_addr, "code",
                                  source);
                }
            }
            if (info.has_addr_ref && !info.has_target) {
                if (info.addr_ref >= base_addr && info.addr_ref < base_addr + used) {
                    if (!addr_ref_excluded(ref_exclusions, current_bank, info.addr_ref)) {
                        add_reference(refs, current_bank, info.addr_ref, current_bank, instr_addr,
                                      "code", source);
                    }
                } else if (in_system_addr(info.addr_ref)) {
                    if (!addr_ref_excluded(ref_exclusions, 0xff, info.addr_ref)) {
                        add_reference(refs, 0xff, info.addr_ref, current_bank, instr_addr, "code",
                                      source);
                    }
                }
            }
            {
                const InlineSignature *sig =
                    inline_signature_for(inline_sigs, current_bank, info.target);

                if (info.has_target && sig &&
                    pos + info.size + sig->length <= used &&
                    paged_rom && bank_labels) {
                    size_t inline_pos = pos + info.size;

                    collect_inline_refs(sig, data, used, &inline_pos, current_bank, paged_rom,
                                        banks, bank_labels,
                                        system_labels ? system_labels : labels, source,
                                        instr_addr, refs);
                }
            }
            pos += info.size;
            pos += inline_bytes_consumed(&info, inline_sigs, current_bank, pos - info.size, used);
            if (!(info.flags & CPU6809_FLOW_STOP) && pos < used) {
                uint32_t next_addr = base_addr + (uint32_t)pos;
                size_t j;
                for (j = 0; j < labels->count; j++) {
                    if (labels->items[j].addr == next_addr &&
                        labels->items[j].is_explicit_entry) {
                        labels->items[j].reached_by_flow = 1;
                        break;
                    }
                }
            }
            if (info.flags & CPU6809_FLOW_STOP) {
                break;
            }
        }
    }
}

const char *vector_entry_at(uint32_t addr, const VectorInfo *vectors, size_t vector_count)
{
    size_t i;

    for (i = 0; i < vector_count; i++) {
        if (vectors[i].vector_addr == addr) {
            return vectors[i].entry_name;
        }
    }
    return NULL;
}

size_t valid_string_len(const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (data[i] == 0x00) {
            return i + 1u;
        }
        if (data[i] < 0x20u || data[i] > 0x7fu) {
            return 0;
        }
    }
    return 0;
}

/* --- sort + binary-search helpers --- */

size_t label_lower_bound(const Label *labels, size_t count, uint32_t addr)
{
    size_t lo = 0, hi = count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (labels[mid].addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t refs_lower_bound(const ReferenceSet *refs, uint8_t bank, uint32_t addr)
{
    size_t lo = 0, hi = refs->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Reference *r = &refs->items[mid];

        if (r->bank < bank || (r->bank == bank && r->addr < addr)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int label_addr_cmp(const void *a, const void *b)
{
    const Label *la = (const Label *)a;
    const Label *lb = (const Label *)b;

    if (la->addr < lb->addr) return -1;
    if (la->addr > lb->addr) return 1;
    return 0;
}

void sort_label_set(LabelSet *set)
{
    if (set->count > 1) {
        qsort(set->items, set->count, sizeof(set->items[0]), label_addr_cmp);
    }
    set->sorted = 1;
}

static int data_range_cmp(const void *a, const void *b)
{
    const DataRange *ra = (const DataRange *)a;
    const DataRange *rb = (const DataRange *)b;

    if (ra->bank != rb->bank) return (int)(unsigned)ra->bank - (int)(unsigned)rb->bank;
    if (ra->addr < rb->addr) return -1;
    if (ra->addr > rb->addr) return 1;
    return 0;
}

void sort_data_ranges(DataRanges *ranges)
{
    if (ranges->count > 1) {
        qsort(ranges->items, ranges->count, sizeof(ranges->items[0]), data_range_cmp);
    }
}

static int table_def_cmp(const void *a, const void *b)
{
    const TableDef *ta = (const TableDef *)a;
    const TableDef *tb = (const TableDef *)b;

    if (ta->bank != tb->bank) return (int)(unsigned)ta->bank - (int)(unsigned)tb->bank;
    if (ta->addr < tb->addr) return -1;
    if (ta->addr > tb->addr) return 1;
    return 0;
}

void sort_table_defs(TableDefs *tables)
{
    if (tables->count > 1) {
        qsort(tables->items, tables->count, sizeof(tables->items[0]), table_def_cmp);
    }
}

static int inline_sig_cmp(const void *a, const void *b)
{
    const InlineSignature *sa = (const InlineSignature *)a;
    const InlineSignature *sb = (const InlineSignature *)b;

    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return 1;
    return 0;
}

void sort_inline_signatures(InlineSignatures *sigs)
{
    if (sigs->count > 1) {
        qsort(sigs->items, sigs->count, sizeof(sigs->items[0]), inline_sig_cmp);
    }
}

static int ref_cmp(const void *a, const void *b)
{
    const Reference *ra = (const Reference *)a;
    const Reference *rb = (const Reference *)b;
    int c;

    if (ra->bank != rb->bank) return (int)(unsigned)ra->bank - (int)(unsigned)rb->bank;
    if (ra->addr < rb->addr) return -1;
    if (ra->addr > rb->addr) return 1;
    if (ra->source_bank != rb->source_bank)
        return (int)(unsigned)ra->source_bank - (int)(unsigned)rb->source_bank;
    if (ra->source_addr < rb->source_addr) return -1;
    if (ra->source_addr > rb->source_addr) return 1;
    if (ra->row_index != rb->row_index) return ra->row_index - rb->row_index;
    c = strcmp(ra->kind, rb->kind);
    if (c) return c;
    return strcmp(ra->source, rb->source);
}

void sort_and_dedup_refs(ReferenceSet *refs)
{
    size_t read_idx, write_idx;

    if (!refs || refs->count < 2) {
        return;
    }
    qsort(refs->items, refs->count, sizeof(refs->items[0]), ref_cmp);
    write_idx = 0;
    for (read_idx = 1; read_idx < refs->count; read_idx++) {
        if (ref_cmp(&refs->items[read_idx], &refs->items[write_idx]) != 0) {
            refs->items[++write_idx] = refs->items[read_idx];
        }
    }
    refs->count = write_idx + 1;
    refs->sorted = 1;
}
