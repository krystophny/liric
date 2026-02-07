#ifndef LLVM_IR_CALLINGCONV_H
#define LLVM_IR_CALLINGCONV_H

namespace llvm {
namespace CallingConv {

enum ID {
    C = 0,
    Fast = 8,
    Cold = 9,
    X86_StdCall = 64,
    X86_FastCall = 65,
};

} // namespace CallingConv
} // namespace llvm

#endif
