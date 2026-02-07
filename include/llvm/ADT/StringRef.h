#ifndef LLVM_ADT_STRINGREF_H
#define LLVM_ADT_STRINGREF_H

#include <cstring>
#include <string>
#include <string_view>

namespace llvm {

class StringRef {
    const char *data_;
    size_t len_;

public:
    StringRef() : data_(""), len_(0) {}
    StringRef(const char *s) : data_(s), len_(s ? std::strlen(s) : 0) {}
    StringRef(const char *s, size_t n) : data_(s), len_(n) {}
    StringRef(const std::string &s) : data_(s.data()), len_(s.size()) {}
    StringRef(std::string_view sv) : data_(sv.data()), len_(sv.size()) {}

    const char *data() const { return data_; }
    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }

    std::string str() const { return std::string(data_, len_); }
    operator std::string() const { return str(); }

    bool equals(StringRef other) const {
        return len_ == other.len_ && std::memcmp(data_, other.data_, len_) == 0;
    }
    bool operator==(StringRef other) const { return equals(other); }
    bool operator!=(StringRef other) const { return !equals(other); }
};

} // namespace llvm

#endif
