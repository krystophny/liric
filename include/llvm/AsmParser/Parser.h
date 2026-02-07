#ifndef LLVM_ASMPARSER_PARSER_H
#define LLVM_ASMPARSER_PARSER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>

namespace llvm {

class Module;

inline std::unique_ptr<Module> parseAssemblyString(
    StringRef AsmString, SMDiagnostic &Err, LLVMContext &Context) {
    (void)AsmString;
    (void)Context;
    Err = SMDiagnostic("parseAssemblyString not yet implemented in liric");
    return nullptr;
}

inline std::unique_ptr<Module> parseAssemblyFile(
    StringRef Filename, SMDiagnostic &Err, LLVMContext &Context) {
    (void)Filename;
    (void)Context;
    Err = SMDiagnostic("parseAssemblyFile not yet implemented in liric");
    return nullptr;
}

} // namespace llvm

#endif
