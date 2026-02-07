#ifndef LLVM_SUPPORT_SOURCEMGR_H
#define LLVM_SUPPORT_SOURCEMGR_H

#include "llvm/Support/raw_ostream.h"
#include <string>

namespace llvm {

class SMDiagnostic {
    std::string msg_;
public:
    SMDiagnostic() = default;
    SMDiagnostic(const std::string &msg) : msg_(msg) {}
    const std::string &getMessage() const { return msg_; }

    void print(const char *ProgName, raw_ostream &S, bool ShowColors = true) const {
        (void)ShowColors;
        if (ProgName) S << ProgName << ": ";
        S << msg_ << "\n";
    }
};

} // namespace llvm

#endif
