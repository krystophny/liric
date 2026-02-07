#ifndef LLVM_SUPPORT_PATH_H
#define LLVM_SUPPORT_PATH_H

#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm {
namespace sys {
namespace path {

inline StringRef filename(StringRef path) {
    std::string s = path.str();
    auto pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return StringRef(path.data() + pos + 1, path.size() - pos - 1);
}

inline StringRef parent_path(StringRef path) {
    std::string s = path.str();
    auto pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return StringRef(path.data(), pos);
}

} // namespace path
} // namespace sys
} // namespace llvm

#endif
