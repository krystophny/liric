#ifndef LLVM_IR_DATALAYOUT_H
#define LLVM_IR_DATALAYOUT_H

#include "llvm/IR/DerivedTypes.h"
#include "llvm/ADT/StringRef.h"
#include <llvm-c/LiricCompat.h>
#include <cstdint>

namespace llvm {

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
public:
    DataLayout() = default;
    explicit DataLayout(StringRef Desc) { (void)Desc; }

    bool isDefault() const { return true; }

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
        (void)Ty;
        return 8;
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
        uint64_t offset = 0;
        for (unsigned i = 0; i < n; i++) {
            offsets[i] = offset;
            offset += getTypeAllocSize(Ty->getElementType(i));
        }
        delete cached;
        cached = new StructLayout(offset, std::move(offsets));
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

    bool operator==(const DataLayout &) const { return true; }
    bool operator!=(const DataLayout &) const { return false; }
};

} // namespace llvm

#endif
