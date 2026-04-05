#if !defined(LIRIC_HAVE_REAL_LLVM_BACKEND) || !LIRIC_HAVE_REAL_LLVM_BACKEND
#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##Target(void) {}
#include "liric/llvm/Config/Targets.def"

#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##TargetInfo(void) {}
#include "liric/llvm/Config/Targets.def"

#define LLVM_TARGET(TargetName) void LLVMInitialize##TargetName##TargetMC(void) {}
#include "liric/llvm/Config/Targets.def"

#define LLVM_ASM_PRINTER(TargetName) void LLVMInitialize##TargetName##AsmPrinter(void) {}
#include "liric/llvm/Config/AsmPrinters.def"

#define LLVM_ASM_PARSER(TargetName) void LLVMInitialize##TargetName##AsmParser(void) {}
#include "liric/llvm/Config/AsmParsers.def"
#else
int liric_real_llvm_backend_uses_system_target_init_symbols = 1;
#endif
