#ifndef LLVM_SUPPORT_SOURCEMGR_H
#define LLVM_SUPPORT_SOURCEMGR_H

#include <string>

namespace llvm {

class SMDiagnostic {
    std::string msg_;
public:
    SMDiagnostic() = default;
    SMDiagnostic(const std::string &msg) : msg_(msg) {}
    const std::string &getMessage() const { return msg_; }
};

} // namespace llvm

#endif
