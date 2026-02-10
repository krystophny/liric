#ifndef LLVM_ADT_TWINE_H
#define LLVM_ADT_TWINE_H

#include "llvm/ADT/StringRef.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
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
    enum NodeKind : unsigned char {
        NullKind,
        EmptyKind,
        TwineKind,
        CStringKind,
        StdStringKind,
        PtrAndLengthKind,
        StringLiteralKind,
        FormatvObjectKind,
        CharKind,
        DecUIKind,
        DecIKind,
        DecULKind,
        DecLKind,
        DecULLKind,
        DecLLKind,
        UHexKind
    };

    union Child {
        const Twine *twine;
        const char *cString;
        const std::string *stdString;
        struct {
            const char *ptr;
            size_t length;
        } ptrAndLength;
        const formatv_object_base *formatvObject;
        char character;
        unsigned int decUI;
        int decI;
        const unsigned long *decUL;
        const long *decL;
        const unsigned long long *decULL;
        const long long *decLL;
        const uint64_t *uHex;
    };

    Child LHS;
    Child RHS;
    NodeKind LHSKind = EmptyKind;
    NodeKind RHSKind = EmptyKind;

    explicit Twine(NodeKind Kind) : LHSKind(Kind) {}

    explicit Twine(Child lhs, NodeKind lhs_kind, Child rhs, NodeKind rhs_kind)
        : LHS(lhs), RHS(rhs), LHSKind(lhs_kind), RHSKind(rhs_kind) {}

    bool isNull() const { return LHSKind == NullKind; }
    bool isEmpty() const { return LHSKind == EmptyKind; }
    bool isNullary() const { return isNull() || isEmpty(); }
    bool isUnary() const { return RHSKind == EmptyKind && !isNullary(); }

    void appendChild(std::string &out, Child node, NodeKind kind) const {
        switch (kind) {
        case NullKind:
        case EmptyKind:
            return;
        case TwineKind:
            if (node.twine != nullptr) {
                node.twine->appendTo(out);
            }
            return;
        case CStringKind:
            if (node.cString != nullptr) {
                out += node.cString;
            }
            return;
        case StdStringKind:
            if (node.stdString != nullptr) {
                out += *node.stdString;
            }
            return;
        case PtrAndLengthKind:
        case StringLiteralKind:
            if (node.ptrAndLength.ptr != nullptr && node.ptrAndLength.length != 0) {
                out.append(node.ptrAndLength.ptr, node.ptrAndLength.length);
            }
            return;
        case FormatvObjectKind:
            out += "<formatv>";
            return;
        case CharKind:
            out.push_back(node.character);
            return;
        case DecUIKind:
            out += std::to_string(node.decUI);
            return;
        case DecIKind:
            out += std::to_string(node.decI);
            return;
        case DecULKind:
            if (node.decUL != nullptr) {
                out += std::to_string(*node.decUL);
            }
            return;
        case DecLKind:
            if (node.decL != nullptr) {
                out += std::to_string(*node.decL);
            }
            return;
        case DecULLKind:
            if (node.decULL != nullptr) {
                out += std::to_string(*node.decULL);
            }
            return;
        case DecLLKind:
            if (node.decLL != nullptr) {
                out += std::to_string(*node.decLL);
            }
            return;
        case UHexKind:
            if (node.uHex != nullptr) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%llx",
                              static_cast<unsigned long long>(*node.uHex));
                out += buf;
            }
            return;
        }
    }

    void appendTo(std::string &out) const {
        if (isNull()) {
            return;
        }
        appendChild(out, LHS, LHSKind);
        appendChild(out, RHS, RHSKind);
    }

public:
    Twine() = default;
    Twine(const Twine &) = default;

    Twine(const char *Str) {
        if (Str != nullptr && Str[0] != '\0') {
            LHS.cString = Str;
            LHSKind = CStringKind;
        } else {
            LHSKind = EmptyKind;
        }
    }
    Twine(std::nullptr_t) = delete;

    Twine(const std::string &Str) : LHSKind(StdStringKind) {
        LHS.stdString = &Str;
    }

    Twine(const std::string_view &Str) : LHSKind(PtrAndLengthKind) {
        LHS.ptrAndLength.ptr = Str.data();
        LHS.ptrAndLength.length = Str.length();
    }

    Twine(const StringRef &Str) : LHSKind(PtrAndLengthKind) {
        LHS.ptrAndLength.ptr = Str.data();
        LHS.ptrAndLength.length = Str.size();
    }

    explicit Twine(char Val) : LHSKind(CharKind) { LHS.character = Val; }
    explicit Twine(signed char Val) : LHSKind(CharKind) {
        LHS.character = static_cast<char>(Val);
    }
    explicit Twine(unsigned char Val) : LHSKind(CharKind) {
        LHS.character = static_cast<char>(Val);
    }

    explicit Twine(unsigned Val) : LHSKind(DecUIKind) { LHS.decUI = Val; }
    explicit Twine(int Val) : LHSKind(DecIKind) { LHS.decI = Val; }
    explicit Twine(const unsigned long &Val) : LHSKind(DecULKind) {
        LHS.decUL = &Val;
    }
    explicit Twine(const long &Val) : LHSKind(DecLKind) { LHS.decL = &Val; }
    explicit Twine(const unsigned long long &Val) : LHSKind(DecULLKind) {
        LHS.decULL = &Val;
    }
    explicit Twine(const long long &Val) : LHSKind(DecLLKind) {
        LHS.decLL = &Val;
    }

    Twine(const char *lhs, const StringRef &rhs)
        : LHSKind(CStringKind), RHSKind(PtrAndLengthKind) {
        LHS.cString = lhs;
        RHS.ptrAndLength.ptr = rhs.data();
        RHS.ptrAndLength.length = rhs.size();
    }

    Twine(const StringRef &lhs, const char *rhs)
        : LHSKind(PtrAndLengthKind), RHSKind(CStringKind) {
        LHS.ptrAndLength.ptr = lhs.data();
        LHS.ptrAndLength.length = lhs.size();
        RHS.cString = rhs;
    }

    Twine &operator=(const Twine &) = delete;

    static Twine createNull() { return Twine(NullKind); }

    bool isTriviallyEmpty() const { return isNullary(); }

    bool isSingleStringRef() const {
        if (RHSKind != EmptyKind) {
            return false;
        }
        return LHSKind == EmptyKind || LHSKind == CStringKind ||
               LHSKind == StdStringKind || LHSKind == PtrAndLengthKind ||
               LHSKind == StringLiteralKind;
    }

    StringRef getSingleStringRef() const {
        assert(isSingleStringRef() && "Twine is not representable as a single StringRef");
        switch (LHSKind) {
        case EmptyKind:
            return StringRef();
        case CStringKind:
            return StringRef(LHS.cString);
        case StdStringKind:
            return StringRef(*LHS.stdString);
        case PtrAndLengthKind:
        case StringLiteralKind:
            return StringRef(LHS.ptrAndLength.ptr, LHS.ptrAndLength.length);
        default:
            return StringRef();
        }
    }

    Twine concat(const Twine &Suffix) const {
        if (isNull() || Suffix.isNull()) {
            return Twine(NullKind);
        }
        if (isEmpty()) {
            return Suffix;
        }
        if (Suffix.isEmpty()) {
            return *this;
        }

        Child new_lhs;
        Child new_rhs;
        NodeKind new_lhs_kind = TwineKind;
        NodeKind new_rhs_kind = TwineKind;
        new_lhs.twine = this;
        new_rhs.twine = &Suffix;
        if (isUnary()) {
            new_lhs = LHS;
            new_lhs_kind = LHSKind;
        }
        if (Suffix.isUnary()) {
            new_rhs = Suffix.LHS;
            new_rhs_kind = Suffix.LHSKind;
        }
        return Twine(new_lhs, new_lhs_kind, new_rhs, new_rhs_kind);
    }

    std::string str() const {
        std::string out;
        appendTo(out);
        return out;
    }

    const char *c_str() const {
        thread_local std::string storage;
        storage = str();
        return storage.c_str();
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
