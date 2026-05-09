#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define BANK_SIZE 0x4000
#define MAX_STRING_LEN 50
#define MAX_TABLES 20000

typedef struct {
    uint32_t offset;
    uint8_t bank_id;
    uint16_t addr;
} FoundTable;

FoundTable found_text_tables[MAX_TABLES];
int num_found_text_tables = 0;
uint8_t rom_bank_ids[256]; // IDs read from first byte of each 16KB physical bank

bool is_valid_string(const uint8_t *rom, uint32_t size, uint32_t offset) {
    if (offset >= size) return false;
    uint32_t len = 0;
    while (offset + len < size && len <= MAX_STRING_LEN) {
        uint8_t c = rom[offset + len];
        if (c == 0x00) {
            return len > 0;
        }
        if (c < 0x20 || c > 0x7F) {
            return false;
        }
        len++;
    }
    return false;
}

uint32_t translate_ptr16(uint16_t ptr, int phys_bank, int total_banks) {
    if (ptr >= 0x4000 && ptr <= 0x7FFF) {
        return (uint32_t)phys_bank * BANK_SIZE + (ptr - 0x4000);
    } else if (ptr >= 0x8000) {
        int system_start_bank = total_banks - 2;
        if (system_start_bank < 0) system_start_bank = 0;
        // 0x8000 is start of the second-to-last bank
        if (ptr < 0xC000) {
             return (uint32_t)system_start_bank * BANK_SIZE + (ptr - 0x8000);
        } else {
             return (uint32_t)(system_start_bank + 1) * BANK_SIZE + (ptr - 0xC000);
        }
    }
    return 0xFFFFFFFF;
}

uint32_t find_offset_by_far_ptr(uint8_t bank_id, uint16_t addr, int total_banks) {
    // Search which physical bank has this ID
    // Note: addr is usually 0x4000-0x7FFF for banked data
    for (int b = 0; b < total_banks; b++) {
        if (rom_bank_ids[b] == bank_id) {
            if (addr >= 0x4000 && addr <= 0x7FFF) {
                return (uint32_t)b * BANK_SIZE + (addr - 0x4000);
            }
        }
    }
    // Fixed area check: If addr is 0x8000+ and bank_id is 0xFF (or matching fixed bank ID)
    if (addr >= 0x8000) {
        // Based on instructions, system area bank_id is 0xff
        if (bank_id == 0xFF) {
            return translate_ptr16(addr, 0, total_banks);
        }
    }
    return 0xFFFFFFFF;
}

bool is_text_table_at(const uint8_t *rom, uint32_t size, uint32_t offset, int total_banks) {
    if (offset + 3 > size) return false;
    uint16_t num_rows = (rom[offset] << 8) | rom[offset + 1];
    uint8_t width = rom[offset + 2];
    if (width != 2 || num_rows == 0 || num_rows > 1000) return false;
    uint32_t data_offset = offset + 3;
    if (data_offset + num_rows * 2 > size) return false;
    int phys_bank = offset / BANK_SIZE;
    for (int i = 0; i < num_rows; i++) {
        uint16_t ptr = (rom[data_offset + i * 2] << 8) | rom[data_offset + i * 2 + 1];
        uint32_t str_offset = translate_ptr16(ptr, phys_bank, total_banks);
        if (str_offset == 0xFFFFFFFF || !is_valid_string(rom, size, str_offset)) {
            return false;
        }
    }
    return true;
}

bool is_known_text_table(uint32_t offset) {
    for (int i = 0; i < num_found_text_tables; i++) {
        if (found_text_tables[i].offset == offset) return true;
    }
    return false;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom_file>\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(size);
    if (!rom || fread(rom, 1, size, f) != size) {
        perror("fread/malloc");
        if(rom) free(rom);
        fclose(f);
        return 1;
    }
    fclose(f);
    int total_banks = size / BANK_SIZE;
    for (int b = 0; b < total_banks; b++) {
        rom_bank_ids[b] = rom[b * BANK_SIZE];
    }

    // Step 1: Text Tables
    for (uint32_t i = 0; i <= size - 3; i++) {
        if (is_text_table_at(rom, size, i, total_banks)) {
            int phys_bank = i / BANK_SIZE;
            uint8_t bank_id = (phys_bank >= total_banks - 2) ? 0xFF : rom_bank_ids[phys_bank];
            uint16_t addr = 0x4000 + (i % BANK_SIZE);
            if (phys_bank >= total_banks - 2) {
                addr = (phys_bank == total_banks - 2) ? 0x8000 + (i % BANK_SIZE) : 0xC000 + (i % BANK_SIZE);
            }
            if (num_found_text_tables < MAX_TABLES) {
                found_text_tables[num_found_text_tables++] = (FoundTable){i, bank_id, addr};
            }
            uint16_t row_count = (rom[i] << 8) | rom[i + 1];
            printf("B%02x_A%04x = counted(ptr16_string) ; %d rows\n", bank_id, addr, row_count);
        }
    }

    // Step 2: Far-Pointer Tables
    for (uint32_t i = 0; i <= size - 9; i++) {
        bool match = true;
        for (int j = 0; j < 3; j++) {
            // "Format is ALWAYS 16-Bit Address (local to bank) and 8-Bit BANK_ID"
            uint16_t addr = (rom[i + j * 3] << 8) | rom[i + j * 3 + 1];
            uint8_t bank_id = rom[i + j * 3 + 2];
            uint32_t target_off = find_offset_by_far_ptr(bank_id, addr, total_banks);
            if (target_off == 0xFFFFFFFF || !is_known_text_table(target_off)) {
                match = false;
                break;
            }
        }
        if (match) {
            int phys_bank = i / BANK_SIZE;
            uint8_t bank_id = (phys_bank >= total_banks - 2) ? 0xFF : rom_bank_ids[phys_bank];
            uint16_t addr = 0x4000 + (i % BANK_SIZE);
            if (phys_bank >= total_banks - 2) {
                addr = (phys_bank == total_banks - 2) ? 0x8000 + (i % BANK_SIZE) : 0xC000 + (i % BANK_SIZE);
            }
            printf("B%02x_A%04x = rows[3](far_data)\n", bank_id, addr);
            i += 8;
        }
    }
    free(rom);
    return 0;
}
