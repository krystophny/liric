#ifndef LLVM_DEBUGINFO_SYMBOLIZE_SYMBOLIZE_H
#define LLVM_DEBUGINFO_SYMBOLIZE_SYMBOLIZE_H

#include <liric/llvm_compat_c.h>
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Error.h"
#include <string>

namespace llvm {

struct DILineInfoSpecifier {
    enum class FileLineInfoKind {
        AbsoluteFilePath
    };
};

namespace symbolize {

struct DILineInfo {
    std::string FileName = "<invalid>";
    std::string FunctionName = "??";
    uint32_t Line = 0;
};

class LLVMSymbolizer {
    bool Demangle_ = false;

public:
    struct Options {
        bool Demangle = false;
    };

    explicit LLVMSymbolizer(const Options &Opts)
        : Demangle_(Opts.Demangle) {}

    Expected<DILineInfo> symbolizeCode(const std::string &BinaryPath,
                                       object::SectionedAddress SA) {
        DILineInfo Info;
        char file_buf[512];
        char func_buf[512];
        uint32_t line = 0;
        int rc = lr_llvm_compat_symbolize_code(
            BinaryPath.c_str(), SA.Address, SA.SectionIndex,
            Demangle_ ? 1 : 0,
            file_buf, sizeof(file_buf),
            func_buf, sizeof(func_buf), &line);
        if (rc != 0)
            return make_error("liric symbolizeCode failed");
        Info.FileName = file_buf;
        Info.FunctionName = func_buf;
        Info.Line = line;
        return Info;
    }
};

} // namespace symbolize
} // namespace llvm

#endif
