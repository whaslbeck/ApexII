#include "apexdmd.h"

#include "apex.h"
#include "apex_analysis.h"
#include "apex_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int starts_with_ci(const char *text, const char *prefix)
{
    while (*prefix) {
        unsigned char a = (unsigned char)*text;
        unsigned char b = (unsigned char)*prefix;
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
        text++;
        prefix++;
    }
    return 1;
}

static int parse_target_address(const char *input, uint8_t *bank, uint32_t *cpu_addr)
{
    unsigned parsed_bank;
    unsigned parsed_cpu;

    while (*input == ' ' || *input == '\t') {
        input++;
    }
    if (sscanf(input, "B%x_A%x", &parsed_bank, &parsed_cpu) == 2) {
        *bank = (uint8_t)parsed_bank;
        *cpu_addr = parsed_cpu;
        return 1;
    }
    if (starts_with_ci(input, "0x") && sscanf(input + 2, "%x", &parsed_cpu) == 1) {
        *bank = 0xffu;
        *cpu_addr = parsed_cpu;
        return 1;
    }
    return 0;
}

static const uint8_t *locate_asset(const Buffer *rom, uint8_t bank, uint32_t addr, size_t *size_out)
{
    size_t paged_size;
    size_t banks;
    size_t offset;
    int bank_index;

    paged_size = rom->size >= APEX_SYSTEM_SIZE ? rom->size - APEX_SYSTEM_SIZE : 0u;
    banks = paged_size / APEX_BANK_SIZE;

    if (bank == 0xffu) {
        if (addr < APEX_SYSTEM_ORG || addr >= APEX_SYSTEM_ORG + APEX_SYSTEM_SIZE) {
            return NULL;
        }
        offset = paged_size + (size_t)(addr - APEX_SYSTEM_ORG);
    } else {
        if (addr < APEX_PAGED_ORG || addr >= APEX_PAGED_ORG + APEX_BANK_SIZE) {
            return NULL;
        }
        bank_index = bank_index_for_id(rom->data, banks, bank);
        if (bank_index < 0) {
            return NULL;
        }
        offset = (size_t)bank_index * APEX_BANK_SIZE + (size_t)(addr - APEX_PAGED_ORG);
    }

    if (offset >= rom->size) {
        return NULL;
    }
    if (size_out) {
        *size_out = rom->size - offset;
    }
    return rom->data + offset;
}

static void ensure_dir(const char *path)
{
#ifdef _WIN32
    if (mkdir(path) != 0 && errno != EEXIST) {
#else
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
#endif
        die("failed to create directory %s", path);
    }
}

static const TableDef *find_table(const TableDefs *tables, uint8_t bank, uint32_t addr)
{
    return table_def_at(bank, addr, tables);
}

static void format_address(char *out, size_t out_size, uint8_t bank, uint32_t addr)
{
    if (bank == 0xffu) {
        snprintf(out, out_size, "Bff_A%04x", (unsigned)addr & 0xffffu);
    } else {
        snprintf(out, out_size, "B%02x_A%04x", bank, (unsigned)addr & 0xffffu);
    }
}

static void run_table_mode(const char *rom_path, const char *config_path, const char *table_text,
                           const char *out_dir)
{
    Buffer rom;
    InlineSignatures sigs = {0};
    ConfigLabels labels = {0};
    ConfigEntries entries = {0};
    TableDefs tables = {0};
    SchemaDefs schemas = {0};
    ConfigDocs docs = {0};
    ConfigSymbols symbols = {0};
    DataRanges data_ranges = {0};
    ConfigOptions options = {0};
    uint8_t table_bank = 0xffu;
    uint32_t table_addr = 0u;
    const TableDef *table;
    const uint8_t *table_src;
    size_t table_size = 0u;
    size_t row;
    FILE *summary;
    size_t same_bank_adjacent = 0u;
    size_t same_type_adjacent = 0u;

    if (!parse_target_address(table_text, &table_bank, &table_addr)) {
        die("invalid table address: %s", table_text);
    }

    {
        ConfigTypes types = {0};

        load_config(config_path, &sigs, &labels, &entries, &tables, &schemas, &docs,
                    &symbols, &data_ranges, &options, &types, NULL, NULL, NULL);
        free_config_types(&types);
    }
    table = find_table(&tables, table_bank, table_addr);
    if (!table) {
        die("table not found in config: %s", table_text);
    }
    if (table->has_header || table->rows == 0u || table->schema.count != 1u ||
        (table->schema.items[0].kind != TABLE_FAR_DATA &&
         table->schema.items[0].kind != TABLE_FAR_DMD_FULLFRAME) ||
        table->schema.items[0].count != 1u) {
        die("table mode currently supports only rows[n](far_data|far_dmd_fullframe) without header");
    }

    rom = read_file(rom_path);
    table_src = locate_asset(&rom, table_bank, table_addr, &table_size);
    if (!table_src) {
        die("table address not mapped in ROM: %s", table_text);
    }
    if (table_size < table->rows * 3u) {
        die("table at %s exceeds ROM size", table_text);
    }

    ensure_dir(out_dir);
    {
        char summary_path[1024];
        snprintf(summary_path, sizeof(summary_path), "%s/summary.tsv", out_dir);
        summary = fopen(summary_path, "wb");
        if (!summary) {
            die("failed to write %s", summary_path);
        }
    }
    fprintf(summary, "index\taddress\ttype\tconsumed\tsingle\tpair_next\n");

    for (row = 0; row < table->rows; row++) {
        size_t entry_off = row * 3u;
        uint16_t target_addr = (uint16_t)(((unsigned)table_src[entry_off] << 8) | table_src[entry_off + 1u]);
        uint8_t target_bank = table_src[entry_off + 2u];
        const uint8_t *src;
        size_t src_size = 0u;
        size_t consumed = 0u;
        uint8_t type = 0u;
        uint8_t plane[APEX_DMD_PAGE_BYTES];
        char addr_buf[32];
        char single_name[128];
        char single_path[1024];
        char pair_name[160] = "-";

        src = locate_asset(&rom, target_bank, target_addr, &src_size);
        if (!src) {
            die("target %lu not mapped in ROM", (unsigned long)row);
        }
        if (!apexdmd_decode_fullframe(src, src_size, plane, &consumed, &type)) {
            die("failed to decode target %lu", (unsigned long)row);
        }
        format_address(addr_buf, sizeof(addr_buf), target_bank, target_addr);
        snprintf(single_name, sizeof(single_name), "row%03lu_%s.pbm", (unsigned long)row, addr_buf);
        snprintf(single_path, sizeof(single_path), "%s/%s", out_dir, single_name);
        if (!apexdmd_write_plane_pbm(single_path, plane)) {
            die("failed to write %s", single_path);
        }

        if (row + 1u < table->rows) {
            size_t next_off = (row + 1u) * 3u;
            uint16_t next_addr = (uint16_t)(((unsigned)table_src[next_off] << 8) | table_src[next_off + 1u]);
            uint8_t next_bank = table_src[next_off + 2u];
            const uint8_t *src1;
            size_t src1_size = 0u;
            size_t consumed1 = 0u;
            uint8_t type1 = 0u;
            uint8_t plane1[APEX_DMD_PAGE_BYTES];

            src1 = locate_asset(&rom, next_bank, next_addr, &src1_size);
            if (src1 && apexdmd_decode_fullframe(src1, src1_size, plane1, &consumed1, &type1)) {
                char next_addr_buf[32];
                char pair_path[1024];

                format_address(next_addr_buf, sizeof(next_addr_buf), next_bank, next_addr);
                snprintf(pair_name, sizeof(pair_name), "pair%03lu_%03lu_%s_%s.pgm",
                         (unsigned long)row, (unsigned long)(row + 1u), addr_buf, next_addr_buf);
                snprintf(pair_path, sizeof(pair_path), "%s/%s", out_dir, pair_name);
                if (!apexdmd_write_pair_pgm(pair_path, plane, plane1)) {
                    die("failed to write %s", pair_path);
                }
                if (next_bank == target_bank) {
                    same_bank_adjacent++;
                }
                if (type1 == type) {
                    same_type_adjacent++;
                }
            }
        }

        fprintf(summary, "%lu\t%s\t0x%02x\t%lu\t%s\t%s\n", (unsigned long)row, addr_buf,
                (unsigned)type, (unsigned long)consumed, single_name, pair_name);
    }

    fprintf(summary, "# rows\t%lu\n", (unsigned long)table->rows);
    fprintf(summary, "# adjacent_same_bank\t%lu\n", (unsigned long)same_bank_adjacent);
    fprintf(summary, "# adjacent_same_type\t%lu\n", (unsigned long)same_type_adjacent);
    fclose(summary);
    free(rom.data);
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s <rom> <Bxx_Ayyyy|0xhhhh> <out.pbm>\n", argv0);
    fprintf(stderr, "  %s --pair <rom> <addr0> <addr1> <out.pgm>\n", argv0);
    fprintf(stderr, "  %s --table <rom> <config.ini> <table_addr> <outdir>\n", argv0);
}

int main(int argc, char **argv)
{
    Buffer rom;
    const uint8_t *src0;
    const uint8_t *src1;
    size_t src0_size = 0u;
    size_t src1_size = 0u;
    size_t consumed0 = 0u;
    size_t consumed1 = 0u;
    uint8_t type0 = 0u;
    uint8_t type1 = 0u;
    uint8_t bank0 = 0xffu;
    uint8_t bank1 = 0xffu;
    uint32_t addr0 = 0u;
    uint32_t addr1 = 0u;
    uint8_t plane0[APEX_DMD_PAGE_BYTES];
    uint8_t plane1[APEX_DMD_PAGE_BYTES];
    int pair_mode = 0;

    if (argc == 6 && strcmp(argv[1], "--pair") == 0) {
        pair_mode = 1;
    } else if (argc == 6 && strcmp(argv[1], "--table") == 0) {
        run_table_mode(argv[2], argv[3], argv[4], argv[5]);
        return 0;
    } else if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    rom = read_file(argv[pair_mode ? 2 : 1]);

    if (!pair_mode) {
        if (!parse_target_address(argv[2], &bank0, &addr0)) {
            die("invalid address: %s", argv[2]);
        }
        src0 = locate_asset(&rom, bank0, addr0, &src0_size);
        if (!src0) {
            die("address not mapped in ROM: %s", argv[2]);
        }
        if (!apexdmd_decode_fullframe(src0, src0_size, plane0, &consumed0, &type0)) {
            die("failed to decode fullframe DMD at %s (type=0x%02x)", argv[2],
                src0_size ? (unsigned)(src0[0] & 0x0fu) : 0u);
        }
        if (!apexdmd_write_plane_pbm(argv[3], plane0)) {
            die("failed to write %s", argv[3]);
        }
        fprintf(stderr, "decoded %s type=0x%02x bytes=%lu -> %s\n", argv[2], (unsigned)type0,
                (unsigned long)consumed0, argv[3]);
        free(rom.data);
        return 0;
    }

    if (!parse_target_address(argv[3], &bank0, &addr0)) {
        die("invalid address: %s", argv[3]);
    }
    if (!parse_target_address(argv[4], &bank1, &addr1)) {
        die("invalid address: %s", argv[4]);
    }

    src0 = locate_asset(&rom, bank0, addr0, &src0_size);
    src1 = locate_asset(&rom, bank1, addr1, &src1_size);
    if (!src0) {
        die("address not mapped in ROM: %s", argv[3]);
    }
    if (!src1) {
        die("address not mapped in ROM: %s", argv[4]);
    }
    if (!apexdmd_decode_fullframe(src0, src0_size, plane0, &consumed0, &type0)) {
        die("failed to decode fullframe DMD at %s (type=0x%02x)", argv[3],
            src0_size ? (unsigned)(src0[0] & 0x0fu) : 0u);
    }
    if (!apexdmd_decode_fullframe(src1, src1_size, plane1, &consumed1, &type1)) {
        die("failed to decode fullframe DMD at %s (type=0x%02x)", argv[4],
            src1_size ? (unsigned)(src1[0] & 0x0fu) : 0u);
    }
    if (!apexdmd_write_pair_pgm(argv[5], plane0, plane1)) {
        die("failed to write %s", argv[5]);
    }
    fprintf(stderr,
            "decoded pair %s(type=0x%02x bytes=%lu) + %s(type=0x%02x bytes=%lu) -> %s\n",
            argv[3], (unsigned)type0, (unsigned long)consumed0, argv[4], (unsigned)type1,
            (unsigned long)consumed1, argv[5]);

    free(rom.data);
    return 0;
}
