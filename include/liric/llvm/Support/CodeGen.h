#ifndef LLVM_SUPPORT_CODEGEN_H
#define LLVM_SUPPORT_CODEGEN_H

namespace liric_llvm {

namespace Reloc {
enum Model {
    Static,
    PIC_,
    DynamicNoPIC,
    ROPI,
    RWPI,
    ROPI_RWPI
};
} // namespace Reloc

enum class CodeModel {
    Tiny,
    Small,
    Kernel,
    Medium,
    Large
};

enum class CodeGenFileType {
    AssemblyFile,
    ObjectFile,
    Null
};

enum class CodeGenOptLevel {
    None,
    Less,
    Default,
    Aggressive
};

// Backwards compat aliases
namespace CodeGenOpt {
using Level = CodeGenOptLevel;
} // namespace CodeGenOpt

} // namespace liric_llvm

#endif
