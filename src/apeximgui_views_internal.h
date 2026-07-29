#ifndef APEXIMGUI_VIEWS_INTERNAL_H
#define APEXIMGUI_VIEWS_INTERNAL_H

/* Small helpers shared between the apeximgui_views*.cpp translation units after
   the panel code was split out of the (formerly ~7.8K-line) apeximgui_views.cpp.
   Defined in apeximgui_views.cpp. */

#include "apeximgui_core.h"

/* Sortable-table flags shared by list panels. */
#define APEX_TABLE_SORT_FLAGS (ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate)

/* Case-insensitive substring test. */
bool str_icontains(const char *hay, const char *needle);

/* Reads the ImGui table sort spec for the current table; returns true and fills
   (*col, *asc) when a sortable column is active. */
bool ui_table_sort(int *col, bool *asc);

/* Three-way comparisons used by table sort comparators. */
int ui_cmp_int(long a, long b);
int ui_cmp_sz(size_t a, size_t b);
int ui_cmp_u32(uint32_t a, uint32_t b);

/* DMD / sprite image previews (shared by the disasm tooltip and the media
   list/gallery panels). Default scale lives here, on the declaration. */
void render_dmd_preview(const DmdPreviewInfo &preview, float max_scale = 6.0f);
void render_sprite_preview(const SpritePreviewInfo &pr, float max_scale = 6.0f);

#endif /* APEXIMGUI_VIEWS_INTERNAL_H */
