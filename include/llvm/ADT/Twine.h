#ifndef LLVM_ADT_TWINE_H
#define LLVM_ADT_TWINE_H

#include "llvm/ADT/StringRef.h"
#include <liric/liric_compat.h>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#define LIRIC_LLVM_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIRIC_LLVM_HIDDEN
#endif

namespace llvm {

class formatv_object_base;

class LIRIC_LLVM_HIDDEN Twine {
    std::string storage_;
    bool is_null_ = false;

    explicit Twine(bool is_null) : is_null_(is_null) {}

    static std::string from_i64(int64_t v) {
        char buf[32];
        size_t n = lc_format_i64(buf, sizeof(buf), v);
        return n ? std::string(buf, n) : std::string();
    }

    static std::string from_u64(uint64_t v) {
        char buf[32];
        size_t n = lc_format_u64(buf, sizeof(buf), v);
        return n ? std::string(buf, n) : std::string();
    }

public:
    Twine() = default;
    Twine(const Twine &) = default;
    Twine(Twine &&) = default;

    Twine(const char *Str) {
        if (Str)
            storage_ = Str;
    }
    Twine(std::nullptr_t) = delete;

    Twine(const std::string &Str) : storage_(Str) {}

    Twine(const std::string_view &Str) : storage_(Str) {}

    Twine(const StringRef &Str) : storage_(Str.str()) {}

    explicit Twine(char Val) : storage_(1, Val) {}
    explicit Twine(signed char Val) : storage_(1, static_cast<char>(Val)) {}
    explicit Twine(unsigned char Val) : storage_(1, static_cast<char>(Val)) {}

    explicit Twine(unsigned Val) : storage_(from_u64(Val)) {}
    explicit Twine(int Val) : storage_(from_i64(Val)) {}
    explicit Twine(const unsigned long &Val) : storage_(from_u64(Val)) {}
    explicit Twine(const long &Val) : storage_(from_i64(Val)) {}
    explicit Twine(const unsigned long long &Val) : storage_(from_u64(Val)) {}
    explicit Twine(const long long &Val) : storage_(from_i64(Val)) {}

    Twine(const char *lhs, const StringRef &rhs)
        : storage_(lhs ? lhs : "") {
        storage_ += rhs.str();
    }

    Twine(const StringRef &lhs, const char *rhs)
        : storage_(lhs.str()) {
        if (rhs)
            storage_ += rhs;
    }

    Twine &operator=(const Twine &) = delete;

    static Twine createNull() { return Twine(true); }

    bool isTriviallyEmpty() const { return is_null_ || storage_.empty(); }

    bool isSingleStringRef() const { return !is_null_; }

    StringRef getSingleStringRef() const {
        assert(isSingleStringRef() && "Twine is not representable as a single StringRef");
        return StringRef(storage_);
    }

    Twine concat(const Twine &Suffix) const {
        if (is_null_ || Suffix.is_null_)
            return createNull();
        Twine result;
        result.storage_ = storage_;
        result.storage_ += Suffix.storage_;
        return result;
    }

    std::string str() const {
        if (is_null_)
            return std::string();
        return storage_;
    }

    const char *c_str() const {
        thread_local std::string thread_storage;
        thread_storage = str();
        return thread_storage.c_str();
    }
};

inline Twine operator+(const Twine &LHS, const Twine &RHS) {
    return LHS.concat(RHS);
}

inline Twine operator+(const char *LHS, const StringRef &RHS) {
    return Twine(LHS, RHS);
}

inline Twine operator+(const StringRef &LHS, const char *RHS) {
    return Twine(LHS, RHS);
}

} // namespace llvm

#undef LIRIC_LLVM_HIDDEN

#endif
