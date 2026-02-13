#ifndef LLVM_ASMPARSER_PARSER_H
#define LLVM_ASMPARSER_PARSER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <string>
#include <fstream>

namespace llvm {

inline bool is_lfortran_jit_wrapper_ir(StringRef AsmString) {
    return lc_is_lfortran_jit_wrapper_ir(AsmString.data(), AsmString.size());
}

inline std::unique_ptr<Module> attach_parsed_module(
    lr_module_t *parsed, SMDiagnostic &Err, LLVMContext &Context) {
    if (!parsed) {
        Err = SMDiagnostic("parseAssemblyString failed in liric");
        return nullptr;
    }
    std::unique_ptr<Module> M = std::make_unique<Module>("asm", Context);
    lc_module_compat_t *compat = M->getCompat();
    if (!compat) {
        lr_module_free(parsed);
        Err = SMDiagnostic("failed to create compat module wrapper");
        return nullptr;
    }
    if (compat->mod)
        lr_module_free(compat->mod);
    compat->mod = parsed;
    if (compat->ctx)
        compat->ctx->mod = parsed;
    Module::setCurrentModule(compat);
    return M;
}

inline std::unique_ptr<Module> build_lfortran_jit_wrapper_module(
    LLVMContext &Context, SMDiagnostic &Err) {
    char parse_err[512] = {0};
    lr_module_t *parsed = lc_build_lfortran_jit_wrapper_module(
        parse_err, sizeof(parse_err));
    if (!parsed) {
        Err = SMDiagnostic(parse_err[0] ? parse_err
                                        : "failed to build wrapper module");
        return nullptr;
    }
    return attach_parsed_module(parsed, Err, Context);
}

inline std::unique_ptr<Module> parseAssemblyString(
    StringRef AsmString, SMDiagnostic &Err, LLVMContext &Context) {
    char parse_err[512] = {0};
    lr_module_t *parsed = nullptr;
    if (lc_is_lfortran_jit_wrapper_ir(AsmString.data(), AsmString.size())) {
        parsed = lc_build_lfortran_jit_wrapper_module(parse_err,
                                                      sizeof(parse_err));
    } else {
        parsed = lr_parse_ll(AsmString.data(), AsmString.size(),
                             parse_err, sizeof(parse_err));
    }
    if (!parsed) {
        Err = SMDiagnostic(parse_err[0] ? parse_err
                                        : "parseAssemblyString failed in liric");
        return nullptr;
    }
    return attach_parsed_module(parsed, Err, Context);
}

inline std::unique_ptr<Module> parseAssemblyFile(
    StringRef Filename, SMDiagnostic &Err, LLVMContext &Context) {
    std::ifstream in(Filename.str(), std::ios::in | std::ios::binary);
    if (!in) {
        Err = SMDiagnostic("failed to open LLVM assembly file: " + Filename.str());
        return nullptr;
    }
    std::string src((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return parseAssemblyString(StringRef(src), Err, Context);
}

} // namespace llvm

#endif
