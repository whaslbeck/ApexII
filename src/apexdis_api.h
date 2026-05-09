#ifndef APEXDIS_API_H
#define APEXDIS_API_H

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *config_path;
    int emit_xrefs;
    int emit_explain;
} ApexDisOptions;

int apexdis_run(const ApexDisOptions *options);

#endif
