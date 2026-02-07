#ifndef LLVM_SUPPORT_TARGETSELECT_H
#define LLVM_SUPPORT_TARGETSELECT_H

#include "llvm/Config/llvm-config.h"

extern "C" {

#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##Target(void);
#include "llvm/Config/Targets.def"

#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##TargetInfo(void);
#include "llvm/Config/Targets.def"

#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##TargetMC(void);
#include "llvm/Config/Targets.def"

#define LLVM_ASM_PRINTER(TargetName) void LLVMInitialize##TargetName##AsmPrinter(void);
#include "llvm/Config/AsmPrinters.def"

#define LLVM_ASM_PARSER(TargetName) void LLVMInitialize##TargetName##AsmParser(void);
#include "llvm/Config/AsmParsers.def"

}

namespace llvm {

inline bool InitializeNativeTarget() {
#ifdef LLVM_NATIVE_TARGET
    LLVM_NATIVE_TARGET();
    LLVM_NATIVE_TARGETINFO();
    LLVM_NATIVE_TARGETMC();
    return false;
#else
    return true;
#endif
}

inline bool InitializeNativeTargetAsmPrinter() {
#ifdef LLVM_NATIVE_ASMPRINTER
    LLVM_NATIVE_ASMPRINTER();
    return false;
#else
    return true;
#endif
}

inline bool InitializeNativeTargetAsmParser() {
#ifdef LLVM_NATIVE_ASMPARSER
    LLVM_NATIVE_ASMPARSER();
    return false;
#else
    return true;
#endif
}

inline void InitializeAllTargetInfos() {
#define LLVM_TARGET(TargetName) LLVMInitialize##TargetName##TargetInfo();
#include "llvm/Config/Targets.def"
}

inline void InitializeAllTargets() {
#define LLVM_TARGET(TargetName) LLVMInitialize##TargetName##Target();
#include "llvm/Config/Targets.def"
}

inline void InitializeAllTargetMCs() {
#define LLVM_TARGET(TargetName) LLVMInitialize##TargetName##TargetMC();
#include "llvm/Config/Targets.def"
}

inline void InitializeAllAsmParsers() {
#define LLVM_ASM_PARSER(TargetName) LLVMInitialize##TargetName##AsmParser();
#include "llvm/Config/AsmParsers.def"
}

inline void InitializeAllAsmPrinters() {
#define LLVM_ASM_PRINTER(TargetName) LLVMInitialize##TargetName##AsmPrinter();
#include "llvm/Config/AsmPrinters.def"
}

} // namespace llvm

#endif
