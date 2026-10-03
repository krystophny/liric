#ifndef LLVM_IR_DATALAYOUT_H
#define LLVM_IR_DATALAYOUT_H

#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/StringRef.h"
#include <liric/liric_compat.h>
#include <cstdint>
#include <string>
#include <stdexcept>

namespace liric_llvm {

class StructLayout {
    size_t size_;
    std::vector<uint64_t> offsets_;

public:
    StructLayout(size_t s, std::vector<uint64_t> o)
        : size_(s), offsets_(std::move(o)) {}

    uint64_t getSizeInBytes() const { return size_; }
    uint64_t getElementOffset(unsigned Idx) const {
        return Idx < offsets_.size() ? offsets_[Idx] : 0;
    }
};

class DataLayout {
    std::string description_;
public:
    DataLayout() = default;
    explicit DataLayout(StringRef description): description_(description.str()) {
        size_t begin = 0;
        while (begin < description_.size()) {
            size_t end = description_.find('-', begin);
            std::string token = description_.substr(begin, end - begin);
            bool supported = token == "e" || token == "p:64:64"
                || token == "p0:64:64" || token == "p:64:64:64"
                || token == "p0:64:64:64" || token == "i1:8" || token == "i8:8"
                || token == "i16:16" || token == "i32:32" || token == "i64:64"
                || token == "i128:128" || token == "f16:16" || token == "f32:32"
                || token == "f64:64" || token == "f80:128" || token == "f128:128"
                || token == "n8:16:32:64" || token == "n32:64" || token == "S128";
            if (!supported) throw std::runtime_error(
                "liric: requested data layout is not supported by this native backend");
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }
    const std::string &getStringRepresentation() const { return description_; }
    bool isDefault() const { return description_.empty(); }

    unsigned getPointerSize() const { return 8; }
    unsigned getPointerSizeInBits() const { return 64; }

    uint64_t getTypeAllocSize(Type *Ty) const {
        return lc_type_alloc_size(Ty->impl());
    }

    uint64_t getTypeStoreSize(Type *Ty) const {
        return lc_type_store_size(Ty->impl());
    }

    uint64_t getTypeSizeInBits(Type *Ty) const {
        return lc_type_size_bits(Ty->impl());
    }

    unsigned getABITypeAlign(Type *Ty) const {
        return lc_type_abi_align(Ty->impl());
    }

    unsigned getPrefTypeAlign(Type *Ty) const {
        return getABITypeAlign(Ty);
    }

    const StructLayout *getStructLayout(StructType *Ty) const {
        static thread_local StructLayout *cached = nullptr;
        static thread_local StructType *cached_ty = nullptr;
        if (cached_ty == Ty && cached) return cached;

        unsigned n = Ty->getNumElements();
        std::vector<uint64_t> offsets(n);
        for (unsigned i = 0; i < n; i++) {
            offsets[i] = lc_type_struct_offset(Ty->impl(), i);
        }
        delete cached;
        cached = new StructLayout(getTypeAllocSize(Ty), std::move(offsets));
        cached_ty = Ty;
        return cached;
    }

    char getGlobalPrefix() const {
#if defined(__APPLE__)
        return '_';
#else
        return '\0';
#endif
    }

    bool operator==(const DataLayout &other) const { return description_ == other.description_; }
    bool operator!=(const DataLayout &other) const { return !(*this == other); }
};

} // namespace liric_llvm

#endif
