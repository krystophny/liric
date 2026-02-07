#ifndef LLVM_SUPPORT_ERRORHANDLING_H
#define LLVM_SUPPORT_ERRORHANDLING_H

#include <cstdlib>
#include <cstdio>

namespace llvm {

[[noreturn]] inline void llvm_unreachable_internal(
    const char *msg, const char *file, unsigned line) {
    fprintf(stderr, "LLVM unreachable: %s at %s:%u\n", msg, file, line);
    abort();
}

inline void report_fatal_error(const char *reason, bool gen_crash_diag = true) {
    (void)gen_crash_diag;
    fprintf(stderr, "LLVM fatal error: %s\n", reason);
    abort();
}

} // namespace llvm

#define llvm_unreachable(msg) \
    ::llvm::llvm_unreachable_internal(msg, __FILE__, __LINE__)

#endif
