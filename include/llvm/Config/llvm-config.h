#ifndef LLVM_CONFIG_LLVM_CONFIG_H
#define LLVM_CONFIG_LLVM_CONFIG_H

#define LLVM_VERSION_MAJOR 21
#define LLVM_VERSION_MINOR 0
#define LLVM_VERSION_PATCH 0
#define LLVM_VERSION_STRING "21.0.0-liric"

#if defined(__aarch64__) || defined(_M_ARM64)
#define LLVM_NATIVE_ARCH AArch64
#elif defined(__x86_64__) || defined(_M_X64)
#define LLVM_NATIVE_ARCH X86
#endif

#define LLVM_PASTE3_(a, b, c) a ## b ## c
#define LLVM_PASTE3(a, b, c) LLVM_PASTE3_(a, b, c)

#define LLVM_NATIVE_TARGET LLVM_PASTE3(LLVMInitialize, LLVM_NATIVE_ARCH, Target)
#define LLVM_NATIVE_TARGETINFO LLVM_PASTE3(LLVMInitialize, LLVM_NATIVE_ARCH, TargetInfo)
#define LLVM_NATIVE_TARGETMC LLVM_PASTE3(LLVMInitialize, LLVM_NATIVE_ARCH, TargetMC)
#define LLVM_NATIVE_ASMPRINTER LLVM_PASTE3(LLVMInitialize, LLVM_NATIVE_ARCH, AsmPrinter)
#define LLVM_NATIVE_ASMPARSER LLVM_PASTE3(LLVMInitialize, LLVM_NATIVE_ARCH, AsmParser)

#if defined(__APPLE__) && defined(__aarch64__)
#define LLVM_DEFAULT_TARGET_TRIPLE "aarch64-apple-darwin"
#define LLVM_HOST_TRIPLE "aarch64-apple-darwin"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LLVM_DEFAULT_TARGET_TRIPLE "aarch64-unknown-linux-gnu"
#define LLVM_HOST_TRIPLE "aarch64-unknown-linux-gnu"
#elif defined(__x86_64__) || defined(_M_X64)
#if defined(__APPLE__)
#define LLVM_DEFAULT_TARGET_TRIPLE "x86_64-apple-darwin"
#define LLVM_HOST_TRIPLE "x86_64-apple-darwin"
#else
#define LLVM_DEFAULT_TARGET_TRIPLE "x86_64-unknown-linux-gnu"
#define LLVM_HOST_TRIPLE "x86_64-unknown-linux-gnu"
#endif
#endif

#endif
