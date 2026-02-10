#ifndef LLVM_DEBUGINFO_SYMBOLIZE_SYMBOLIZE_H
#define LLVM_DEBUGINFO_SYMBOLIZE_SYMBOLIZE_H

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"
#include <string>

namespace llvm {
namespace symbolize {

struct DILineInfo {
    std::string FileName = "<invalid>";
    std::string FunctionName = "??";
    uint32_t Line = 0;
};

class LLVMSymbolizer {
public:
    struct Options {
        bool Demangle = false;
    };

    explicit LLVMSymbolizer(const Options &) {}

    Expected<DILineInfo> symbolizeCode(const std::string &,
                                       object::SectionedAddress) {
        return DILineInfo();
    }
};

} // namespace symbolize
} // namespace llvm

#endif
