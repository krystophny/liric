#ifndef LLVM_SUPPORT_TARGETSELECT_H
#define LLVM_SUPPORT_TARGETSELECT_H

namespace llvm {

inline bool InitializeNativeTarget() { return false; }
inline bool InitializeNativeTargetAsmPrinter() { return false; }
inline bool InitializeNativeTargetAsmParser() { return false; }
inline void InitializeAllTargetInfos() {}
inline void InitializeAllTargets() {}
inline void InitializeAllTargetMCs() {}
inline void InitializeAllAsmParsers() {}
inline void InitializeAllAsmPrinters() {}

} // namespace llvm

#endif
