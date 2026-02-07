#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_ADDRESSSANITIZER_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_ADDRESSSANITIZER_H

namespace llvm {

class AddressSanitizerPass {
public:
    AddressSanitizerPass() = default;
};

class ModuleAddressSanitizerPass {
public:
    ModuleAddressSanitizerPass() = default;
};

} // namespace llvm

#endif
