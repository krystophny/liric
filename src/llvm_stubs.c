#if !defined(LIRIC_HAVE_REAL_LLVM_BACKEND) || !LIRIC_HAVE_REAL_LLVM_BACKEND
void LLVMInitializeAArch64Target(void) {}
void LLVMInitializeAArch64TargetInfo(void) {}
void LLVMInitializeAArch64TargetMC(void) {}
void LLVMInitializeAArch64AsmPrinter(void) {}
void LLVMInitializeAArch64AsmParser(void) {}
void LLVMInitializeX86Target(void) {}
void LLVMInitializeX86TargetInfo(void) {}
void LLVMInitializeX86TargetMC(void) {}
void LLVMInitializeX86AsmPrinter(void) {}
void LLVMInitializeX86AsmParser(void) {}
#else
int liric_real_llvm_backend_uses_system_target_init_symbols = 1;
#endif
