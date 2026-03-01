#include <liric/liric_session.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

typedef struct lr_target lr_target_t;
const lr_target_t *lr_target_host(void);
const lr_target_t *lr_target_by_name(const char *name);
int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out);
int lr_emit_executable(lr_module_t *m, const lr_target_t *target, FILE *out,
                       const char *entry_symbol);

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

typedef struct {
    lr_session_t *session;
    lr_module_t *module;
} built_module_t;

static built_module_t build_ret42_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    if (lr_session_func_begin(s, "f", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

#if !defined(__APPLE__)

static built_module_t build_call_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    lr_session_declare(s, "external_func", i32,
                       (lr_type_t *[]){i32}, 1, false, &err);

    if (lr_session_func_begin(s, "caller", i32,
                              (lr_type_t *[]){i32}, 1, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t va = lr_session_param(s, 0);
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t ext_gid = lr_session_intern(s, "external_func");
    lr_operand_desc_t args[] = {LR_VREG(va, i32)};
    uint32_t cr = lr_emit_call(s, i32, LR_GLOBAL(ext_gid, ptr), args, 1);
    lr_emit_ret(s, LR_VREG(cr, i32));

    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

static built_module_t build_module_init_symbol_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *v = lr_type_void_s(s);
    lr_type_t *i64 = lr_type_i64_s(s);
    if (lr_session_func_begin(s, "__lfortran_module_init_demo", v,
                              NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret_void(s);
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }

    if (lr_session_func_begin(s, "_copy_demo_t", v, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret_void(s);
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }

    lr_session_global(s, "_Type_Info_t", i64, false, NULL, 0);
    lr_session_global(s, "__module_file_common_block_demo", i64, false, NULL, 0);
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

int test_objfile_elf_header(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(bm.module, target, fp);
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
#if defined(__aarch64__)
    TEST_ASSERT_EQ(e_machine, 183, "EM_AARCH64");
#else
    TEST_ASSERT_EQ(e_machine, 62, "EM_X86_64");
#endif

    fclose(fp);
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_elf_symbols(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(bm.module, target, fp);
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
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_elf_lfortran_module_init_symbol_is_weak(void) {
    built_module_t bm = build_module_init_symbol_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(bm.module, target, fp);
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

    bool found_module_init = false;
    bool found_copy_helper = false;
    bool found_type_info = false;
    bool found_common_block = false;
    uint32_t num_syms = (uint32_t)(symtab_size / 24);
    for (uint32_t i = 0; i < num_syms; i++) {
        uint8_t *sym = buf + symtab_off + i * 24;
        uint32_t st_name = 0;
        memcpy(&st_name, sym, 4);
        if (st_name == 0)
            continue;
        const char *name = (const char *)(buf + strtab_off + st_name);
        if (strcmp(name, "__lfortran_module_init_demo") != 0 &&
            strcmp(name, "_copy_demo_t") != 0 &&
            strcmp(name, "_Type_Info_t") != 0 &&
            strcmp(name, "__module_file_common_block_demo") != 0)
            continue;
        uint8_t st_info = sym[4];
        TEST_ASSERT_EQ((st_info >> 4), 2, "helper is STB_WEAK");
        uint16_t st_shndx = 0;
        memcpy(&st_shndx, sym + 6, 2);
        TEST_ASSERT(st_shndx != 0, "helper is defined");
        if (strcmp(name, "__lfortran_module_init_demo") == 0)
            found_module_init = true;
        else if (strcmp(name, "_copy_demo_t") == 0)
            found_copy_helper = true;
        else if (strcmp(name, "_Type_Info_t") == 0)
            found_type_info = true;
        else if (strcmp(name, "__module_file_common_block_demo") == 0)
            found_common_block = true;
    }
    TEST_ASSERT(found_module_init, "module init symbol found in .symtab");
    TEST_ASSERT(found_copy_helper, "copy helper symbol found in .symtab");
    TEST_ASSERT(found_type_info, "type info symbol found in .symtab");
    TEST_ASSERT(found_common_block, "common block symbol found in .symtab");

    free(buf);
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_elf_call_relocation(void) {
    built_module_t bm = build_call_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(bm.module, target, fp);
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
    uint32_t rela_link = 0;
    for (uint16_t i = 0; i < e_shnum; i++) {
        uint8_t *sh = buf + e_shoff + i * e_shentsize;
        uint32_t sh_type = 0;
        memcpy(&sh_type, sh + 4, 4);
        if (sh_type == 4) { /* SHT_RELA */
            memcpy(&rela_off, sh + 24, 8);
            memcpy(&rela_size, sh + 32, 8);
            memcpy(&rela_link, sh + 40, 4);
            break;
        }
    }

    TEST_ASSERT(rela_off > 0, "found .rela.text");
    TEST_ASSERT(rela_size > 0, "has relocations");
    TEST_ASSERT(rela_link < e_shnum, "valid .rela.text sh_link");

    uint8_t *symtab_sh = buf + e_shoff + rela_link * e_shentsize;
    uint64_t symtab_off = 0;
    uint64_t symtab_size = 0;
    uint32_t symtab_link = 0;
    memcpy(&symtab_off, symtab_sh + 24, 8);
    memcpy(&symtab_size, symtab_sh + 32, 8);
    memcpy(&symtab_link, symtab_sh + 40, 4);
    TEST_ASSERT(symtab_off > 0, "symtab offset");
    TEST_ASSERT(symtab_size >= 24, "symtab size");
    TEST_ASSERT(symtab_link < e_shnum, "valid symtab sh_link");

    uint8_t *strtab_sh = buf + e_shoff + symtab_link * e_shentsize;
    uint64_t strtab_off = 0;
    memcpy(&strtab_off, strtab_sh + 24, 8);
    TEST_ASSERT(strtab_off > 0, "strtab offset");

    uint32_t num_relas = (uint32_t)(rela_size / 24);
    TEST_ASSERT(num_relas >= 1, "at least 1 relocation");

    uint32_t num_syms = (uint32_t)(symtab_size / 24);
    bool found_expected_reloc = false;
    bool found_external_func_reloc = false;
#if defined(__aarch64__)
#endif
    for (uint32_t i = 0; i < num_relas; i++) {
        uint8_t *rela = buf + rela_off + i * 24;
        uint64_t r_info = 0;
        memcpy(&r_info, rela + 8, 8);
        uint32_t r_sym = (uint32_t)(r_info >> 32);
        uint32_t r_type = (uint32_t)(r_info & 0xFFFFFFFF);
        if (r_sym < num_syms) {
            uint8_t *sym = buf + symtab_off + (size_t)r_sym * 24u;
            uint32_t st_name = 0;
            memcpy(&st_name, sym, 4);
            if (st_name > 0) {
                const char *name = (const char *)(buf + strtab_off + st_name);
                if (strcmp(name, "external_func") == 0)
                    found_external_func_reloc = true;
            }
        }
#if defined(__aarch64__)
        (void)r_type;
#else
        /* Streaming ISel emits indirect calls via R10 with a
           GOTPCRELX relocation (type 41) for external symbols. */
        if (r_type == 41) { /* R_X86_64_GOTPCRELX */
            found_expected_reloc = true;
            int64_t r_addend = 0;
            memcpy(&r_addend, rela + 16, 8);
            TEST_ASSERT_EQ(r_addend, -4, "GOTPCRELX addend = -4");
        }
#endif
    }
#if defined(__aarch64__)
    (void)found_expected_reloc;
#else
    TEST_ASSERT(found_expected_reloc, "found R_X86_64_GOTPCRELX relocation");
#endif
    TEST_ASSERT(found_external_func_reloc, "relocation targets external_func");

    free(buf);
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_elf_readelf_validates(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *path = "/tmp/liric_test_objfile.o";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_object(bm.module, target, fp);
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
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_elf_executable_aarch64_header(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_by_name("aarch64");
    TEST_ASSERT(target != NULL, "aarch64 target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_executable(bm.module, target, fp, "f");
    TEST_ASSERT_EQ(rc, 0, "emit aarch64 executable");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[64];
    size_t rd = fread(hdr, 1, 64, fp);
    TEST_ASSERT_EQ(rd, 64, "read 64 bytes");

    TEST_ASSERT_EQ(hdr[0], 0x7F, "ELF magic byte 0");
    TEST_ASSERT_EQ(hdr[1], 'E', "ELF magic byte 1");
    TEST_ASSERT_EQ(hdr[2], 'L', "ELF magic byte 2");
    TEST_ASSERT_EQ(hdr[3], 'F', "ELF magic byte 3");

    uint16_t e_type = (uint16_t)(hdr[16] | (hdr[17] << 8));
    TEST_ASSERT_EQ(e_type, 2, "ET_EXEC");

    uint16_t e_machine = (uint16_t)(hdr[18] | (hdr[19] << 8));
    TEST_ASSERT_EQ(e_machine, 183, "EM_AARCH64");

    fclose(fp);
    lr_session_destroy(bm.session);
    return 0;
}

static built_module_t build_ret42_module_mode(lr_session_mode_t mode) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = mode;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    if (lr_session_func_begin(s, "f", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

int test_objfile_session_emit_object_stream_direct(void) {
    built_module_t bm = build_ret42_module_mode(LR_MODE_DIRECT);
    TEST_ASSERT(bm.session != NULL, "direct session create");

    lr_error_t err = {0};
    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_session_emit_object_stream(bm.session, fp, &err);
    TEST_ASSERT_EQ(rc, 0, "direct emit object stream");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    TEST_ASSERT(fsize >= 64, "direct .o size >= 64");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[4];
    size_t rd = fread(hdr, 1, 4, fp);
    TEST_ASSERT_EQ(rd, 4, "read 4 bytes");
    TEST_ASSERT_EQ(hdr[0], 0x7F, "ELF magic 0");
    TEST_ASSERT_EQ(hdr[1], 'E', "ELF magic 1");
    TEST_ASSERT_EQ(hdr[2], 'L', "ELF magic 2");
    TEST_ASSERT_EQ(hdr[3], 'F', "ELF magic 3");

    fclose(fp);
    lr_session_destroy(bm.session);
    return 0;
}

int test_objfile_session_emit_object_stream_ir(void) {
    built_module_t bm = build_ret42_module_mode(LR_MODE_IR);
    TEST_ASSERT(bm.session != NULL, "ir session create");

    lr_error_t err = {0};
    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_session_emit_object_stream(bm.session, fp, &err);
    TEST_ASSERT_EQ(rc, 0, "ir emit object stream");

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    TEST_ASSERT(fsize >= 64, "ir .o size >= 64");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[4];
    size_t rd = fread(hdr, 1, 4, fp);
    TEST_ASSERT_EQ(rd, 4, "read 4 bytes");
    TEST_ASSERT_EQ(hdr[0], 0x7F, "ELF magic 0");
    TEST_ASSERT_EQ(hdr[1], 'E', "ELF magic 1");
    TEST_ASSERT_EQ(hdr[2], 'L', "ELF magic 2");
    TEST_ASSERT_EQ(hdr[3], 'F', "ELF magic 3");

    fclose(fp);
    lr_session_destroy(bm.session);
    return 0;
}

#if defined(__linux__)

static built_module_t build_main_ret42_module_mode(lr_session_mode_t mode) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = mode;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    if (lr_session_func_begin(s, "main", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

int test_objfile_link_and_run_direct(void) {
    built_module_t bm = build_main_ret42_module_mode(LR_MODE_DIRECT);
    TEST_ASSERT(bm.session != NULL, "direct session create");

    lr_error_t err = {0};
    const char *obj_path = "/tmp/liric_test_direct_link.o";
    FILE *fp = fopen(obj_path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_session_emit_object_stream(bm.session, fp, &err);
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit direct .o");

    const char *exe_path = "/tmp/liric_test_direct_linked";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cc -o %s %s 2>/dev/null", exe_path, obj_path);
    rc = system(cmd);
    remove(obj_path);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "cc link direct .o");

    chmod(exe_path, 0755);
    int status = system(exe_path);
    TEST_ASSERT(WIFEXITED(status), "exited normally");
    TEST_ASSERT_EQ(WEXITSTATUS(status), 42, "exit code 42");

    remove(exe_path);
    lr_session_destroy(bm.session);
    return 0;
}

#endif /* __linux__ link-and-run tests */

#if defined(__linux__)

int test_objfile_elf_exe_runs(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *path = "/tmp/liric_test_elf_exe";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "f");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit executable");

    chmod(path, 0755);
    int status = system(path);
    TEST_ASSERT(WIFEXITED(status), "exited normally");
    TEST_ASSERT_EQ(WEXITSTATUS(status), 42, "exit code 42");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

static built_module_t build_main_ret42_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    if (lr_session_func_begin(s, "main", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

int test_objfile_link_and_run(void) {
    built_module_t bm = build_main_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *obj_path = "/tmp/liric_test_link.o";
    FILE *fp = fopen(obj_path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_object(bm.module, target, fp);
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    const char *exe_path = "/tmp/liric_test_linked";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cc -o %s %s 2>/dev/null", exe_path, obj_path);
    rc = system(cmd);
    remove(obj_path);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "cc link succeeded");

    chmod(exe_path, 0755);
    int status = system(exe_path);
    TEST_ASSERT(WIFEXITED(status), "exited normally");
    TEST_ASSERT_EQ(WEXITSTATUS(status), 42, "exit code 42");

    remove(exe_path);
    lr_session_destroy(bm.session);
    return 0;
}

static built_module_t build_puts_hello_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i8 = lr_type_i8_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    lr_session_declare(s, "puts", i32, (lr_type_t *[]){ptr}, 1, false, &err);

    const char hello[] = "Hello from liric!\0";
    lr_type_t *str_ty = lr_type_array_s(s, i8, sizeof(hello));
    lr_session_global(s, ".str", str_ty, true, hello, sizeof(hello));

    if (lr_session_func_begin(s, "main", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t str_gid = lr_session_intern(s, ".str");
    uint32_t puts_gid = lr_session_intern(s, "puts");

    lr_operand_desc_t call_args[] = {LR_GLOBAL(str_gid, ptr)};
    lr_emit_call(s, i32, LR_GLOBAL(puts_gid, ptr), call_args, 1);
    lr_emit_ret(s, LR_IMM(0, i32));

    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

static built_module_t build_muldc3_import_module(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    lr_session_declare(s, "__muldc3", i32, (lr_type_t *[]){i32}, 1, false, &err);

    if (lr_session_func_begin(s, "main", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t mul_gid = lr_session_intern(s, "__muldc3");
    lr_operand_desc_t call_args[] = {LR_IMM(0, i32)};
    (void)lr_emit_call(s, i32, LR_GLOBAL(mul_gid, ptr), call_args, 1);
    lr_emit_ret(s, LR_IMM(0, i32));

    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

static int host_supports_dynelf_tests(const lr_target_t *target) {
    const lr_target_t *x86_64 = lr_target_by_name("x86_64");
    return target && x86_64 && target == x86_64;
}

int test_dynelf_puts_hello(void) {
    built_module_t bm = build_puts_hello_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");
    if (!host_supports_dynelf_tests(target)) {
        lr_session_destroy(bm.session);
        return 0;
    }

    const char *path = "/tmp/liric_test_dynelf_puts";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit dynamic executable");

    chmod(path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s > /tmp/liric_test_dynelf_out.txt 2>&1", path);
    int status = system(cmd);
    TEST_ASSERT(WIFEXITED(status), "exited normally");
    TEST_ASSERT_EQ(WEXITSTATUS(status), 0, "exit code 0");

    FILE *out = fopen("/tmp/liric_test_dynelf_out.txt", "r");
    TEST_ASSERT(out != NULL, "open output");
    char line[256] = {0};
    char *got = fgets(line, sizeof(line), out);
    fclose(out);
    TEST_ASSERT(got != NULL, "read output line");
    TEST_ASSERT(strstr(line, "Hello from liric!") != NULL, "output contains greeting");

    remove(path);
    remove("/tmp/liric_test_dynelf_out.txt");
    lr_session_destroy(bm.session);
    return 0;
}

int test_dynelf_readelf_dynamic(void) {
    built_module_t bm = build_puts_hello_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");
    if (!host_supports_dynelf_tests(target)) {
        lr_session_destroy(bm.session);
        return 0;
    }

    const char *path = "/tmp/liric_test_dynelf_readelf";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit dynamic executable");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "readelf -d %s 2>/dev/null | grep -q 'libc.so.6'", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "readelf shows DT_NEEDED libc.so.6");

    snprintf(cmd, sizeof(cmd), "readelf -r %s 2>/dev/null | grep -q 'GLOB_DAT'", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "readelf shows R_X86_64_GLOB_DAT");

    snprintf(cmd, sizeof(cmd), "readelf -l %s 2>/dev/null | grep -q 'INTERP'", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "readelf shows PT_INTERP");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

int test_dynelf_ldd_check(void) {
    built_module_t bm = build_puts_hello_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");
    if (!host_supports_dynelf_tests(target)) {
        lr_session_destroy(bm.session);
        return 0;
    }

    const char *path = "/tmp/liric_test_dynelf_ldd";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit dynamic executable");
    chmod(path, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ldd %s 2>/dev/null | grep -q 'libc.so'", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "ldd shows libc.so dependency");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

int test_dynelf_complex_helper_adds_libgcc_needed(void) {
    built_module_t bm = build_muldc3_import_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");
    if (!host_supports_dynelf_tests(target)) {
        lr_session_destroy(bm.session);
        return 0;
    }

    const char *path = "/tmp/liric_test_dynelf_libgcc_needed";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit dynamic executable");

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "readelf -d %s 2>/dev/null | grep -q 'libgcc_s.so.1'", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "readelf shows DT_NEEDED libgcc_s.so.1");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

#endif /* __linux__ */

#else /* __APPLE__ */

int test_objfile_macho_header(void) {
    built_module_t bm = build_ret42_module();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    FILE *fp = tmpfile();
    TEST_ASSERT(fp != NULL, "tmpfile");

    int rc = lr_emit_object(bm.module, target, fp);
    TEST_ASSERT_EQ(rc, 0, "emit object");

    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[32];
    size_t rd = fread(hdr, 1, 32, fp);
    TEST_ASSERT_EQ(rd, 32, "read 32 bytes");

    uint32_t magic = (uint32_t)(hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24));
    TEST_ASSERT_EQ(magic, 0xFEEDFACFu, "Mach-O magic");

    fclose(fp);
    lr_session_destroy(bm.session);
    return 0;
}

static built_module_t build_main_ret42_module_macho(void) {
    built_module_t result = {0};
    lr_session_config_t cfg = {0};
    cfg.mode = LR_MODE_IR;
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) return result;

    lr_type_t *i32 = lr_type_i32_s(s);
    if (lr_session_func_begin(s, "main", i32, NULL, 0, false, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));
    if (lr_session_func_end(s, NULL, &err) != 0) {
        lr_session_destroy(s);
        return result;
    }
    result.session = s;
    result.module = lr_session_module(s);
    return result;
}

int test_macho_exe_runs(void) {
    built_module_t bm = build_main_ret42_module_macho();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *path = "/tmp/liric_test_macho_exe";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit executable");

    chmod(path, 0755);
    int status = system(path);
    TEST_ASSERT(WIFEXITED(status), "exited normally");
    TEST_ASSERT_EQ(WEXITSTATUS(status), 42, "exit code 42");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

int test_macho_exe_codesign_verify(void) {
    built_module_t bm = build_main_ret42_module_macho();
    TEST_ASSERT(bm.module != NULL, "module create");

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target");

    const char *path = "/tmp/liric_test_macho_codesign";
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT(fp != NULL, "fopen");

    int rc = lr_emit_executable(bm.module, target, fp, "main");
    fclose(fp);
    TEST_ASSERT_EQ(rc, 0, "emit executable");

    chmod(path, 0755);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "codesign --verify --verbose %s 2>/dev/null", path);
    rc = system(cmd);
    TEST_ASSERT(WIFEXITED(rc) && WEXITSTATUS(rc) == 0,
                "codesign --verify passes");

    remove(path);
    lr_session_destroy(bm.session);
    return 0;
}

#endif
