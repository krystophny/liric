#ifndef LLVM_ADT_STRINGREF_H
#define LLVM_ADT_STRINGREF_H

#include <cstring>
#include <string>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#define LIRIC_LLVM_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIRIC_LLVM_HIDDEN
#endif

namespace llvm {

class LIRIC_LLVM_HIDDEN StringRef {
    const char *data_;
    size_t len_;

public:
    StringRef() : data_(""), len_(0) {}
    StringRef(const char *s) : data_(s ? s : ""), len_(s ? std::strlen(s) : 0) {}
    StringRef(const char *s, size_t n) : data_(s ? s : ""), len_(s ? n : 0) {}
    StringRef(const std::string &s) : data_(s.data()), len_(s.size()) {}
    StringRef(std::string_view sv)
        : data_(sv.data() ? sv.data() : ""), len_(sv.data() ? sv.size() : 0) {}

    const char *data() const { return data_; }
    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }

    std::string str() const {
        if (len_ == 0) return std::string();
        return std::string(data_, len_);
    }
    operator std::string() const { return str(); }

    bool equals(StringRef other) const {
        if (len_ != other.len_) return false;
        if (len_ == 0) return true;
        return std::memcmp(data_, other.data_, len_) == 0;
    }
    bool operator==(StringRef other) const { return equals(other); }
    bool operator!=(StringRef other) const { return !equals(other); }
};

} // namespace llvm

#undef LIRIC_LLVM_HIDDEN

#endif
