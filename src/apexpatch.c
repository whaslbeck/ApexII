/* apexpatch — build ROM patches described by a .patch.ini (see apex_patch.h).
 *
 *   apexpatch init  <rom> <analysis.ini> <out.patch.ini>
 *   apexpatch check <patch.ini>
 *   apexpatch build <patch.ini> [-o out.rom]
 *   apexpatch diff  <patch.ini>
 *   apexpatch pin   <patch.ini>
 *   apexpatch map   <patch.ini>
 *   apexpatch export <patch.ini> <out.ips|out.bps>
 *   apexpatch apply  <rom> <patch.ips|patch.bps> -o out.rom
 *   apexpatch report <patch.ini> [-o report.md]
 *   apexpatch rebase <patch.ini> <target.rom> [target.ini] -o <new.patch.ini>
 *   apexpatch ram <patch.ini> [min bytes]
 *   apexpatch snippets [patch.ini]
 *   apexpatch add <patch.ini> <snippet> <op name> [at=..] [alloc=..] [NAME=value ...]
 */
#include "apex.h"
#include "apex_ips.h"
#include "apex_patch.h"
#include "apex_project.h"
#include "apex_ramfind.h"
#include "apex_rominfo.h"
#include "apex_snippet.h"
#include "cpu6809.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f) {
    fputs("usage: apexpatch <command> ...\n"
          "  init  <rom> <analysis.ini|-> <out.patch.ini>  Start a patch file for a ROM\n"
          "  check <patch.ini>                 Build in memory and report every op\n"
          "  build <patch.ini> [-o out.rom]    Write the patched ROM (checksum per [patch])\n"
          "  diff  <patch.ini>                 Show each op's original and new bytes / code\n"
          "  pin   <patch.ini>                 Add missing expect guards and pin allocations\n"
          "                                    (placed = ...; rewrites the file)\n"
          "  map   <patch.ini>                 Free space per bank before / after the patch\n"
          "  export <patch.ini> <out.ips|.bps> Write the patch as IPS or BPS (for players)\n"
          "  apply <rom> <patch.ips|.bps> -o <out.rom>\n"
          "                                    Apply an IPS / BPS patch to a ROM\n"
          "  report <patch.ini> [-o report.md] Markdown report: ROMs, ops, changes, references,\n"
          "                                    free space\n"
          "  rebase <patch.ini> <target.rom> [target.ini] -o <new.patch.ini>\n"
          "                                    Carry the patch over to another ROM version\n"
          "                                    (without target.ini: labels via apexmatch)\n"
          "  ram <patch.ini> [min] [--sample ramdump.bin ...]\n"
          "                                    RAM the game does not seem to use: candidates\n"
          "                                    for a ram op (8 KB dumps: non-zero = used)\n"
          "  snippets [patch.ini]              List the snippet library (code templates)\n"
          "  add <patch.ini> <snippet|file.asm> <op name> [at=X] [alloc=Y] [NAME=value ...]\n"
          "                                    Append an op made from a snippet\n",
          f);
}

/* Patch + project + build, loaded together. */
typedef struct {
    ApexPatch patch;
    ApexProject *project;
    ApexPatchBuild build;
} Loaded;

static int load_and_build(const char *path, Loaded *l) {
    char err[512], rom[1024], ini[1024];
    memset(l, 0, sizeof(*l));
    if (apex_patch_load(path, &l->patch, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return -1;
    }
    if (!l->patch.base_rom) {
        fprintf(stderr, "%s: [patch] has no base_rom\n", path);
        return -1;
    }
    apex_patch_path(&l->patch, l->patch.base_rom, rom, sizeof(rom));
    apex_patch_path(&l->patch, l->patch.analysis, ini, sizeof(ini));
    l->project = apex_project_open(rom, l->patch.analysis ? ini : NULL);
    if (!l->project || apex_project_analyze(l->project) != 0) {
        fprintf(stderr, "%s: cannot analyse %s\n", path, rom);
        return -1;
    }
    return apex_patch_build(&l->patch, l->project, &l->build) < 0 ? -1 : 0;
}

static void unload(Loaded *l) {
    apex_patch_build_free(&l->build);
    if (l->project)
        apex_project_free(l->project);
    apex_patch_free(&l->patch);
}

static void report(const Loaded *l) {
    size_t i;
    for (i = 0; i < l->patch.count; i++) {
        const ApexPatchOp *o = &l->patch.ops[i];
        const ApexPatchOpResult *r = &l->build.ops[i];
        printf("%-5s %-24s %-5s", apex_patch_status_word(r->status), o->name,
               apex_patch_type_name(o->type));
        if (r->length)
            printf(" B%02x_A%04x +%lu", r->bank, (unsigned)r->addr, (unsigned long)r->length);
        if (r->has_alloc && o->type == APEX_PATCH_HOOK)
            printf(" -> B%02x_A%04x +%lu", r->alloc_bank, (unsigned)r->alloc_addr,
                   (unsigned long)r->alloc_length);
        if (r->msg[0])
            printf("  %s", r->msg);
        putchar('\n');
    }
    if (l->build.msg[0])
        printf("ERROR %s\n", l->build.msg);
    printf("%lu op(s), %d error(s), %d warning(s)\n", (unsigned long)l->patch.count,
           l->build.errors, l->build.warnings);
}

static int cmd_check(int argc, char **argv, int write) {
    const char *path = NULL, *out = NULL;
    Loaded l;
    for (int i = 0; i < argc; i++) {
        if (write && !strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (argv[i][0] == '-' || path) {
            usage(stderr);
            return 2;
        } else
            path = argv[i];
    }
    if (!path) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(path, &l) < 0) {
        unload(&l);
        return 1;
    }
    report(&l);
    int rc = l.build.errors ? 1 : 0;
    if (write && !rc) {
        char dst[1024];
        if (out)
            snprintf(dst, sizeof(dst), "%s", out);
        else if (l.patch.output)
            apex_patch_path(&l.patch, l.patch.output, dst, sizeof(dst));
        else
            snprintf(dst, sizeof(dst), "%s.rom", path);
        write_file(dst, l.build.rom, l.build.rom_size);
        printf("wrote %s (CRC-32 %08X)\n", dst, (unsigned)ari_crc32(l.build.rom, l.build.rom_size));
    } else if (write) {
        puts("not written: the patch has errors");
    }
    unload(&l);
    return rc;
}

static int cmd_diff(int argc, char **argv) {
    Loaded l;
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(argv[0], &l) < 0) {
        unload(&l);
        return 1;
    }
    for (size_t i = 0; i < l.patch.count; i++)
        apex_patch_write_diff_op(stdout, &l.patch, &l.build, i);
    unload(&l);
    return 0;
}

/* Free space per bank: before the patch (detected + declared - reserved) and
   what is left after it, plus the blocks still free. */
static int cmd_map(int argc, char **argv) {
    Loaded l;
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(argv[0], &l) < 0) {
        unload(&l);
        return 1;
    }
    unsigned long tot[256] = {0}, left[256] = {0};
    size_t i;
    for (i = 0; i < l.build.free_total.count; i++)
        tot[l.build.free_total.items[i].bank] += l.build.free_total.items[i].len;
    for (i = 0; i < l.build.free_left.count; i++)
        left[l.build.free_left.items[i].bank] += l.build.free_left.items[i].len;
    puts("bank  free     used by patch  left");
    for (int b = 0; b < 256; b++) {
        if (tot[b])
            printf("%02x    %6lu   %6lu         %6lu\n", b, tot[b], tot[b] - left[b], left[b]);
    }
    puts("free blocks left:");
    for (i = 0; i < l.build.free_left.count; i++) {
        const ApexFreeBlock *fb = &l.build.free_left.items[i];
        static const char *kinds[] = {"trailing", "interior", "declared"};
        if (fb->len)
            printf("  B%02x_A%04x  %5u byte(s)  %s\n", fb->bank, (unsigned)fb->addr,
                   (unsigned)fb->len, kinds[fb->kind]);
    }
    unload(&l);
    return 0;
}

static int cmd_pin(int argc, char **argv) {
    Loaded l;
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(argv[0], &l) < 0) {
        unload(&l);
        return 1;
    }
    int n = apex_patch_pin(&l.patch, &l.build);
    if (!l.patch.has_crc) {
        l.patch.base_crc32 = ari_crc32(l.project->rom.data, l.project->rom.size);
        l.patch.has_crc = 1;
        n++;
    }
    int rc = 0;
    if (n > 0 && apex_patch_save(&l.patch, argv[0]) != 0) {
        fprintf(stderr, "cannot write %s\n", argv[0]);
        rc = 1;
    }
    printf("%d guard(s) added\n", n);
    unload(&l);
    return rc;
}

static int cmd_init(int argc, char **argv) {
    if (argc != 3) {
        usage(stderr);
        return 2;
    }
    FILE *f = fopen(argv[2], "r");
    if (f) {
        fclose(f);
        fprintf(stderr, "%s exists; not overwritten\n", argv[2]);
        return 1;
    }
    int no_ini = !strcmp(argv[1], "-");
    ApexProject *p = apex_project_open(argv[0], no_ini ? NULL : argv[1]);
    if (!p) {
        fprintf(stderr, "cannot open %s\n", argv[0]);
        return 1;
    }
    ApexPatch patch;
    memset(&patch, 0, sizeof(patch));
    /* Paths in the file are relative to it, or absolute. */
    char rom[4096], ini[4096];
    apex_patch_relpath(argv[2], argv[0], rom, sizeof(rom));
    apex_patch_relpath(argv[2], argv[1], ini, sizeof(ini));
    patch.base_rom = rom;
    patch.analysis = no_ini ? NULL : ini;
    patch.base_crc32 = ari_crc32(p->rom.data, p->rom.size);
    patch.has_crc = 1;
    patch.checksum = APEX_PATCH_CHECKSUM_FIX;
    patch.comment = (char *)"; ApexII patch file -- see apexpatch / apex_patch.h";
    int rc = apex_patch_save(&patch, argv[2]);
    apex_project_free(p);
    if (rc) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    printf("wrote %s\n", argv[2]);
    return 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), k = strlen(suffix);
    if (n < k)
        return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return 0;
    }
    return 1;
}

/* The patch as IPS / BPS against its base ROM (checksum fix included). */
static int cmd_export(int argc, char **argv) {
    Loaded l;
    if (argc != 2 || (!ends_with(argv[1], ".ips") && !ends_with(argv[1], ".bps"))) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(argv[0], &l) < 0 || l.build.errors || !l.build.rom) {
        if (l.build.rom)
            report(&l);
        fprintf(stderr, "%s: not exported, the patch has errors\n", argv[0]);
        unload(&l);
        return 1;
    }
    size_t n = 0;
    uint8_t *pf;
    if (ends_with(argv[1], ".ips")) {
        pf = apex_ips_make(l.project->rom.data, l.build.rom, l.build.rom_size, &n);
    } else {
        char meta[512];
        const char *base = strrchr(argv[0], '/');
        snprintf(meta, sizeof(meta), "apexpatch %s", base ? base + 1 : argv[0]);
        pf = apex_bps_make(l.project->rom.data, l.build.rom, l.build.rom_size, meta, &n);
    }
    if (!pf) {
        fprintf(stderr, "%s: cannot make the patch\n", argv[1]);
        unload(&l);
        return 1;
    }
    write_file(argv[1], pf, n);
    printf("wrote %s (%lu bytes; ROM CRC-32 %08X -> %08X)\n", argv[1], (unsigned long)n,
           (unsigned)ari_crc32(l.project->rom.data, l.build.rom_size),
           (unsigned)ari_crc32(l.build.rom, l.build.rom_size));
    free(pf);
    unload(&l);
    return 0;
}

static int cmd_apply(int argc, char **argv) {
    const char *out = NULL;
    if (argc == 4 && !strcmp(argv[2], "-o"))
        out = argv[3];
    if (!out) {
        usage(stderr);
        return 2;
    }
    FILE *f = fopen(argv[0], "rb");
    FILE *g = fopen(argv[1], "rb");
    if (!f || !g) {
        fprintf(stderr, "cannot open %s\n", !f ? argv[0] : argv[1]);
        if (f)
            fclose(f);
        if (g)
            fclose(g);
        return 1;
    }
    fclose(f);
    fclose(g);
    Buffer rom = read_file(argv[0]), pb = read_file(argv[1]);
    char err[256];
    size_t n = 0;
    uint8_t *img = apex_patchfile_apply(rom.data, rom.size, pb.data, pb.size, &n, err, sizeof(err));
    if (!img) {
        fprintf(stderr, "%s: %s\n", argv[1], err);
        free(rom.data);
        free(pb.data);
        return 1;
    }
    write_file(out, img, n);
    printf("wrote %s (CRC-32 %08X)\n", out, (unsigned)ari_crc32(img, n));
    free(img);
    free(rom.data);
    free(pb.data);
    return 0;
}

/* Markdown report of a patch: for the record, a review or a release note. */
static int cmd_report(int argc, char **argv) {
    const char *path = NULL, *out = NULL;
    Loaded l;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (argv[i][0] == '-' || path) {
            usage(stderr);
            return 2;
        } else
            path = argv[i];
    }
    if (!path) {
        usage(stderr);
        return 2;
    }
    if (load_and_build(path, &l) < 0) {
        unload(&l);
        return 1;
    }
    FILE *f = out ? fopen(out, "w") : stdout;
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out);
        unload(&l);
        return 1;
    }
    apex_patch_write_report(f, path, &l.patch, l.project, l.project->rom.data, &l.build);
    if (out) {
        fclose(f);
        printf("wrote %s\n", out);
    }
    int rc = l.build.errors ? 1 : 0;
    unload(&l);
    return rc;
}

static int cmd_rebase(int argc, char **argv) {
    const char *path = NULL, *trom = NULL, *tini = NULL, *out = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!path)
            path = argv[i];
        else if (!trom)
            trom = argv[i];
        else if (!tini)
            tini = argv[i];
        else {
            usage(stderr);
            return 2;
        }
    }
    if (!path || !trom || !out) {
        usage(stderr);
        return 2;
    }
    Loaded l;
    if (load_and_build(path, &l) < 0) {
        unload(&l);
        return 1;
    }
    ApexPatch np;
    ApexRebaseOp *ro = NULL;
    char err[512], labels[1200];
    int failed = apex_patch_rebase_files(&l.patch, l.project, &l.build, trom, tini, out, &np, &ro,
                                         labels, sizeof(labels), err, sizeof(err));
    if (failed < 0) {
        fprintf(stderr, "%s\n", err);
        unload(&l);
        return 1;
    }
    if (labels[0])
        printf("target analysis: %s\n", labels);
    for (size_t i = 0; i < np.count; i++) {
        const ApexPatchOp *o = &np.ops[i];
        printf("%-5s %-24s %-6s", apex_patch_status_word(ro[i].status), o->name,
               apex_patch_type_name(o->type));
        if (ro[i].old_bank || ro[i].old_addr)
            printf(" B%02x_A%04x", ro[i].old_bank, (unsigned)ro[i].old_addr);
        if (ro[i].method[0] && (ro[i].old_bank || ro[i].old_addr))
            printf(" -> B%02x_A%04x", ro[i].new_bank, (unsigned)ro[i].new_addr);
        if (ro[i].method[0])
            printf("  (%s)", ro[i].method);
        if (ro[i].msg[0])
            printf("  %s", ro[i].msg);
        putchar('\n');
    }
    printf("wrote %s: %lu op(s), %d not found\n", out, (unsigned long)np.count, failed);
    apex_patch_free(&np);
    free(ro);
    unload(&l);
    return failed ? 1 : 0;
}

/* RAM the analysed code does not use (static evidence only; see
   apex_ramfind.h), minus what the patch's ram ops already claim. */
static int cmd_ram(int argc, char **argv) {
    const char *path = NULL;
    unsigned min = 4u;
    const char *samples[16];
    int ns = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--sample") && i + 1 < argc && ns < 16)
            samples[ns++] = argv[++i];
        else if (!path)
            path = argv[i];
        else
            min = (unsigned)strtoul(argv[i], NULL, 0);
    }
    if (!path) {
        usage(stderr);
        return 2;
    }
    Loaded l;
    if (load_and_build(path, &l) < 0) {
        unload(&l);
        return 1;
    }
    static ApexRamMap map;
    memset(&map, 0, sizeof(map));
    apex_ram_map_static(l.project, apex_project_render(l.project, 0, 0), &map, 0, 0);
    for (size_t i = 0; i < l.patch.count; i++) {
        const ApexPatchOp *o = &l.patch.ops[i];
        if (o->type == APEX_PATCH_RAM && o->enabled && o->at)
            apex_ram_map_mark(&map, (uint32_t)strtoul(o->at, NULL, 0), o->length ? o->length : 1u,
                              APEX_RAM_PATCH);
    }
    for (int k = 0; k < ns; k++) {
        FILE *f = fopen(samples[k], "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", samples[k]);
            continue;
        }
        fclose(f);
        Buffer b = read_file(samples[k]);
        if (b.size >= APEX_RAM_SIZE)
            apex_ram_map_sample(&map, b.data);
        else
            fprintf(stderr, "%s: a RAM dump needs %u bytes\n", samples[k], APEX_RAM_SIZE);
        free(b.data);
    }
    apex_ram_map_runs(&map, min);
    unsigned used = 0;
    for (uint32_t a = 0; a < APEX_RAM_SIZE; a++)
        used += map.use[a] != 0;
    printf("RAM 0x0000-0x%04x: %u byte(s) used (code operands, symbols, pointer bases, "
           "stacks%s)\n",
           APEX_RAM_SIZE - 1u, used, map.samples ? ", non-zero in a snapshot" : "");
    if (map.samples)
        printf("unused runs of %u+ bytes (no static use, zero in %u snapshot(s)):\n", min,
               map.samples);
    else
        printf("unused runs of %u+ bytes (static evidence only -- confirm with RAM snapshots, "
               "--sample):\n",
               min);
    for (size_t i = 0; i < map.count && i < 40; i++)
        printf("  0x%04x  %5u byte(s)%s\n", (unsigned)map.runs[i].addr, (unsigned)map.runs[i].len,
               map.runs[i].nvram ? "  (NVRAM area: audits / settings? risky)" : "");
    if (map.count > 40)
        printf("  ... %lu more\n", (unsigned long)(map.count - 40));
    apex_ram_map_free(&map);
    unload(&l);
    return 0;
}

static const char *g_exe_dir;

static void load_snippets(ApexSnippetList *list, const char *patch_path) {
    memset(list, 0, sizeof(*list));
    apex_snippet_load_default(list, patch_path, g_exe_dir);
}

static int cmd_snippets(int argc, char **argv) {
    ApexSnippetList list;
    if (argc > 1) {
        usage(stderr);
        return 2;
    }
    load_snippets(&list, argc ? argv[0] : NULL);
    for (size_t i = 0; i < list.count; i++) {
        const ApexSnippet *sn = &list.items[i];
        printf("%-20s %-4s  %s\n", sn->id, sn->type, sn->title);
        if (sn->about)
            printf("    %s\n", sn->about);
        for (size_t k = 0; k < sn->param_count; k++)
            printf("    {%s}  %s%s%s\n", sn->params[k].name, sn->params[k].desc,
                   sn->params[k].def ? "  default " : "",
                   sn->params[k].def ? sn->params[k].def : "");
    }
    if (!list.count)
        puts("no snippets found (snippets/ next to the patch, $APEX_SNIPPETS, ./snippets)");
    apex_snippet_list_free(&list);
    return 0;
}

static int cmd_add(int argc, char **argv) {
    if (argc < 3) {
        usage(stderr);
        return 2;
    }
    const char *path = argv[0], *which = argv[1], *name = argv[2];
    char err[512];
    ApexSnippetList list;
    ApexSnippet own;
    const ApexSnippet *sn = NULL;
    int rc = 1;
    memset(&own, 0, sizeof(own));
    load_snippets(&list, path);
    if (strchr(which, '/') || ends_with(which, ".asm")) {
        if (apex_snippet_load(which, &own, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s\n", err);
            apex_snippet_list_free(&list);
            return 1;
        }
        sn = &own;
    } else if (!(sn = apex_snippet_find(&list, which))) {
        fprintf(stderr, "no snippet '%s' (apexpatch snippets lists them)\n", which);
        apex_snippet_list_free(&list);
        return 1;
    }
    const char *at = NULL, *alloc = NULL;
    const char **vals = calloc(sn->param_count + 1u, sizeof(*vals));
    for (int i = 3; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        size_t k, n = eq ? (size_t)(eq - argv[i]) : 0;
        if (!eq) {
            fprintf(stderr, "expected NAME=value: %s\n", argv[i]);
            goto done;
        }
        if (n == 2 && !strncmp(argv[i], "at", 2)) {
            at = eq + 1;
            continue;
        }
        if (n == 5 && !strncmp(argv[i], "alloc", 5)) {
            alloc = eq + 1;
            continue;
        }
        for (k = 0; k < sn->param_count; k++) {
            if (strlen(sn->params[k].name) == n && !strncmp(argv[i], sn->params[k].name, n))
                break;
        }
        if (k == sn->param_count) {
            fprintf(stderr, "snippet %s has no parameter %.*s\n", sn->id, (int)n, argv[i]);
            goto done;
        }
        vals[k] = eq + 1;
    }
    char *code = apex_snippet_expand(sn, vals, err, sizeof(err));
    if (!code) {
        fprintf(stderr, "%s\n", err);
        goto done;
    }
    ApexPatch patch;
    if (apex_patch_load(path, &patch, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        apex_patch_free(&patch);
        free(code);
        goto done;
    }
    int type = !strcmp(sn->type, "hook") ? APEX_PATCH_HOOK : APEX_PATCH_ASM;
    long idx = apex_patch_add_op(&patch, type, name, at);
    if (idx >= 0) {
        ApexPatchOp *o = &patch.ops[idx];
        char note[256];
        snprintf(note, sizeof(note), "snippet %s: %s", sn->id, sn->title);
        apex_patch_set_field(&o->code, code);
        apex_patch_set_field(&o->note, note);
        if (alloc)
            apex_patch_set_field(&o->alloc, alloc);
        if (apex_patch_save(&patch, path) == 0) {
            printf("added [op:%s] (%s from snippet %s) to %s\n", o->name,
                   apex_patch_type_name(type), sn->id, path);
            rc = 0;
        } else {
            fprintf(stderr, "cannot write %s\n", path);
        }
    }
    apex_patch_free(&patch);
    free(code);
done:
    free(vals);
    apex_snippet_free(&own);
    apex_snippet_list_free(&list);
    return rc;
}

int main(int argc, char **argv) {
    static char exe_dir[1024];
    const char *sl = strrchr(argv[0], '/');
    if (sl) {
        snprintf(exe_dir, sizeof(exe_dir), "%.*s", (int)(sl - argv[0]), argv[0]);
        g_exe_dir = exe_dir;
    }
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "init"))
        return cmd_init(argc - 2, argv + 2);
    if (!strcmp(argv[1], "check"))
        return cmd_check(argc - 2, argv + 2, 0);
    if (!strcmp(argv[1], "build"))
        return cmd_check(argc - 2, argv + 2, 1);
    if (!strcmp(argv[1], "diff"))
        return cmd_diff(argc - 2, argv + 2);
    if (!strcmp(argv[1], "pin"))
        return cmd_pin(argc - 2, argv + 2);
    if (!strcmp(argv[1], "map"))
        return cmd_map(argc - 2, argv + 2);
    if (!strcmp(argv[1], "export"))
        return cmd_export(argc - 2, argv + 2);
    if (!strcmp(argv[1], "apply"))
        return cmd_apply(argc - 2, argv + 2);
    if (!strcmp(argv[1], "report"))
        return cmd_report(argc - 2, argv + 2);
    if (!strcmp(argv[1], "rebase"))
        return cmd_rebase(argc - 2, argv + 2);
    if (!strcmp(argv[1], "ram"))
        return cmd_ram(argc - 2, argv + 2);
    if (!strcmp(argv[1], "snippets"))
        return cmd_snippets(argc - 2, argv + 2);
    if (!strcmp(argv[1], "add"))
        return cmd_add(argc - 2, argv + 2);
    fprintf(stderr, "apexpatch: unknown command '%s'\n\n", argv[1]);
    usage(stderr);
    return 2;
}
