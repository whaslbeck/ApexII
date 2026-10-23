/* apexasm — assemble ApexII .asm files into a ROM image (see apex_asm.h). */
#include "apex.h"
#include "apex_asm.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    uint8_t *rom;
    size_t size;
    char err[640];

    if (argc < 3) {
        fputs("usage: apexasm <output-rom> <input-asm> [input-asm ...]\n", stderr);
        return 2; /* usage error, like the other tools */
    }
    if (apex_asm_files((const char *const *)(argv + 2), (size_t)(argc - 2), &rom, &size, err,
                       sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    write_file(argv[1], rom, size);
    free(rom);
    return 0;
}
