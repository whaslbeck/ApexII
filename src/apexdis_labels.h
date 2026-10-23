#ifndef APEXDIS_LABELS_H
#define APEXDIS_LABELS_H

/* Label passes of the disassembler (apexdis.c) that the project analysis
   (apex_project.c) runs as well. */

#include "apex_analysis.h"
#include "apex_config.h"

#include <stddef.h>
#include <stdint.h>

void build_system_labels(const uint8_t *data, const VectorInfo *vectors, size_t vector_count,
                         InlineSignatures *inline_sigs, const uint8_t *paged_rom, size_t banks,
                         LabelSet *bank_labels, LabelSet *labels, const ConfigLabels *config_labels,
                         const ConfigEntries *config_entries, const DataRanges *data_ranges,
                         const ConfigOptions *options, ReferenceSet *refs,
                         const ConfigEntries *ref_exclusions, const ConfigEntries *literals,
                         const ConfigEntries *far_imms);
void collect_bank_code_targets(const uint8_t *paged_rom, size_t banks,
                               const InlineSignatures *inline_sigs, LabelSet *bank_labels,
                               LabelSet *system_labels, const DataRanges *data_ranges,
                               ReferenceSet *refs, const ConfigEntries *ref_exclusions,
                               const ConfigEntries *literals, const ConfigEntries *far_imms);
void apply_config_bank_labels(const ConfigLabels *config_labels, const uint8_t *paged_rom,
                              size_t banks, LabelSet *bank_labels, const ConfigOptions *options);
void apply_config_bank_entries(const ConfigEntries *config_entries, const uint8_t *paged_rom,
                               size_t banks, LabelSet *bank_labels);
void apply_data_range_labels(const DataRanges *data_ranges, const uint8_t *paged_rom, size_t banks,
                             const uint8_t *system_rom, LabelSet *bank_labels,
                             LabelSet *system_labels, ReferenceSet *refs);
void apply_table_labels(const TableDefs *tables, const uint8_t *paged_rom, size_t banks,
                        LabelSet *bank_labels, LabelSet *system_labels, const uint8_t *system_rom,
                        ReferenceSet *refs);

#endif /* APEXDIS_LABELS_H */
