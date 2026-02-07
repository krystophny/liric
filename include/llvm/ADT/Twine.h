#ifndef LLVM_ADT_TWINE_H
#define LLVM_ADT_TWINE_H

#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm {

class Twine {
    std::string storage_;

public:
    Twine() = default;
    Twine(const char *s) : storage_(s ? s : "") {}
    Twine(const std::string &s) : storage_(s) {}
    Twine(StringRef s) : storage_(s.data(), s.size()) {}

    std::string str() const { return storage_; }
    const char *c_str() const { return storage_.c_str(); }
    StringRef getSingleStringRef() const { return storage_; }

    Twine operator+(const Twine &other) const {
        return Twine(storage_ + other.storage_);
    }
};

} // namespace llvm

#endif
