#ifndef LLVM_OBJECT_OBJECTFILE_H
#define LLVM_OBJECT_OBJECTFILE_H

#include <liric/llvm_compat_c.h>
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
namespace object {

class Binary {
public:
    enum class BinaryKind : uint8_t {
        Object
    };

    virtual ~Binary() = default;
    virtual BinaryKind getKind() const { return BinaryKind::Object; }

    static bool classof(const Binary *B) { return B != nullptr; }
};

struct SectionedAddress {
    uint64_t Address = 0;
    uint64_t SectionIndex = 0;
};

class SectionRef {
    uint64_t Address_ = 0;
    uint64_t Size_ = 0;
    uint64_t Index_ = 0;

public:
    SectionRef() = default;
    SectionRef(uint64_t Address, uint64_t Size, uint64_t Index)
        : Address_(Address), Size_(Size), Index_(Index) {}

    uint64_t getAddress() const { return Address_; }
    uint64_t getSize() const { return Size_; }
    uint64_t getIndex() const { return Index_; }
};

template <typename T>
class OwningBinary {
    std::unique_ptr<T> Binary_;

public:
    explicit OwningBinary(std::unique_ptr<T> Binary)
        : Binary_(std::move(Binary)) {}

    T *getBinary() { return Binary_.get(); }
    const T *getBinary() const { return Binary_.get(); }
    std::unique_ptr<T> takeBinary() { return std::move(Binary_); }
};

class ObjectFile : public Binary {
    lr_llvm_compat_object_t *Handle_ = nullptr;
    mutable std::vector<SectionRef> Sections_;

    void load_sections_if_needed() const {
        if (!Sections_.empty())
            return;
        if (!Handle_) {
            Sections_.emplace_back(0, std::numeric_limits<uint64_t>::max(), 0);
            return;
        }
        size_t n = lr_llvm_compat_object_section_count(Handle_);
        for (size_t i = 0; i < n; i++) {
            uint64_t addr = 0;
            uint64_t size = 0;
            uint64_t idx = 0;
            if (lr_llvm_compat_object_section_get(Handle_, i, &addr, &size, &idx) != 0)
                continue;
            Sections_.emplace_back(addr, size, idx);
        }
        if (Sections_.empty())
            Sections_.emplace_back(0, std::numeric_limits<uint64_t>::max(), 0);
    }

public:
    explicit ObjectFile(lr_llvm_compat_object_t *Handle)
        : Handle_(Handle) {}

    ~ObjectFile() override {
        if (Handle_) {
            lr_llvm_compat_object_destroy(Handle_);
            Handle_ = nullptr;
        }
    }

    ObjectFile(const ObjectFile &) = delete;
    ObjectFile &operator=(const ObjectFile &) = delete;

    ObjectFile(ObjectFile &&Other) noexcept
        : Handle_(Other.Handle_),
          Sections_(std::move(Other.Sections_)) {
        Other.Handle_ = nullptr;
    }

    ObjectFile &operator=(ObjectFile &&Other) noexcept {
        if (this == &Other)
            return *this;
        if (Handle_)
            lr_llvm_compat_object_destroy(Handle_);
        Handle_ = Other.Handle_;
        Sections_ = std::move(Other.Sections_);
        Other.Handle_ = nullptr;
        return *this;
    }

    static Expected<OwningBinary<ObjectFile>> createObjectFile(const std::string &Path) {
        lr_llvm_compat_object_t *Obj = nullptr;
        if (lr_llvm_compat_object_create(Path.c_str(), &Obj) != 0)
            return make_error("liric object creation failed");
        return OwningBinary<ObjectFile>(std::make_unique<ObjectFile>(Obj));
    }

    static bool classof(const Binary *B) {
        return B != nullptr && B->getKind() == BinaryKind::Object;
    }

    const lr_llvm_compat_object_t *raw_handle() const { return Handle_; }

    const std::vector<SectionRef> &sections() const {
        load_sections_if_needed();
        return Sections_;
    }
};

} // namespace object
} // namespace llvm

#endif
