#define _POSIX_C_SOURCE 200809L

#include "apex.h"
#include "apexdis_api.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char *name;
    unsigned bank;
    unsigned addr;
    unsigned rom_offset;
    char *kind;
    char *block_kind;
    char *spec;
    char *doc;
    char *details;
    size_t line_index;
} LabelInfo;

typedef struct {
    char *target_name;
    char *kind;
    char *source;
} XrefInfo;

typedef struct {
    char *name;
    unsigned bank;
    unsigned addr;
    unsigned rom_offset;
    size_t line_index;
} TableInfo;

typedef struct {
    char *kind;
    char *target_name;
    unsigned bank;
    unsigned addr;
    unsigned rom_offset;
    size_t line_index;
} DataEdgeInfo;

typedef struct {
    char *key;
    char *value;
} OverrideItem;

typedef struct {
    OverrideItem *items;
    size_t count;
    size_t cap;
} OverrideSet;

static const char *override_value(const OverrideSet *set, const char *key)
{
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].key, key) == 0) {
            return set->items[i].value;
        }
    }
    return NULL;
}

typedef struct {
    char *rom_path;
    char *config_path;
    char *overlay_path;
    char *asm_text;
    char *text;
    char **lines;
    size_t line_count;
    LabelInfo *labels;
    size_t label_count;
    size_t label_cap;
    XrefInfo *xrefs;
    size_t xref_count;
    size_t xref_cap;
    TableInfo *tables;
    size_t table_count;
    size_t table_cap;
    DataEdgeInfo *data_edges;
    size_t data_edge_count;
    size_t data_edge_cap;
    OverrideSet label_overrides;
    OverrideSet inline_overrides;
    OverrideSet entry_overrides;
    OverrideSet data_overrides;
    OverrideSet table_overrides;
    OverrideSet routine_doc_overrides;
    OverrideSet table_doc_overrides;
    Buffer rom;
} GuiProject;

extern const unsigned char apexgui_html[];
extern const unsigned int apexgui_html_len;

static void clear_runtime_project(GuiProject *project);
static void format_target_key(unsigned bank, unsigned addr, char *out, size_t out_size);

static char *dup_string_local(const char *s)
{
    size_t len = strlen(s);
    char *copy = xmalloc(len + 1u);

    memcpy(copy, s, len + 1u);
    return copy;
}

static void free_override_set(OverrideSet *set)
{
    size_t i;

    for (i = 0; i < set->count; i++) {
        free(set->items[i].key);
        free(set->items[i].value);
    }
    free(set->items);
    memset(set, 0, sizeof(*set));
}

static void set_override(OverrideSet *set, const char *key, const char *value)
{
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].key, key) == 0) {
            free(set->items[i].value);
            set->items[i].value = dup_string_local(value);
            return;
        }
    }
    if (set->count == set->cap) {
        size_t new_cap = set->cap == 0 ? 8 : set->cap * 2u;
        OverrideItem *items = realloc(set->items, new_cap * sizeof(set->items[0]));

        if (!items) {
            die("out of memory");
        }
        set->items = items;
        set->cap = new_cap;
    }
    set->items[set->count].key = dup_string_local(key);
    set->items[set->count].value = dup_string_local(value);
    set->count++;
}

static void remove_override(OverrideSet *set, const char *key)
{
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].key, key) == 0) {
            free(set->items[i].key);
            free(set->items[i].value);
            memmove(&set->items[i], &set->items[i + 1],
                    (set->count - i - 1u) * sizeof(set->items[0]));
            set->count--;
            return;
        }
    }
}

static char *dirname_copy(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;

    if (!slash) {
        return dup_string_local(".");
    }
    dir = xmalloc((size_t)(slash - path) + 1u);
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = '\0';
    return dir;
}

static const char *basename_ptr(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *join_path2(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 2u;
    char *path = xmalloc(len);

    snprintf(path, len, "%s/%s", dir, name);
    return path;
}

static char *make_overlay_path(const char *rom_path, const char *config_path)
{
    char *dir;
    const char *base;
    size_t len;
    char *name;
    char *path;

    if (config_path && *config_path) {
        dir = dirname_copy(config_path);
        base = basename_ptr(config_path);
    } else {
        dir = dirname_copy(rom_path);
        base = basename_ptr(rom_path);
    }
    len = strlen(base) + strlen(".apexgui.ini") + 1u;
    name = xmalloc(len);
    snprintf(name, len, "%s.apexgui.ini", base);
    path = join_path2(dir, name);
    free(name);
    free(dir);
    return path;
}

static int file_exists_local(const char *path)
{
    FILE *f = fopen(path, "r");

    if (!f) {
        return 0;
    }
    fclose(f);
    return 1;
}

static void load_overlay_file(const char *path, OverrideSet *labels, OverrideSet *inlines,
                              OverrideSet *entries, OverrideSet *data, OverrideSet *tables,
                              OverrideSet *routine_docs, OverrideSet *table_docs)
{
    Buffer raw;
    char *text;
    char *line;
    char *saveptr = NULL;
    OverrideSet *current = NULL;

    if (!path || !file_exists_local(path)) {
        return;
    }
    raw = read_file(path);
    text = xmalloc(raw.size + 1u);
    memcpy(text, raw.data, raw.size);
    text[raw.size] = '\0';
    free(raw.data);
    for (line = strtok_r(text, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        char *eq;
        char *key;
        char *value;

        line = trim(line);
        if (*line == '\0' || *line == ';' || *line == '#') {
            continue;
        }
        if (*line == '[') {
            if (strcmp(line, "[labels]") == 0) {
                current = labels;
            } else if (strcmp(line, "[inline]") == 0) {
                current = inlines;
            } else if (strcmp(line, "[entries]") == 0) {
                current = entries;
            } else if (strcmp(line, "[data]") == 0) {
                current = data;
            } else if (strcmp(line, "[tables]") == 0) {
                current = tables;
            } else if (strcmp(line, "[routine_docs]") == 0) {
                current = routine_docs;
            } else if (strcmp(line, "[table_docs]") == 0) {
                current = table_docs;
            } else {
                current = NULL;
            }
            continue;
        }
        if (!current) {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);
        if (*key && *value) {
            set_override(current, key, value);
        }
    }
    free(text);
}

static void write_section(FILE *out, const char *name, const OverrideSet *set)
{
    size_t i;

    if (set->count == 0) {
        return;
    }
    fprintf(out, "\n[%s]\n", name);
    for (i = 0; i < set->count; i++) {
        const char *value = set->items[i].value;
        int needs_quotes = 0;
        const char *p;

        for (p = value; *p; p++) {
            if (*p == '\n' || *p == ';' || *p == '#' || *p == '\\' || *p == '"' ||
                isspace((unsigned char)*p)) {
                needs_quotes = 1;
                break;
            }
        }
        fprintf(out, "%s = ", set->items[i].key);
        if (!needs_quotes) {
            fputs(value, out);
        } else {
            fputc('"', out);
            for (p = value; *p; p++) {
                switch (*p) {
                case '\n':
                    fputs("\\n", out);
                    break;
                case ';':
                    fputs("\\;", out);
                    break;
                case '#':
                    fputs("\\#", out);
                    break;
                case '\\':
                    fputs("\\\\", out);
                    break;
                case '"':
                    fputs("\\\"", out);
                    break;
                default:
                    fputc(*p, out);
                    break;
                }
            }
            fputc('"', out);
        }
        fputc('\n', out);
    }
}

static void write_overlay_file(const GuiProject *project)
{
    FILE *out = fopen(project->overlay_path, "w");

    if (!out) {
        die("failed to open %s for writing", project->overlay_path);
    }
    fprintf(out, "; Apex GUI overlay\n");
    if (project->config_path && *project->config_path) {
        fprintf(out, "include = %s\n", basename_ptr(project->config_path));
    }
    write_section(out, "labels", &project->label_overrides);
    write_section(out, "inline", &project->inline_overrides);
    write_section(out, "entries", &project->entry_overrides);
    write_section(out, "data", &project->data_overrides);
    write_section(out, "tables", &project->table_overrides);
    write_section(out, "routine_docs", &project->routine_doc_overrides);
    write_section(out, "table_docs", &project->table_doc_overrides);
    if (fclose(out) != 0) {
        die("failed to close %s", project->overlay_path);
    }
}

static void clear_runtime_project(GuiProject *project)
{
    size_t i;

    free(project->lines);
    project->lines = NULL;
    for (i = 0; i < project->label_count; i++) {
        free(project->labels[i].name);
        free(project->labels[i].kind);
        free(project->labels[i].block_kind);
        free(project->labels[i].spec);
        free(project->labels[i].doc);
        free(project->labels[i].details);
    }
    for (i = 0; i < project->xref_count; i++) {
        free(project->xrefs[i].target_name);
        free(project->xrefs[i].kind);
        free(project->xrefs[i].source);
    }
    for (i = 0; i < project->table_count; i++) {
        free(project->tables[i].name);
    }
    for (i = 0; i < project->data_edge_count; i++) {
        free(project->data_edges[i].kind);
        free(project->data_edges[i].target_name);
    }
    free(project->labels);
    free(project->xrefs);
    free(project->tables);
    free(project->data_edges);
    free(project->asm_text);
    free(project->text);
    free(project->rom.data);
    project->labels = NULL;
    project->xrefs = NULL;
    project->tables = NULL;
    project->data_edges = NULL;
    project->asm_text = NULL;
    project->text = NULL;
    project->rom.data = NULL;
    project->label_count = project->label_cap = 0;
    project->xref_count = project->xref_cap = 0;
    project->table_count = project->table_cap = 0;
    project->data_edge_count = project->data_edge_cap = 0;
    project->line_count = 0;
}

static const char *effective_config_path(const GuiProject *project)
{
    if (project->overlay_path && file_exists_local(project->overlay_path)) {
        return project->overlay_path;
    }
    return project->config_path;
}

static void free_project(GuiProject *project)
{
    if (!project) {
        return;
    }
    clear_runtime_project(project);
    free_override_set(&project->label_overrides);
    free_override_set(&project->inline_overrides);
    free_override_set(&project->entry_overrides);
    free_override_set(&project->data_overrides);
    free_override_set(&project->table_overrides);
    free_override_set(&project->routine_doc_overrides);
    free_override_set(&project->table_doc_overrides);
    free(project->rom_path);
    free(project->config_path);
    free(project->overlay_path);
}

static void add_label(GuiProject *project, const char *name, unsigned bank, unsigned addr,
                      unsigned rom_offset, const char *kind, const char *block_kind,
                      const char *spec, const char *doc, const char *details, size_t line_index)
{
    LabelInfo *label;

    if (project->label_count == project->label_cap) {
        size_t new_cap = project->label_cap == 0 ? 128 : project->label_cap * 2u;
        LabelInfo *items = realloc(project->labels, new_cap * sizeof(project->labels[0]));

        if (!items) {
            die("out of memory");
        }
        project->labels = items;
        project->label_cap = new_cap;
    }
    label = &project->labels[project->label_count++];
    label->name = dup_string_local(name);
    label->bank = bank;
    label->addr = addr;
    label->rom_offset = rom_offset;
    label->kind = dup_string_local(kind ? kind : "");
    label->block_kind = dup_string_local(block_kind ? block_kind : "");
    label->spec = dup_string_local(spec ? spec : "");
    label->doc = dup_string_local(doc ? doc : "");
    label->details = dup_string_local(details ? details : "");
    label->line_index = line_index;
}

static void add_table(GuiProject *project, const char *name, unsigned bank, unsigned addr,
                      unsigned rom_offset, size_t line_index)
{
    TableInfo *table;

    if (project->table_count == project->table_cap) {
        size_t new_cap = project->table_cap == 0 ? 64 : project->table_cap * 2u;
        TableInfo *items = realloc(project->tables, new_cap * sizeof(project->tables[0]));

        if (!items) {
            die("out of memory");
        }
        project->tables = items;
        project->table_cap = new_cap;
    }
    table = &project->tables[project->table_count++];
    table->name = dup_string_local(name);
    table->bank = bank;
    table->addr = addr;
    table->rom_offset = rom_offset;
    table->line_index = line_index;
}

static void add_data_edge(GuiProject *project, const char *kind, const char *target_name,
                          unsigned bank, unsigned addr, unsigned rom_offset, size_t line_index)
{
    DataEdgeInfo *edge;

    if (project->data_edge_count == project->data_edge_cap) {
        size_t new_cap = project->data_edge_cap == 0 ? 64 : project->data_edge_cap * 2u;
        DataEdgeInfo *items =
            realloc(project->data_edges, new_cap * sizeof(project->data_edges[0]));

        if (!items) {
            die("out of memory");
        }
        project->data_edges = items;
        project->data_edge_cap = new_cap;
    }
    edge = &project->data_edges[project->data_edge_count++];
    edge->kind = dup_string_local(kind);
    edge->target_name = dup_string_local(target_name ? target_name : "");
    edge->bank = bank;
    edge->addr = addr;
    edge->rom_offset = rom_offset;
    edge->line_index = line_index;
}

static void append_text(char **text, size_t *len, const char *line)
{
    size_t add = strlen(line);
    char *next = realloc(*text, *len + add + 2u);

    if (!next) {
        die("out of memory");
    }
    *text = next;
    memcpy(*text + *len, line, add);
    *len += add;
    (*text)[(*len)++] = '\n';
    (*text)[*len] = '\0';
}

static void trim_trailing_newline(char *text)
{
    size_t len;

    if (!text) {
        return;
    }
    len = strlen(text);
    while (len > 0 && text[len - 1u] == '\n') {
        text[--len] = '\0';
    }
}

static void add_xref(GuiProject *project, const char *target_name, const char *kind,
                     const char *source)
{
    XrefInfo *xref;

    if (project->xref_count == project->xref_cap) {
        size_t new_cap = project->xref_cap == 0 ? 256 : project->xref_cap * 2u;
        XrefInfo *items = realloc(project->xrefs, new_cap * sizeof(project->xrefs[0]));

        if (!items) {
            die("out of memory");
        }
        project->xrefs = items;
        project->xref_cap = new_cap;
    }
    xref = &project->xrefs[project->xref_count++];
    xref->target_name = dup_string_local(target_name);
    xref->kind = dup_string_local(kind);
    xref->source = dup_string_local(source);
}

static int parse_hex_value(const char *text, unsigned *out)
{
    uint32_t value;

    if (!parse_u32(text, &value)) {
        return 0;
    }
    *out = (unsigned)value;
    return 1;
}

static int parse_target_key_local(const char *text, unsigned *bank, unsigned *addr)
{
    unsigned parsed_bank = 0;
    unsigned parsed_addr = 0;

    if (sscanf(text, "B%2x_A%4x", &parsed_bank, &parsed_addr) != 2 || parsed_bank > 0xffu ||
        parsed_addr > 0xffffu) {
        return 0;
    }
    *bank = parsed_bank;
    *addr = parsed_addr;
    return 1;
}

static int parse_label_comment(const char *line, unsigned *bank, unsigned *addr, unsigned *rom)
{
    return sscanf(line, "; label bank=0x%x cpu=0x%x rom=0x%x", bank, addr, rom) == 3;
}

static int parse_transition_comment(const char *line, char *kind, size_t kind_size,
                                    unsigned *bank, unsigned *addr, unsigned *rom)
{
    (void)kind_size;
    return sscanf(line, "; %63s bank=0x%x cpu=0x%x rom=0x%x", kind, bank, addr, rom) == 4;
}

static int is_label_definition(const char *line, char *name, size_t name_size)
{
    size_t len = strlen(line);
    size_t i;

    if (len < 2 || line[len - 1] != ':') {
        return 0;
    }
    for (i = 0; i + 1 < len; i++) {
        if (!(isalnum((unsigned char)line[i]) || line[i] == '_')) {
            return 0;
        }
    }
    if (!(isalpha((unsigned char)line[0]) || line[0] == '_')) {
        return 0;
    }
    if (len >= name_size) {
        return 0;
    }
    memcpy(name, line, len - 1u);
    name[len - 1u] = '\0';
    return 1;
}

static int is_generated_label_name_local(const char *name)
{
    unsigned bank = 0;
    unsigned addr = 0;

    if (sscanf(name, "B%2x_A%4x", &bank, &addr) == 2) {
        return 1;
    }
    return 0;
}

static int prefer_label_name(const char *current, const char *candidate)
{
    int current_generated = is_generated_label_name_local(current);
    int candidate_generated = is_generated_label_name_local(candidate);

    if (current_generated != candidate_generated) {
        return !candidate_generated;
    }
    return strcmp(candidate, current) < 0;
}

static void set_text_value(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0) {
        return;
    }
    snprintf(dest, dest_size, "%s", src ? src : "");
}

static LabelInfo *find_label(GuiProject *project, const char *name)
{
    size_t i;

    for (i = 0; i < project->label_count; i++) {
        if (strcmp(project->labels[i].name, name) == 0) {
            return &project->labels[i];
        }
    }
    return NULL;
}

static int label_name_conflicts(const GuiProject *project, const char *name, unsigned bank,
                                unsigned addr, char *other_key, size_t other_key_size)
{
    size_t i;

    for (i = 0; i < project->label_count; i++) {
        const LabelInfo *label = &project->labels[i];

        if (strcmp(label->name, name) != 0) {
            continue;
        }
        if (label->bank == bank && label->addr == addr) {
            return 0;
        }
        format_target_key(label->bank, label->addr, other_key, other_key_size);
        return 1;
    }
    return 0;
}

static const char *label_name_at_address(const GuiProject *project, unsigned bank, unsigned addr)
{
    size_t i;

    for (i = 0; i < project->label_count; i++) {
        if (project->labels[i].bank == bank && project->labels[i].addr == addr) {
            return project->labels[i].name;
        }
    }
    return NULL;
}

static void format_label_key(const LabelInfo *label, char *out, size_t out_size)
{
    if (label->bank == 0xffu) {
        snprintf(out, out_size, "Bff_A%04x", label->addr & 0xffffu);
    } else {
        snprintf(out, out_size, "B%02x_A%04x", label->bank & 0xffu, label->addr & 0xffffu);
    }
}

static void format_target_key(unsigned bank, unsigned addr, char *out, size_t out_size)
{
    if (bank == 0xffu) {
        snprintf(out, out_size, "Bff_A%04x", addr & 0xffffu);
    } else {
        snprintf(out, out_size, "B%02x_A%04x", bank & 0xffu, addr & 0xffffu);
    }
}

static void split_lines(GuiProject *project)
{
    char *cursor = project->text;
    size_t count = 1;
    size_t i = 0;

    while (*cursor) {
        if (*cursor == '\n') {
            count++;
        }
        cursor++;
    }
    project->lines = xmalloc(count * sizeof(project->lines[0]));
    project->line_count = count;
    project->lines[i++] = project->text;
    for (cursor = project->text; *cursor; cursor++) {
        if (*cursor == '\n') {
            *cursor = '\0';
            if (i < count) {
                project->lines[i++] = cursor + 1;
            }
        }
    }
}

static void parse_project_metadata(GuiProject *project)
{
    size_t i;
    int have_pending = 0;
    unsigned pending_bank = 0;
    unsigned pending_addr = 0;
    unsigned pending_rom = 0;
    char pending_kind[64];
    char pending_block_kind[16];
    char pending_spec[256];
    char *pending_doc = NULL;
    size_t pending_doc_len = 0;
    char pending_label_name[256];
    size_t pending_label_line = 0;
    char *pending_details = NULL;
    size_t pending_details_len = 0;
    const char *xref_target = NULL;

    pending_kind[0] = '\0';
    pending_block_kind[0] = '\0';
    pending_spec[0] = '\0';
    pending_label_name[0] = '\0';
    for (i = 0; i < project->line_count; i++) {
        char label_name[256];
        char transition_kind[64];
        unsigned transition_bank;
        unsigned transition_addr;
        unsigned transition_rom;
        char *line = project->lines[i];

        if (parse_label_comment(line, &pending_bank, &pending_addr, &pending_rom)) {
            have_pending = 1;
            pending_kind[0] = '\0';
            pending_block_kind[0] = '\0';
            pending_spec[0] = '\0';
            pending_label_name[0] = '\0';
            free(pending_doc);
            pending_doc = NULL;
            pending_doc_len = 0;
            free(pending_details);
            pending_details = NULL;
            pending_details_len = 0;
            continue;
        }
        if (parse_transition_comment(line, transition_kind, sizeof(transition_kind),
                                     &transition_bank, &transition_addr, &transition_rom) &&
            (strcmp(transition_kind, "code_to_data") == 0 ||
             strcmp(transition_kind, "table_to_data") == 0)) {
            add_data_edge(project, transition_kind, "", transition_bank, transition_addr,
                          transition_rom, i);
            continue;
        }
        if (strncmp(line, "; kind ", 7) == 0) {
            snprintf(pending_kind, sizeof(pending_kind), "%s", trim(line + 7));
            if (strcmp(trim(line + 7), "routine") == 0) {
                set_text_value(pending_block_kind, sizeof(pending_block_kind), "code");
            } else if (strcmp(trim(line + 7), "data") == 0) {
                set_text_value(pending_block_kind, sizeof(pending_block_kind), "data");
            } else if (strcmp(trim(line + 7), "table") == 0) {
                set_text_value(pending_block_kind, sizeof(pending_block_kind), "table");
            } else {
                set_text_value(pending_block_kind, sizeof(pending_block_kind), "unknown");
            }
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (strncmp(line, "; inline params=", 16) == 0) {
            set_text_value(pending_spec, sizeof(pending_spec), trim(line + 16));
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (strncmp(line, "; data type=", 12) == 0) {
            const char *type = trim(line + 12);
            const char *length = strstr(type, " length=");

            if (length) {
                size_t type_len = (size_t)(length - type);
                char kind_name[64];

                if (type_len >= sizeof(kind_name)) {
                    type_len = sizeof(kind_name) - 1u;
                }
                memcpy(kind_name, type, type_len);
                kind_name[type_len] = '\0';
                snprintf(pending_spec, sizeof(pending_spec), "%s[%s]", kind_name, length + 8);
            } else {
                set_text_value(pending_spec, sizeof(pending_spec), type);
            }
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (strncmp(line, "; table rows=", 13) == 0) {
            const char *format = strstr(line, " row_format=");

            if (format) {
                char format_buf[256];

                set_text_value(format_buf, sizeof(format_buf), format + 12);
                snprintf(pending_spec, sizeof(pending_spec), "counted(%s)", trim(format_buf));
            }
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (strncmp(line, "; doc ", 6) == 0) {
            append_text(&pending_doc, &pending_doc_len, line + 6);
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (have_pending && strncmp(line, "; ", 2) == 0 &&
            strncmp(line, "; referenced_by ", 16) != 0 &&
            strncmp(line, "; label ", 8) != 0) {
            append_text(&pending_details, &pending_details_len, trim(line + 2));
            continue;
        }
        if (strcmp(line, "; XREF INDEX") == 0) {
            xref_target = "";
            continue;
        }
        if (xref_target && strncmp(line, "; XREF ", 7) == 0) {
            static char target_name[256];

            if (sscanf(line, "; XREF %255s", target_name) == 1) {
                char *space = strchr(target_name, ' ');

                if (space) {
                    *space = '\0';
                }
                xref_target = target_name;
            }
            continue;
        }
        if (xref_target && strncmp(line, ";   ", 4) == 0) {
            char kind[64];
            char source[256];
            const char *payload = line + 4;
            const char *colon = strchr(payload, ':');

            if (xref_target[0] != '\0' && colon) {
                size_t kind_len = (size_t)(colon - payload);

                if (kind_len < sizeof(kind)) {
                    memcpy(kind, payload, kind_len);
                    kind[kind_len] = '\0';
                    snprintf(source, sizeof(source), "%s", colon + 1);
                    add_xref(project, xref_target, kind, source);
                }
            }
            continue;
        }
        if (is_label_definition(line, label_name, sizeof(label_name)) && have_pending) {
            if (pending_label_name[0] == '\0' ||
                prefer_label_name(pending_label_name, label_name)) {
                snprintf(pending_label_name, sizeof(pending_label_name), "%s", label_name);
                pending_label_line = i;
            }
            continue;
        }
        if (have_pending && pending_label_name[0] != '\0') {
            trim_trailing_newline(pending_doc);
            add_label(project, pending_label_name, pending_bank, pending_addr, pending_rom,
                      pending_kind[0] ? pending_kind : "unknown",
                      pending_block_kind[0] ? pending_block_kind : "unknown", pending_spec,
                      pending_doc ? pending_doc : "",
                      pending_details ? pending_details : "", pending_label_line);
            if (strcmp(pending_block_kind[0] ? pending_block_kind : "unknown", "table") == 0) {
                add_table(project, pending_label_name, pending_bank, pending_addr, pending_rom,
                          pending_label_line);
            }
            have_pending = 0;
            pending_kind[0] = '\0';
            pending_block_kind[0] = '\0';
            pending_spec[0] = '\0';
            pending_label_name[0] = '\0';
            free(pending_doc);
            pending_doc = NULL;
            pending_doc_len = 0;
            free(pending_details);
            pending_details = NULL;
            pending_details_len = 0;
        }
    }
    if (have_pending && pending_label_name[0] != '\0') {
        trim_trailing_newline(pending_doc);
        add_label(project, pending_label_name, pending_bank, pending_addr, pending_rom,
                  pending_kind[0] ? pending_kind : "unknown",
                  pending_block_kind[0] ? pending_block_kind : "unknown", pending_spec,
                  pending_doc ? pending_doc : "",
                  pending_details ? pending_details : "", pending_label_line);
        if (strcmp(pending_block_kind[0] ? pending_block_kind : "unknown", "table") == 0) {
            add_table(project, pending_label_name, pending_bank, pending_addr, pending_rom,
                      pending_label_line);
        }
    }
    free(pending_doc);
    free(pending_details);
}

static char *read_text_file_owned(const char *path)
{
    Buffer raw = read_file(path);
    char *text = xmalloc(raw.size + 1u);

    memcpy(text, raw.data, raw.size);
    text[raw.size] = '\0';
    free(raw.data);
    return text;
}

static void build_project(GuiProject *project, const char *rom_path, const char *config_path)
{
    ApexDisOptions options;
    char temp_path[] = "/tmp/apexguiXXXXXX";
    int fd = mkstemp(temp_path);

    if (fd < 0) {
        die("failed to create temporary file: %s", strerror(errno));
    }
    close(fd);
    if (!project->rom_path) {
        project->rom_path = dup_string_local(rom_path);
    }
    if (!project->config_path && config_path) {
        project->config_path = dup_string_local(config_path);
    }
    if (!project->overlay_path) {
        project->overlay_path = make_overlay_path(rom_path, config_path);
        load_overlay_file(project->overlay_path, &project->label_overrides,
                          &project->inline_overrides, &project->entry_overrides,
                          &project->data_overrides, &project->table_overrides,
                          &project->routine_doc_overrides, &project->table_doc_overrides);
    }
    memset(&options, 0, sizeof(options));
    options.input_path = rom_path;
    options.output_path = temp_path;
    options.config_path = effective_config_path(project);
    options.emit_xrefs = 1;
    options.emit_explain = 1;
    if (apexdis_run(&options) != 0) {
        unlink(temp_path);
        die("apexdis failed");
    }
    project->asm_text = read_text_file_owned(temp_path);
    project->text = dup_string_local(project->asm_text);
    unlink(temp_path);
    project->rom = read_file(rom_path);
    split_lines(project);
    parse_project_metadata(project);
}

static void json_write_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    fputc('"', out);
    while (*p) {
        if (*p == '\\' || *p == '"') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p == '\n') {
            fputs("\\n", out);
        } else if (*p == '\r') {
            fputs("\\r", out);
        } else if (*p == '\t') {
            fputs("\\t", out);
        } else if (*p < 0x20u) {
            fprintf(out, "\\u%04x", *p);
        } else {
            fputc(*p, out);
        }
        p++;
    }
    fputc('"', out);
}

static void rebuild_project(GuiProject *project)
{
    char *rom_path = dup_string_local(project->rom_path);
    char *config_path = project->config_path ? dup_string_local(project->config_path) : NULL;

    clear_runtime_project(project);
    build_project(project, rom_path, config_path);
    free(rom_path);
    free(config_path);
}

static const char *default_class_spec(const char *kind)
{
    if (strcmp(kind, "string") == 0) {
        return "string";
    }
    if (strcmp(kind, "data") == 0) {
        return "bytes[16]";
    }
    if (strcmp(kind, "table") == 0) {
        return "counted(ptr16_data)";
    }
    return "";
}

static int apply_edit(GuiProject *project, const char *target_name, const char *label_name,
                      const char *kind, const char *spec, const char *doc, char **selected_name,
                      char **error_text)
{
    LabelInfo *label = find_label(project, target_name);
    unsigned target_bank = 0;
    unsigned target_addr = 0;
    char key[32];
    char other_key[32];
    const char *effective_kind = (kind && *kind) ? kind : (label ? label->block_kind : "");
    const char *class_spec =
        spec && *spec ? spec : default_class_spec(effective_kind ? effective_kind : "");

    if (label) {
        target_bank = label->bank;
        target_addr = label->addr;
    } else if (!parse_target_key_local(target_name, &target_bank, &target_addr)) {
        size_t len = strlen(target_name) + 32u;
        *error_text = xmalloc(len);
        snprintf(*error_text, len, "unknown target: %s", target_name);
        return 0;
    }
    if (label) {
        format_label_key(label, key, sizeof(key));
    } else {
        format_target_key(target_bank, target_addr, key, sizeof(key));
    }
    if (label_name && *label_name &&
        label_name_conflicts(project, label_name, target_bank, target_addr, other_key,
                             sizeof(other_key))) {
        size_t len = strlen(label_name) + strlen(other_key) + 64u;

        *error_text = xmalloc(len);
        snprintf(*error_text, len, "label '%s' already exists at %s", label_name, other_key);
        return 0;
    }
    if (label_name && *label_name) {
        set_override(&project->label_overrides, key, label_name);
        *selected_name = dup_string_local(label_name);
    } else if (!label) {
        *selected_name = dup_string_local(key);
    } else {
        *selected_name = dup_string_local(label->name);
    }

    remove_override(&project->inline_overrides, key);
    remove_override(&project->entry_overrides, key);
    remove_override(&project->data_overrides, key);
    remove_override(&project->table_overrides, key);
    remove_override(&project->routine_doc_overrides, key);
    remove_override(&project->table_doc_overrides, key);
    if (effective_kind && *effective_kind) {
        if (strcmp(effective_kind, "code") == 0) {
            set_override(&project->entry_overrides, key, "code");
            if (spec && *spec) {
                set_override(&project->inline_overrides, key, spec);
            }
            if (doc && *doc) {
                set_override(&project->routine_doc_overrides, key, doc);
            }
        } else if (strcmp(effective_kind, "string") == 0 || strcmp(effective_kind, "data") == 0) {
            set_override(&project->data_overrides, key, class_spec);
        } else if (strcmp(effective_kind, "table") == 0) {
            set_override(&project->table_overrides, key, class_spec);
            if (doc && *doc) {
                set_override(&project->table_doc_overrides, key, doc);
            }
        } else {
            *error_text = dup_string_local("unsupported kind");
            return 0;
        }
    }
    write_overlay_file(project);
    rebuild_project(project);
    return 1;
}

static int clear_edit_overrides(GuiProject *project, const char *target_name, const char *scope,
                                char **selected_name, char **error_text)
{
    LabelInfo *label = find_label(project, target_name);
    unsigned target_bank = 0;
    unsigned target_addr = 0;
    char key[32];

    if (label) {
        target_bank = label->bank;
        target_addr = label->addr;
    } else if (!parse_target_key_local(target_name, &target_bank, &target_addr)) {
        size_t len = strlen(target_name) + 32u;

        *error_text = xmalloc(len);
        snprintf(*error_text, len, "unknown target: %s", target_name);
        return 0;
    }
    format_target_key(target_bank, target_addr, key, sizeof(key));
    if (!scope || strcmp(scope, "all") == 0) {
        remove_override(&project->label_overrides, key);
        remove_override(&project->inline_overrides, key);
        remove_override(&project->entry_overrides, key);
        remove_override(&project->data_overrides, key);
        remove_override(&project->table_overrides, key);
        remove_override(&project->routine_doc_overrides, key);
        remove_override(&project->table_doc_overrides, key);
    } else if (strcmp(scope, "label") == 0) {
        remove_override(&project->label_overrides, key);
    } else if (strcmp(scope, "class") == 0) {
        remove_override(&project->inline_overrides, key);
        remove_override(&project->entry_overrides, key);
        remove_override(&project->data_overrides, key);
        remove_override(&project->table_overrides, key);
    } else if (strcmp(scope, "doc") == 0) {
        remove_override(&project->routine_doc_overrides, key);
        remove_override(&project->table_doc_overrides, key);
    } else {
        *error_text = dup_string_local("unsupported clear scope");
        return 0;
    }
    *selected_name = dup_string_local(key);
    write_overlay_file(project);
    rebuild_project(project);
    return 1;
}

static void send_headers(FILE *out, const char *status, const char *content_type)
{
    fprintf(out, "HTTP/1.1 %s\r\n", status);
    fprintf(out, "Content-Type: %s; charset=utf-8\r\n", content_type);
    fprintf(out, "Connection: close\r\n\r\n");
}

static void send_not_found(FILE *out)
{
    send_headers(out, "404 Not Found", "text/plain");
    fputs("not found", out);
}

static char *url_decode_in_place(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src) {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3];

            hex[0] = src[1];
            hex[1] = src[2];
            hex[2] = '\0';
            *dst++ = (char)strtoul(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return text;
}

static char *query_value(char *target, const char *key)
{
    char *query = strchr(target, '?');

    if (!query) {
        return NULL;
    }
    query++;
    while (*query) {
        char *next = strchr(query, '&');
        char *eq = strchr(query, '=');

        if (next) {
            *next = '\0';
        }
        if (eq) {
            char *value_ptr;

            *eq = '\0';
            if (strcmp(query, key) == 0) {
                value_ptr = url_decode_in_place(eq + 1);
                *eq = '=';
                if (next) {
                    *next = '&';
                }
                return value_ptr;
            }
            *eq = '=';
        }
        if (!next) {
            break;
        }
        *next = '&';
        query = next + 1;
    }
    return NULL;
}

static char *query_value_owned(const char *target, const char *key)
{
    const char *query = strchr(target, '?');
    size_t key_len = strlen(key);

    if (!query) {
        return NULL;
    }
    query++;
    while (*query) {
        const char *next = strchr(query, '&');
        const char *eq = strchr(query, '=');

        if (!eq || (next && eq > next)) {
            if (!next) {
                break;
            }
            query = next + 1;
            continue;
        }
        if ((size_t)(eq - query) == key_len && strncmp(query, key, key_len) == 0) {
            size_t raw_len = next ? (size_t)(next - (eq + 1)) : strlen(eq + 1);
            char *value = xmalloc(raw_len + 1u);

            memcpy(value, eq + 1, raw_len);
            value[raw_len] = '\0';
            return url_decode_in_place(value);
        }
        if (!next) {
            break;
        }
        query = next + 1;
    }
    return NULL;
}

static void send_index(FILE *out, GuiProject *project)
{
    size_t i;

    send_headers(out, "200 OK", "application/json");
    fputs("{\"overlay_path\":", out);
    json_write_string(out, project->overlay_path ? project->overlay_path : "");
    fputs(",\"labels\":[", out);
    for (i = 0; i < project->label_count; i++) {
        const LabelInfo *label = &project->labels[i];
        char key[32];
        const char *overlay_label;
        const char *overlay_inline;
        const char *overlay_entry;
        const char *overlay_data;
        const char *overlay_table;
        const char *overlay_routine_doc;
        const char *overlay_table_doc;

        if (i > 0) {
            fputc(',', out);
        }
        format_target_key(label->bank, label->addr, key, sizeof(key));
        overlay_label = override_value(&project->label_overrides, key);
        overlay_inline = override_value(&project->inline_overrides, key);
        overlay_entry = override_value(&project->entry_overrides, key);
        overlay_data = override_value(&project->data_overrides, key);
        overlay_table = override_value(&project->table_overrides, key);
        overlay_routine_doc = override_value(&project->routine_doc_overrides, key);
        overlay_table_doc = override_value(&project->table_doc_overrides, key);
        fputc('{', out);
        fputs("\"name\":", out);
        json_write_string(out, label->name);
        fprintf(out, ",\"bank\":%u,\"addr\":%u,\"rom_offset\":%u,\"line_index\":%lu,\"kind\":",
                label->bank, label->addr, label->rom_offset, (unsigned long)label->line_index);
        json_write_string(out, label->kind);
        fputs(",\"block_kind\":", out);
        json_write_string(out, label->block_kind);
        fputs(",\"spec\":", out);
        json_write_string(out, label->spec);
        fputs(",\"doc\":", out);
        json_write_string(out, label->doc);
        fputs(",\"overlay_label\":", out);
        json_write_string(out, overlay_label ? overlay_label : "");
        fputs(",\"overlay_inline\":", out);
        json_write_string(out, overlay_inline ? overlay_inline : "");
        fputs(",\"overlay_entry\":", out);
        json_write_string(out, overlay_entry ? overlay_entry : "");
        fputs(",\"overlay_data\":", out);
        json_write_string(out, overlay_data ? overlay_data : "");
        fputs(",\"overlay_table\":", out);
        json_write_string(out, overlay_table ? overlay_table : "");
        fputs(",\"overlay_routine_doc\":", out);
        json_write_string(out, overlay_routine_doc ? overlay_routine_doc : "");
        fputs(",\"overlay_table_doc\":", out);
        json_write_string(out, overlay_table_doc ? overlay_table_doc : "");
        fputs(",\"details\":", out);
        json_write_string(out, label->details);
        fputc('}', out);
    }
    fputs("],\"tables\":[", out);
    for (i = 0; i < project->table_count; i++) {
        const TableInfo *table = &project->tables[i];

        if (i > 0) {
            fputc(',', out);
        }
        fprintf(out,
                "{\"name\":");
        json_write_string(out, table->name);
        fprintf(out, ",\"bank\":%u,\"addr\":%u,\"rom_offset\":%u,\"line_index\":%lu}",
                table->bank, table->addr, table->rom_offset, (unsigned long)table->line_index);
    }
    fputs("],\"transitions\":[", out);
    for (i = 0; i < project->data_edge_count; i++) {
        const DataEdgeInfo *edge = &project->data_edges[i];
        const char *target_name = label_name_at_address(project, edge->bank, edge->addr);

        if (i > 0) {
            fputc(',', out);
        }
        fputs("{\"kind\":", out);
        json_write_string(out, edge->kind);
        fputs(",\"target_name\":", out);
        json_write_string(out, target_name ? target_name : edge->target_name);
        fprintf(out, ",\"bank\":%u,\"addr\":%u,\"rom_offset\":%u,\"line_index\":%lu}",
                edge->bank, edge->addr, edge->rom_offset, (unsigned long)edge->line_index);
    }
    fputs("]}", out);
}

static void send_xrefs(FILE *out, GuiProject *project, const char *label_name)
{
    size_t i;
    int first = 1;

    send_headers(out, "200 OK", "application/json");
    fputs("{\"xrefs\":[", out);
    for (i = 0; i < project->xref_count; i++) {
        const XrefInfo *xref = &project->xrefs[i];

        if (strcmp(xref->target_name, label_name) != 0) {
            continue;
        }
        if (!first) {
            fputc(',', out);
        }
        first = 0;
        fputs("{\"kind\":", out);
        json_write_string(out, xref->kind);
        fputs(",\"source\":", out);
        json_write_string(out, xref->source);
        fputc('}', out);
    }
    fputs("]}", out);
}

static void format_hexdump_line(char *out, size_t out_size, unsigned rom_offset,
                                const uint8_t *data, size_t count)
{
    size_t i;
    size_t pos = 0;

    pos += (size_t)snprintf(out + pos, out_size - pos, "%06x  ", rom_offset);
    for (i = 0; i < 16; i++) {
        if (i < count) {
            pos += (size_t)snprintf(out + pos, out_size - pos, "%02x ", data[i]);
        } else {
            pos += (size_t)snprintf(out + pos, out_size - pos, "   ");
        }
    }
    pos += (size_t)snprintf(out + pos, out_size - pos, " |");
    for (i = 0; i < count; i++) {
        out[pos++] = isprint(data[i]) ? (char)data[i] : '.';
    }
    out[pos++] = '|';
    out[pos++] = '\n';
    out[pos] = '\0';
}

static void send_hex(FILE *out, GuiProject *project, const char *label_name)
{
    LabelInfo *label = find_label(project, label_name);
    unsigned start;
    unsigned end;
    unsigned offset;
    char line[128];

    if (!label) {
        send_not_found(out);
        return;
    }
    send_headers(out, "200 OK", "text/plain");
    start = label->rom_offset >= 64u ? label->rom_offset - 64u : 0u;
    start &= ~0x0fu;
    end = label->rom_offset + 64u;
    if (end > project->rom.size) {
        end = (unsigned)project->rom.size;
    }
    for (offset = start; offset < end; offset += 16u) {
        size_t remaining = end - offset;
        size_t count = remaining < 16u ? remaining : 16u;

        format_hexdump_line(line, sizeof(line), offset, project->rom.data + offset, count);
        fputs(line, out);
    }
}

static void send_asm(FILE *out, GuiProject *project)
{
    send_headers(out, "200 OK", "text/plain");
    fputs(project->asm_text, out);
}

static void send_html(FILE *out)
{
    send_headers(out, "200 OK", "text/html");
    fwrite(apexgui_html, 1u, apexgui_html_len, out);
}

static void send_edit_result(FILE *out, int ok, const char *selected, const char *overlay_path,
                             const char *error_text)
{
    send_headers(out, ok ? "200 OK" : "400 Bad Request", "application/json");
    fprintf(out, "{\"ok\":%s", ok ? "true" : "false");
    if (selected) {
        fputs(",\"selected\":", out);
        json_write_string(out, selected);
    }
    if (overlay_path) {
        fputs(",\"overlay_path\":", out);
        json_write_string(out, overlay_path);
    }
    if (error_text) {
        fputs(",\"error\":", out);
        json_write_string(out, error_text);
    }
    fputs("}", out);
}

static void serve_client(int client_fd, GuiProject *project)
{
    char request[4096];
    ssize_t len = read(client_fd, request, sizeof(request) - 1);
    FILE *out;
    char method[16];
    char target[2048];

    if (len <= 0) {
        close(client_fd);
        return;
    }
    request[len] = '\0';
    if (sscanf(request, "%15s %2047s", method, target) != 2) {
        close(client_fd);
        return;
    }
    out = fdopen(dup(client_fd), "w");
    if (!out) {
        close(client_fd);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_headers(out, "405 Method Not Allowed", "text/plain");
        fputs("method not allowed", out);
    } else if (strcmp(target, "/") == 0) {
        send_html(out);
    } else if (strncmp(target, "/api/index", 10) == 0) {
        send_index(out, project);
    } else if (strncmp(target, "/api/asm", 8) == 0) {
        send_asm(out, project);
    } else if (strncmp(target, "/api/xrefs", 10) == 0) {
        char *label_name = query_value(target, "label");

        if (!label_name) {
            send_not_found(out);
        } else {
            send_xrefs(out, project, label_name);
        }
    } else if (strncmp(target, "/api/hex", 8) == 0) {
        char *label_name = query_value(target, "label");

        if (!label_name) {
            send_not_found(out);
        } else {
            send_hex(out, project, label_name);
        }
    } else if (strncmp(target, "/api/edit", 9) == 0) {
        char *target_name = query_value_owned(target, "target");
        char *action = query_value_owned(target, "action");
        char *scope = query_value_owned(target, "scope");
        char *label_name = query_value_owned(target, "label");
        char *kind = query_value_owned(target, "kind");
        char *spec = query_value_owned(target, "spec");
        char *doc = query_value_owned(target, "doc");
        char *selected = NULL;
        char *error_text = NULL;

        if (!target_name || !*target_name) {
            send_edit_result(out, 0, NULL, project->overlay_path, "missing target");
        } else if (action && strcmp(action, "clear") == 0) {
            if (clear_edit_overrides(project, target_name, scope ? scope : "all", &selected,
                                     &error_text)) {
                send_edit_result(out, 1, selected, project->overlay_path, NULL);
            } else {
                send_edit_result(out, 0, selected, project->overlay_path,
                                 error_text ? error_text : "clear failed");
            }
        } else if (apply_edit(project, target_name, label_name ? label_name : "",
                              kind ? kind : "", spec ? spec : "", doc ? doc : "", &selected,
                              &error_text)) {
            send_edit_result(out, 1, selected, project->overlay_path, NULL);
        } else {
            send_edit_result(out, 0, selected, project->overlay_path,
                             error_text ? error_text : "edit failed");
        }
        free(target_name);
        free(action);
        free(scope);
        free(label_name);
        free(kind);
        free(spec);
        free(doc);
        free(selected);
        free(error_text);
    } else {
        send_not_found(out);
    }
    fclose(out);
    close(client_fd);
}

static int open_server_socket(const char *host, unsigned short port)
{
    int fd = -1;
    int opt = 1;
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *it;
    char port_text[16];
    int gai_rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    gai_rc = getaddrinfo(host, port_text, &hints, &results);
    if (gai_rc != 0) {
        die("failed to resolve host %s: %s", host, gai_strerror(gai_rc));
    }
    for (it = results; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        die("failed to bind socket on %s:%u", host, (unsigned)port);
    }
    if (listen(fd, 16) != 0) {
        die("failed to listen on socket: %s", strerror(errno));
    }
    return fd;
}

static unsigned short socket_port(int fd)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        die("failed to query socket port: %s", strerror(errno));
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
    }
    die("unsupported socket family");
    return 0;
}

static void usage(void)
{
    die("usage: apexgui [--host <host>] [--port <port>] <input-rom> [config.ini]");
}

int main(int argc, char **argv)
{
    GuiProject project;
    const char *host = "127.0.0.1";
    unsigned short port = 8123;
    int argi = 1;
    int server_fd;

    memset(&project, 0, sizeof(project));
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--port") == 0) {
            unsigned parsed_port = 0;

            if (argi + 1 >= argc || !parse_hex_value(argv[argi + 1], &parsed_port) ||
                parsed_port > 65535u) {
                usage();
            }
            port = (unsigned short)parsed_port;
            argi += 2;
        } else if (strcmp(argv[argi], "--host") == 0) {
            if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
                usage();
            }
            host = argv[argi + 1];
            argi += 2;
        } else {
            usage();
        }
    }
    if (argc - argi != 1 && argc - argi != 2) {
        usage();
    }

    build_project(&project, argv[argi], argc - argi == 2 ? argv[argi + 1] : NULL);
    server_fd = open_server_socket(host, port);
    port = socket_port(server_fd);
    printf("Apex GUI listening on http://%s%s%s:%u/\n", strchr(host, ':') ? "[" : "", host,
           strchr(host, ':') ? "]" : "", port);
    fflush(stdout);
    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("accept failed: %s", strerror(errno));
        }
        serve_client(client_fd, &project);
    }

    free_project(&project);
    return 0;
}
