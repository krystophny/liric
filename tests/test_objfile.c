#include <liric/liric.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Forward-declare internal types and functions to avoid enum clash
 * between public liric.h and internal ir.h (both define LR_FCMP_*) */
typedef struct lr_target lr_target_t;
const lr_target_t *lr_target_host(void);
int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out);

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

static lr_module_t *build_ret42_module(void) {
    lr_module_t *m = lr_module_create_new();
    if (!m) return NULL;
    lr_type_t *i32 = lr_type_i32_get(m);
    lr_func_t *f = lr_func_define(m, "f", i32, NULL, 0, false);
    if (!f) { lr_module_free(m); return NULL; }
    lr_block_t *entry = lr_block_new(f, m, "entry");
    lr_build_ret(m, entry, LR_IMM(42, i32));
    return m;
}

#if !defined(__APPLE__)

static lr_module_t *build_call_module(void) {
    lr_module_t *m = lr_module_create_new();
    if (!m) return NULL;
    lr_type_t *i32 = lr_type_i32_get(m);

    lr_type_t *ext_params[] = { i32 };
    lr_func_declare_ext(m, "external_func", i32, ext_params, 1, false);
    uint32_t ext_gid = lr_symbol_intern(m, "external_func");

    lr_type_t *params[] = { i32 };
    lr_func_t *f = lr_func_define(m, "caller", i32, params, 1, false);
    lr_block_t *entry = lr_block_new(f, m, "entry");
    uint32_t va = lr_func_param_vreg(f, 0);

    lr_type_t *ptr = lr_type_ptr_get(m);
    lr_operand_desc_t call_args[] = { LR_VREG(va, i32) };
    uint32_t result = lr_build_call(m, entry, f, i32,
                                     LR_GLOBAL(ext_gid, ptr),
                                     call_args, 1);
    lr_build_ret(m, entry, LR_VREG(result, i32));
    return m;
}

int test_objfile_elf_header(void) {
    lr_module_t *m = build_ret42_module();
    TEST_ASSERT(m != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(m, target, fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    TEST_ASSERT(fsize >= 64, "file size >= 64 (ELF header)");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[64];
    size_t rd = fread(hdr, 1, 64, fp);
    TEST_ASSERT_EQ(rd, 64, "read 64 bytes");

    TEST_ASSERT_EQ(hdr[0], 0x7F, "ELF magic byte 0");
    TEST_ASSERT_EQ(hdr[1], 'E', "ELF magic byte 1");
    TEST_ASSERT_EQ(hdr[2], 'L', "ELF magic byte 2");
    TEST_ASSERT_EQ(hdr[3], 'F', "ELF magic byte 3");
    TEST_ASSERT_EQ(hdr[4], 2, "ELFCLASS64");
    TEST_ASSERT_EQ(hdr[5], 1, "ELFDATA2LSB");
    TEST_ASSERT_EQ(hdr[6], 1, "EV_CURRENT");

    uint16_t e_type = (uint16_t)(hdr[16] | (hdr[17] << 8));
    TEST_ASSERT_EQ(e_type, 1, "ET_REL");

    uint16_t e_machine = (uint16_t)(hdr[18] | (hdr[19] << 8));
    TEST_ASSERT_EQ(e_machine, 62, "EM_X86_64");

    fclose(fp);
    lr_module_free(m);
    return 0;
}

int test_objfile_elf_symbols(void) {
    lr_module_t *m = build_ret42_module();
    TEST_ASSERT(m != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(m, target, fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)fsize);
    TEST_ASSERT(buf != NULL, "alloc buffer");
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    TEST_ASSERT(nread == (size_t)fsize, "read full object buffer");
    fclose(fp);

    uint64_t e_shoff = 0;
    memcpy(&e_shoff, buf + 40, 8);
    uint16_t e_shentsize = 0;
    memcpy(&e_shentsize, buf + 58, 2);
    uint16_t e_shnum = 0;
    memcpy(&e_shnum, buf + 60, 2);

    TEST_ASSERT(e_shnum >= 5, "at least 5 sections");

    uint64_t symtab_off = 0;
    uint64_t symtab_size = 0;
    uint32_t symtab_link = 0;
    for (uint16_t i = 0; i < e_shnum; i++) {
        uint8_t *sh = buf + e_shoff + i * e_shentsize;
        uint32_t sh_type = 0;
        memcpy(&sh_type, sh + 4, 4);
        if (sh_type == 2) { /* SHT_SYMTAB */
            memcpy(&symtab_off, sh + 24, 8);
            memcpy(&symtab_size, sh + 32, 8);
            memcpy(&symtab_link, sh + 40, 4);
            break;
        }
    }
    TEST_ASSERT(symtab_off > 0, "found .symtab");
    TEST_ASSERT(symtab_size > 0, "symtab not empty");

    uint8_t *strtab_sh = buf + e_shoff + symtab_link * e_shentsize;
    uint64_t strtab_off = 0;
    memcpy(&strtab_off, strtab_sh + 24, 8);

    uint32_t num_syms = (uint32_t)(symtab_size / 24);
    bool found_f = false;
    for (uint32_t i = 0; i < num_syms; i++) {
        uint8_t *sym = buf + symtab_off + i * 24;
        uint32_t st_name = 0;
        memcpy(&st_name, sym, 4);
        if (st_name > 0) {
            const char *name = (const char *)(buf + strtab_off + st_name);
            if (strcmp(name, "f") == 0) {
                found_f = true;
                uint8_t st_info = sym[4];
                TEST_ASSERT_EQ((st_info >> 4), 1, "f is STB_GLOBAL");
                uint16_t st_shndx = 0;
                memcpy(&st_shndx, sym + 6, 2);
                TEST_ASSERT(st_shndx != 0, "f is defined (shndx != SHN_UNDEF)");
            }
        }
    }
    TEST_ASSERT(found_f, "symbol 'f' found in .symtab");

    free(buf);
    lr_module_free(m);
    return 0;
}

int test_objfile_elf_call_relocation(void) {
    lr_module_t *m = build_call_module();
    TEST_ASSERT(m != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(m, target, fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = malloc((size_t)fsize);
    TEST_ASSERT(buf != NULL, "alloc buffer");
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    TEST_ASSERT(nread == (size_t)fsize, "read full object buffer");
    fclose(fp);

    uint64_t e_shoff = 0;
    memcpy(&e_shoff, buf + 40, 8);
    uint16_t e_shentsize = 0;
    memcpy(&e_shentsize, buf + 58, 2);
    uint16_t e_shnum = 0;
    memcpy(&e_shnum, buf + 60, 2);

    uint64_t rela_off = 0;
    uint64_t rela_size = 0;
    for (uint16_t i = 0; i < e_shnum; i++) {
        uint8_t *sh = buf + e_shoff + i * e_shentsize;
        uint32_t sh_type = 0;
        memcpy(&sh_type, sh + 4, 4);
        if (sh_type == 4) { /* SHT_RELA */
            memcpy(&rela_off, sh + 24, 8);
            memcpy(&rela_size, sh + 32, 8);
            break;
        }
    }

    TEST_ASSERT(rela_off > 0, "found .rela.text");
    TEST_ASSERT(rela_size > 0, "has relocations");

    uint32_t num_relas = (uint32_t)(rela_size / 24);
    TEST_ASSERT(num_relas >= 1, "at least 1 relocation");

    bool found_plt32 = false;
    for (uint32_t i = 0; i < num_relas; i++) {
        uint8_t *rela = buf + rela_off + i * 24;
        uint64_t r_info = 0;
        memcpy(&r_info, rela + 8, 8);
        uint32_t r_type = (uint32_t)(r_info & 0xFFFFFFFF);
        if (r_type == 4) { /* R_X86_64_PLT32 */
            found_plt32 = true;
            int64_t r_addend = 0;
            memcpy(&r_addend, rela + 16, 8);
            TEST_ASSERT_EQ(r_addend, -4, "PLT32 addend = -4");
        }
    }
    TEST_ASSERT(found_plt32, "found R_X86_64_PLT32 relocation");

    free(buf);
    lr_module_free(m);
    return 0;
}

int test_objfile_elf_readelf_validates(void) {
    lr_module_t *m = build_ret42_module();
    TEST_ASSERT(m != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *path = "/tmp/liric_test_objfile.o";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_object(m, target, fp);
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "readelf -h %s > /dev/null 2>&1", path);
    rc = system(cmd);
    TEST_ASSERT_EQ(rc, 0, "readelf -h validates");

    snprintf(cmd, sizeof(cmd), "readelf -s %s > /dev/null 2>&1", path);
    rc = system(cmd);
    TEST_ASSERT_EQ(rc, 0, "readelf -s validates");

    snprintf(cmd, sizeof(cmd), "readelf -S %s > /dev/null 2>&1", path);
    rc = system(cmd);
    TEST_ASSERT_EQ(rc, 0, "readelf -S validates");

    remove(path);
    lr_module_free(m);
    return 0;
}

#else /* __APPLE__ */

int test_objfile_macho_header(void) {
    lr_module_t *m = build_ret42_module();
    TEST_ASSERT(m != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(m, target, fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[32];
    size_t rd = fread(hdr, 1, 32, fp);
    TEST_ASSERT_EQ(rd, 32, "read 32 bytes");

    uint32_t magic = (uint32_t)(hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24));
    TEST_ASSERT_EQ(magic, 0xFEEDFACFu, "Mach-O magic");

    fclose(fp);
    lr_module_free(m);
    return 0;
}

#endif
