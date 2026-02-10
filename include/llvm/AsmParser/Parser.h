#ifndef LLVM_ASMPARSER_PARSER_H
#define LLVM_ASMPARSER_PARSER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <string>
#include <fstream>
#include <vector>

namespace llvm {

inline bool is_lfortran_jit_wrapper_ir(StringRef AsmString) {
    const std::string s = AsmString.str();
    return s.find("declare i32 @main(i32, i8**)\n") != std::string::npos &&
           s.find("define i32 @__lfortran_jit_entry(i32 %argc, i8** %argv)") != std::string::npos &&
           s.find("call i32 @main(i32 %argc, i8** %argv)") != std::string::npos &&
           s.find("ret i32 %ret") != std::string::npos;
}

inline std::unique_ptr<Module> build_lfortran_jit_wrapper_module(
    LLVMContext &Context, SMDiagnostic &Err) {
    std::unique_ptr<Module> M = std::make_unique<Module>("asm", Context);
    Type *i32 = Type::getInt32Ty(Context);
    Type *i8_ptr = Type::getInt8PtrTy(Context);
    Type *argv_ty = PointerType::getUnqual(i8_ptr);
    Type *params[] = { i32, argv_ty };
    FunctionType *fty = FunctionType::get(i32, params, false);
    if (!fty) {
        Err = SMDiagnostic("failed to create wrapper function type");
        return nullptr;
    }

    Function *main_decl = Function::Create(
        fty, GlobalValue::ExternalLinkage, "main", *M);
    Function *entry_fn = Function::Create(
        fty, GlobalValue::ExternalLinkage, "__lfortran_jit_entry", *M);
    if (!main_decl || !entry_fn) {
        Err = SMDiagnostic("failed to create wrapper functions");
        return nullptr;
    }

    BasicBlock *entry = BasicBlock::Create(Context, "entry", entry_fn);
    if (!entry) {
        Err = SMDiagnostic("failed to create wrapper entry block");
        return nullptr;
    }

    std::vector<Value *> call_args;
    call_args.reserve(entry_fn->arg_size());
    for (auto it = entry_fn->arg_begin(); it != entry_fn->arg_end(); ++it) {
        call_args.push_back(&(*it));
    }

    IRBuilder<> builder(entry, Context);
    Value *ret = builder.CreateCall(main_decl, call_args, "ret");
    if (!ret) {
        Err = SMDiagnostic("failed to create wrapper call");
        return nullptr;
    }
    builder.CreateRet(ret);

    return M;
}

inline std::unique_ptr<Module> parseAssemblyString(
    StringRef AsmString, SMDiagnostic &Err, LLVMContext &Context) {
    if (is_lfortran_jit_wrapper_ir(AsmString)) {
        std::unique_ptr<Module> M =
            build_lfortran_jit_wrapper_module(Context, Err);
        if (!M) return nullptr;
        lc_module_compat_t *compat = M->getCompat();
        Module::setCurrentModule(compat);
        return M;
    }

    char parse_err[512] = {0};
    lr_module_t *parsed = lr_parse_ll(AsmString.data(), AsmString.size(),
                                      parse_err, sizeof(parse_err));
    if (!parsed) {
        Err = SMDiagnostic(parse_err[0] ? parse_err
                                        : "parseAssemblyString failed in liric");
        return nullptr;
    }

    std::unique_ptr<Module> M = std::make_unique<Module>("asm", Context);
    lc_module_compat_t *compat = M->getCompat();
    if (!compat) {
        lr_module_free(parsed);
        Err = SMDiagnostic("failed to create compat module wrapper");
        return nullptr;
    }

    if (compat->mod) {
        lr_module_free(compat->mod);
    }
    compat->mod = parsed;
    if (compat->ctx) compat->ctx->mod = parsed;
    Module::setCurrentModule(compat);
    return M;
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
