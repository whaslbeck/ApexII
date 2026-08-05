#ifndef APEX_IMGEXPORT_H
#define APEX_IMGEXPORT_H

/* Minimal, dependency-free image writers for exporting the live DMD:
   - a single-frame PNG (8-bit RGB, stored/uncompressed zlib);
   - a streaming animated GIF built from indexed frames + a 256-entry palette.
   Pure C++ (no ImGui), so it can be unit-tested headless. */

#include <stdint.h>
#include <stddef.h>

/* Write w*h*3 RGB bytes as a PNG.  Returns false on any I/O error. */
bool apex_png_write_rgb(const char *path, int w, int h, const uint8_t *rgb);

/* Animated GIF, one indexed byte per pixel into a shared 256-colour palette
   (palette256_rgb = 256*3 bytes).  Loops forever; per-frame delay in
   centiseconds (1/100 s).  Returns NULL on failure. */
struct ApexGif;
ApexGif *apex_gif_begin(const char *path, int w, int h, const uint8_t *palette256_rgb);
bool     apex_gif_add_frame(ApexGif *g, const uint8_t *indices, int delay_centiseconds);
bool     apex_gif_end(ApexGif *g); /* writes trailer, closes, frees g */

#endif /* APEX_IMGEXPORT_H */
