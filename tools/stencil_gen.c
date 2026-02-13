#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include "stencil_data.h"

typedef struct source_file {
    char *path;
    char *stem;
} source_file_t;

typedef struct reloc_entry {
    uint16_t offset;
    uint8_t size;
    lr_stencil_hole_t hole;
} reloc_entry_t;

typedef struct stencil_entry {
    char *stem;
    uint8_t *text;
    size_t text_size;
    reloc_entry_t *relocs;
    size_t reloc_count;
    size_t reloc_cap;
} stencil_entry_t;

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s --input-dir <dir> --output <header> [--compiler <cc>]\n",
            prog);
}

static int has_suffix(const char *s, const char *suffix) {
    size_t s_len = strlen(s);
    size_t suff_len = strlen(suffix);
    if (s_len < suff_len) {
        return 0;
    }
    return strcmp(s + s_len - suff_len, suffix) == 0;
}

static char *dup_cstr(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

static char *path_join(const char *a, const char *b) {
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    bool need_sep = (a_len > 0 && a[a_len - 1] != '/');
    size_t total = a_len + (need_sep ? 1 : 0) + b_len + 1;
    char *out = (char *)malloc(total);
    if (!out) {
        return NULL;
    }
    memcpy(out, a, a_len);
    if (need_sep) {
        out[a_len] = '/';
        memcpy(out + a_len + 1, b, b_len);
        out[a_len + 1 + b_len] = '\0';
    } else {
        memcpy(out + a_len, b, b_len);
        out[a_len + b_len] = '\0';
    }
    return out;
}

static char *stem_from_filename(const char *name) {
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, name, n);
    out[n] = '\0';
    return out;
}

static int compare_source_files(const void *a, const void *b) {
    const source_file_t *sa = (const source_file_t *)a;
    const source_file_t *sb = (const source_file_t *)b;
    return strcmp(sa->stem, sb->stem);
}

static int compare_reloc_entries(const void *a, const void *b) {
    const reloc_entry_t *ra = (const reloc_entry_t *)a;
    const reloc_entry_t *rb = (const reloc_entry_t *)b;
    if (ra->offset < rb->offset) return -1;
    if (ra->offset > rb->offset) return 1;
    if (ra->hole < rb->hole) return -1;
    if (ra->hole > rb->hole) return 1;
    return 0;
}

static int list_sources(const char *input_dir, source_file_t **out_files, size_t *out_count) {
    DIR *dir = opendir(input_dir);
    struct dirent *de;
    source_file_t *files = NULL;
    size_t count = 0;
    size_t cap = 0;
    if (!dir) {
        fprintf(stderr, "stencil_gen: failed to open input dir '%s': %s\n",
                input_dir, strerror(errno));
        return -1;
    }
    while ((de = readdir(dir)) != NULL) {
        char *full_path;
        char *stem;
        source_file_t *next_files;
        if (de->d_name[0] == '.') {
            continue;
        }
        if (!has_suffix(de->d_name, ".c")) {
            continue;
        }
        full_path = path_join(input_dir, de->d_name);
        if (!full_path) {
            closedir(dir);
            return -1;
        }
        stem = stem_from_filename(de->d_name);
        if (!stem) {
            free(full_path);
            closedir(dir);
            return -1;
        }
        if (count == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            next_files = (source_file_t *)realloc(files, new_cap * sizeof(*files));
            if (!next_files) {
                free(full_path);
                free(stem);
                closedir(dir);
                return -1;
            }
            files = next_files;
            cap = new_cap;
        }
        files[count].path = full_path;
        files[count].stem = stem;
        count++;
    }
    closedir(dir);
    if (count == 0) {
        free(files);
        fprintf(stderr, "stencil_gen: no .c files found in '%s'\n", input_dir);
        return -1;
    }
    qsort(files, count, sizeof(*files), compare_source_files);
    *out_files = files;
    *out_count = count;
    return 0;
}

static void free_sources(source_file_t *files, size_t count) {
    size_t i;
    if (!files) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].stem);
    }
    free(files);
}

static int run_child(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "stencil_gen: exec failed for '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int compile_stencil_source(const char *compiler, const char *src, const char *obj) {
    char *const argv[] = {
        (char *)compiler,
        (char *)"-O3",
        (char *)"-fno-pic",
        (char *)"-fno-pie",
        (char *)"-fno-stack-protector",
        (char *)"-fno-asynchronous-unwind-tables",
        (char *)"-fno-unwind-tables",
        (char *)"-c",
        (char *)src,
        (char *)"-o",
        (char *)obj,
        NULL
    };
    return run_child(argv);
}

static int read_file(const char *path, uint8_t **out_data, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    n = ftell(fp);
    if (n < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_data = buf;
    *out_size = (size_t)n;
    return 0;
}

static int map_hole_symbol(const char *name, lr_stencil_hole_t *out_hole) {
    if (strcmp(name, "__hole_src0_off") == 0) {
        *out_hole = LR_STENCIL_HOLE_SRC0_OFF;
        return 0;
    }
    if (strcmp(name, "__hole_src1_off") == 0) {
        *out_hole = LR_STENCIL_HOLE_SRC1_OFF;
        return 0;
    }
    if (strcmp(name, "__hole_dst_off") == 0) {
        *out_hole = LR_STENCIL_HOLE_DST_OFF;
        return 0;
    }
    if (strcmp(name, "__hole_imm64") == 0) {
        *out_hole = LR_STENCIL_HOLE_IMM64;
        return 0;
    }
    if (strcmp(name, "__hole_branch_rel") == 0) {
        *out_hole = LR_STENCIL_HOLE_BRANCH_REL;
        return 0;
    }
    if (strcmp(name, "__hole_func_addr") == 0) {
        *out_hole = LR_STENCIL_HOLE_FUNC_ADDR;
        return 0;
    }
    if (strcmp(name, "__hole_global_addr") == 0) {
        *out_hole = LR_STENCIL_HOLE_GLOBAL_ADDR;
        return 0;
    }
    return -1;
}

static int reloc_size_for_type(uint32_t type, uint8_t *out_size) {
    switch (type) {
    case R_X86_64_64:
        *out_size = 8;
        return 0;
    case R_X86_64_32:
    case R_X86_64_32S:
    case R_X86_64_PC32:
        *out_size = 4;
        return 0;
    case R_X86_64_16:
        *out_size = 2;
        return 0;
    case R_X86_64_8:
        *out_size = 1;
        return 0;
    default:
        return -1;
    }
}

static int add_reloc(stencil_entry_t *entry, uint16_t offset, uint8_t size, lr_stencil_hole_t hole) {
    reloc_entry_t *next;
    if (entry->reloc_count == entry->reloc_cap) {
        size_t new_cap = entry->reloc_cap ? entry->reloc_cap * 2 : 8;
        next = (reloc_entry_t *)realloc(entry->relocs, new_cap * sizeof(*entry->relocs));
        if (!next) {
            return -1;
        }
        entry->relocs = next;
        entry->reloc_cap = new_cap;
    }
    entry->relocs[entry->reloc_count].offset = offset;
    entry->relocs[entry->reloc_count].size = size;
    entry->relocs[entry->reloc_count].hole = hole;
    entry->reloc_count++;
    return 0;
}

static int parse_elf_object(const char *obj_path, stencil_entry_t *entry) {
    uint8_t *data = NULL;
    size_t size = 0;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *shdrs;
    const char *shstrtab;
    uint16_t text_index = 0;
    uint16_t symtab_index = 0;
    uint16_t i;
    if (read_file(obj_path, &data, &size) != 0) {
        fprintf(stderr, "stencil_gen: failed reading object '%s'\n", obj_path);
        return -1;
    }
    if (size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "stencil_gen: malformed object '%s' (too small)\n", obj_path);
        free(data);
        return -1;
    }
    eh = (const Elf64_Ehdr *)data;
    if (!(eh->e_ident[EI_MAG0] == ELFMAG0 &&
          eh->e_ident[EI_MAG1] == ELFMAG1 &&
          eh->e_ident[EI_MAG2] == ELFMAG2 &&
          eh->e_ident[EI_MAG3] == ELFMAG3)) {
        fprintf(stderr, "stencil_gen: '%s' is not an ELF object\n", obj_path);
        free(data);
        return -1;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "stencil_gen: unsupported ELF class/data in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) {
        fprintf(stderr, "stencil_gen: missing section headers in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    if (eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > size) {
        fprintf(stderr, "stencil_gen: section headers out of bounds in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    shdrs = (const Elf64_Shdr *)(data + eh->e_shoff);
    if (eh->e_shstrndx >= eh->e_shnum) {
        fprintf(stderr, "stencil_gen: invalid shstr index in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    if (shdrs[eh->e_shstrndx].sh_offset + shdrs[eh->e_shstrndx].sh_size > size) {
        fprintf(stderr, "stencil_gen: bad shstr bounds in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    shstrtab = (const char *)(data + shdrs[eh->e_shstrndx].sh_offset);
    for (i = 0; i < eh->e_shnum; i++) {
        const char *name = shstrtab + shdrs[i].sh_name;
        if (strcmp(name, ".text") == 0) {
            text_index = i;
        } else if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_index = i;
        }
    }
    if (text_index == 0) {
        fprintf(stderr, "stencil_gen: no .text section in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    if (symtab_index == 0) {
        fprintf(stderr, "stencil_gen: no symbol table in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    if (shdrs[text_index].sh_offset + shdrs[text_index].sh_size > size ||
        shdrs[text_index].sh_size == 0) {
        fprintf(stderr, "stencil_gen: invalid .text bounds in '%s'\n", obj_path);
        free(data);
        return -1;
    }
    entry->text = (uint8_t *)malloc(shdrs[text_index].sh_size);
    if (!entry->text) {
        free(data);
        return -1;
    }
    memcpy(entry->text, data + shdrs[text_index].sh_offset, shdrs[text_index].sh_size);
    entry->text_size = shdrs[text_index].sh_size;
    {
        const Elf64_Shdr *symtab = &shdrs[symtab_index];
        const Elf64_Shdr *strtab;
        const Elf64_Sym *syms;
        const char *sym_names;
        size_t sym_count;
        if (symtab->sh_link >= eh->e_shnum || symtab->sh_entsize != sizeof(Elf64_Sym)) {
            fprintf(stderr, "stencil_gen: invalid symbol table metadata in '%s'\n", obj_path);
            free(data);
            return -1;
        }
        strtab = &shdrs[symtab->sh_link];
        if (symtab->sh_offset + symtab->sh_size > size ||
            strtab->sh_offset + strtab->sh_size > size) {
            fprintf(stderr, "stencil_gen: bad symbol/string table bounds in '%s'\n", obj_path);
            free(data);
            return -1;
        }
        syms = (const Elf64_Sym *)(data + symtab->sh_offset);
        sym_names = (const char *)(data + strtab->sh_offset);
        sym_count = symtab->sh_size / sizeof(Elf64_Sym);
        for (i = 0; i < eh->e_shnum; i++) {
            const Elf64_Shdr *sh = &shdrs[i];
            if ((sh->sh_type != SHT_RELA && sh->sh_type != SHT_REL) || sh->sh_info != text_index) {
                continue;
            }
            if (sh->sh_offset + sh->sh_size > size) {
                fprintf(stderr, "stencil_gen: relocation section out of bounds in '%s'\n", obj_path);
                free(data);
                return -1;
            }
            if (sh->sh_type == SHT_RELA) {
                size_t count = sh->sh_size / sizeof(Elf64_Rela);
                size_t j;
                const Elf64_Rela *rels = (const Elf64_Rela *)(data + sh->sh_offset);
                for (j = 0; j < count; j++) {
                    uint32_t sym_i = ELF64_R_SYM(rels[j].r_info);
                    uint32_t type = ELF64_R_TYPE(rels[j].r_info);
                    lr_stencil_hole_t hole;
                    uint8_t patch_size = 0;
                    uint64_t off = rels[j].r_offset;
                    const char *sym_name;
                    if (sym_i >= sym_count) {
                        continue;
                    }
                    sym_name = sym_names + syms[sym_i].st_name;
                    if (map_hole_symbol(sym_name, &hole) != 0) {
                        continue;
                    }
                    if (reloc_size_for_type(type, &patch_size) != 0) {
                        fprintf(stderr,
                                "stencil_gen: unsupported relocation type %u for '%s'\n",
                                type, sym_name);
                        free(data);
                        return -1;
                    }
                    if (off > UINT16_MAX || off + patch_size > entry->text_size) {
                        fprintf(stderr, "stencil_gen: relocation offset out of range in '%s'\n", obj_path);
                        free(data);
                        return -1;
                    }
                    if (add_reloc(entry, (uint16_t)off, patch_size, hole) != 0) {
                        free(data);
                        return -1;
                    }
                }
            } else {
                size_t count = sh->sh_size / sizeof(Elf64_Rel);
                size_t j;
                const Elf64_Rel *rels = (const Elf64_Rel *)(data + sh->sh_offset);
                for (j = 0; j < count; j++) {
                    uint32_t sym_i = ELF64_R_SYM(rels[j].r_info);
                    uint32_t type = ELF64_R_TYPE(rels[j].r_info);
                    lr_stencil_hole_t hole;
                    uint8_t patch_size = 0;
                    uint64_t off = rels[j].r_offset;
                    const char *sym_name;
                    if (sym_i >= sym_count) {
                        continue;
                    }
                    sym_name = sym_names + syms[sym_i].st_name;
                    if (map_hole_symbol(sym_name, &hole) != 0) {
                        continue;
                    }
                    if (reloc_size_for_type(type, &patch_size) != 0) {
                        fprintf(stderr,
                                "stencil_gen: unsupported relocation type %u for '%s'\n",
                                type, sym_name);
                        free(data);
                        return -1;
                    }
                    if (off > UINT16_MAX || off + patch_size > entry->text_size) {
                        fprintf(stderr, "stencil_gen: relocation offset out of range in '%s'\n", obj_path);
                        free(data);
                        return -1;
                    }
                    if (add_reloc(entry, (uint16_t)off, patch_size, hole) != 0) {
                        free(data);
                        return -1;
                    }
                }
            }
        }
    }
    qsort(entry->relocs, entry->reloc_count, sizeof(*entry->relocs), compare_reloc_entries);
    free(data);
    if (entry->reloc_count == 0) {
        fprintf(stderr, "stencil_gen: no hole relocations found in '%s'\n", obj_path);
        return -1;
    }
    return 0;
}

static const char *hole_name(lr_stencil_hole_t hole) {
    switch (hole) {
    case LR_STENCIL_HOLE_SRC0_OFF: return "LR_STENCIL_HOLE_SRC0_OFF";
    case LR_STENCIL_HOLE_SRC1_OFF: return "LR_STENCIL_HOLE_SRC1_OFF";
    case LR_STENCIL_HOLE_DST_OFF: return "LR_STENCIL_HOLE_DST_OFF";
    case LR_STENCIL_HOLE_IMM64: return "LR_STENCIL_HOLE_IMM64";
    case LR_STENCIL_HOLE_BRANCH_REL: return "LR_STENCIL_HOLE_BRANCH_REL";
    case LR_STENCIL_HOLE_FUNC_ADDR: return "LR_STENCIL_HOLE_FUNC_ADDR";
    case LR_STENCIL_HOLE_GLOBAL_ADDR: return "LR_STENCIL_HOLE_GLOBAL_ADDR";
    default: return "LR_STENCIL_HOLE_IMM64";
    }
}

static char *sanitize_identifier(const char *stem) {
    size_t n = strlen(stem);
    char *out = (char *)malloc(n + 2);
    size_t i;
    size_t pos = 0;
    if (!out) {
        return NULL;
    }
    if (n == 0 || (stem[0] >= '0' && stem[0] <= '9')) {
        out[pos++] = '_';
    }
    for (i = 0; i < n; i++) {
        char c = stem[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            out[pos++] = c;
        } else {
            out[pos++] = '_';
        }
    }
    out[pos] = '\0';
    return out;
}

static int write_header(const char *output, const stencil_entry_t *entries, size_t count) {
    FILE *fp = fopen(output, "wb");
    size_t i;
    if (!fp) {
        fprintf(stderr, "stencil_gen: failed to open output '%s': %s\n",
                output, strerror(errno));
        return -1;
    }
    fprintf(fp, "/* Generated by stencil_gen. Do not edit. */\n");
    fprintf(fp, "#ifndef LIRIC_STENCIL_DATA_X86_64_H\n");
    fprintf(fp, "#define LIRIC_STENCIL_DATA_X86_64_H\n\n");
    fprintf(fp, "#include <stddef.h>\n");
    fprintf(fp, "#include <stdint.h>\n");
    fprintf(fp, "#include \"stencil_data.h\"\n\n");

    for (i = 0; i < count; i++) {
        char *id = sanitize_identifier(entries[i].stem);
        size_t j;
        if (!id) {
            fclose(fp);
            return -1;
        }
        fprintf(fp, "static const uint8_t lr_stencil_%s_bytes[] = {\n", id);
        for (j = 0; j < entries[i].text_size; j++) {
            if (j % 12 == 0) {
                fprintf(fp, "    ");
            }
            fprintf(fp, "0x%02x", entries[i].text[j]);
            if (j + 1 < entries[i].text_size) {
                fprintf(fp, ", ");
            }
            if (j % 12 == 11 || j + 1 == entries[i].text_size) {
                fprintf(fp, "\n");
            }
        }
        fprintf(fp, "};\n");
        fprintf(fp, "static const lr_stencil_reloc_t lr_stencil_%s_relocs[] = {\n", id);
        for (j = 0; j < entries[i].reloc_count; j++) {
            fprintf(fp, "    { %u, %u, %s }%s\n",
                    (unsigned)entries[i].relocs[j].offset,
                    (unsigned)entries[i].relocs[j].size,
                    hole_name(entries[i].relocs[j].hole),
                    (j + 1 < entries[i].reloc_count) ? "," : "");
        }
        fprintf(fp, "};\n");
        fprintf(fp, "static const lr_stencil_t lr_stencil_%s = {\n", id);
        fprintf(fp, "    \"%s\",\n", entries[i].stem);
        fprintf(fp, "    lr_stencil_%s_bytes,\n", id);
        fprintf(fp, "    (uint16_t)%zu,\n", entries[i].text_size);
        fprintf(fp, "    lr_stencil_%s_relocs,\n", id);
        fprintf(fp, "    (uint8_t)%zu\n", entries[i].reloc_count);
        fprintf(fp, "};\n\n");
        free(id);
    }
    fprintf(fp, "static const lr_stencil_t *const lr_generated_stencils[] = {\n");
    for (i = 0; i < count; i++) {
        char *id = sanitize_identifier(entries[i].stem);
        if (!id) {
            fclose(fp);
            return -1;
        }
        fprintf(fp, "    &lr_stencil_%s%s\n", id, (i + 1 < count) ? "," : "");
        free(id);
    }
    fprintf(fp, "};\n");
    fprintf(fp, "static const size_t lr_generated_stencils_count =\n");
    fprintf(fp, "    sizeof(lr_generated_stencils) / sizeof(lr_generated_stencils[0]);\n\n");
    fprintf(fp, "#endif\n");
    fclose(fp);
    return 0;
}

static void free_entries(stencil_entry_t *entries, size_t count) {
    size_t i;
    if (!entries) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(entries[i].stem);
        free(entries[i].text);
        free(entries[i].relocs);
    }
    free(entries);
}

int main(int argc, char **argv) {
    const char *input_dir = NULL;
    const char *output = NULL;
    const char *compiler = "cc";
    source_file_t *sources = NULL;
    size_t source_count = 0;
    stencil_entry_t *entries = NULL;
    size_t i;
    int rc = 1;
    char tmp_dir[PATH_MAX];

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--input-dir") == 0 && i + 1 < (size_t)argc) {
            input_dir = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < (size_t)argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--compiler") == 0 && i + 1 < (size_t)argc) {
            compiler = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!input_dir || !output) {
        usage(argv[0]);
        return 1;
    }

    if (snprintf(tmp_dir, sizeof(tmp_dir), "%s.tmp.%ld", output, (long)getpid()) >= (int)sizeof(tmp_dir)) {
        fprintf(stderr, "stencil_gen: temp dir path too long\n");
        return 1;
    }
    if (mkdir(tmp_dir, 0700) != 0) {
        fprintf(stderr, "stencil_gen: failed to create temp dir '%s': %s\n",
                tmp_dir, strerror(errno));
        return 1;
    }

    if (list_sources(input_dir, &sources, &source_count) != 0) {
        goto cleanup;
    }
    entries = (stencil_entry_t *)calloc(source_count, sizeof(*entries));
    if (!entries) {
        goto cleanup;
    }

    for (i = 0; i < source_count; i++) {
        char *obj_path = path_join(tmp_dir, sources[i].stem);
        char *obj_path_full;
        if (!obj_path) {
            goto cleanup;
        }
        obj_path_full = (char *)realloc(obj_path, strlen(obj_path) + 3);
        if (!obj_path_full) {
            free(obj_path);
            goto cleanup;
        }
        obj_path = obj_path_full;
        strcat(obj_path, ".o");
        entries[i].stem = dup_cstr(sources[i].stem);
        if (!entries[i].stem) {
            free(obj_path);
            goto cleanup;
        }
        if (compile_stencil_source(compiler, sources[i].path, obj_path) != 0) {
            fprintf(stderr, "stencil_gen: compile failed for '%s'\n", sources[i].path);
            free(obj_path);
            goto cleanup;
        }
        if (parse_elf_object(obj_path, &entries[i]) != 0) {
            free(obj_path);
            goto cleanup;
        }
        unlink(obj_path);
        free(obj_path);
    }

    if (write_header(output, entries, source_count) != 0) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (sources) {
        for (i = 0; i < source_count; i++) {
            char *obj_path = path_join(tmp_dir, sources[i].stem);
            char *obj_path_full;
            if (!obj_path) {
                continue;
            }
            obj_path_full = (char *)realloc(obj_path, strlen(obj_path) + 3);
            if (obj_path_full) {
                obj_path = obj_path_full;
                strcat(obj_path, ".o");
                unlink(obj_path);
            }
            free(obj_path);
        }
    }
    rmdir(tmp_dir);
    free_entries(entries, source_count);
    free_sources(sources, source_count);
    return rc;
}
