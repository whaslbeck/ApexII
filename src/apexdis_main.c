#include "apex.h"
#include "apexdis_api.h"

#include <string.h>

static void usage(void)
{
    die("usage: apexdis [--xref] [--explain] <input-rom> <output-asm> [config.ini]");
}

int main(int argc, char **argv)
{
    ApexDisOptions options;
    int argi = 1;

    memset(&options, 0, sizeof(options));
    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--xref") == 0) {
            options.emit_xrefs = 1;
        } else if (strcmp(argv[argi], "--explain") == 0) {
            options.emit_explain = 1;
        } else {
            usage();
        }
        argi++;
    }
    if (argc - argi != 2 && argc - argi != 3) {
        usage();
    }

    options.input_path = argv[argi];
    options.output_path = argv[argi + 1];
    options.config_path = argc - argi == 3 ? argv[argi + 2] : NULL;
    return apexdis_run(&options);
}
