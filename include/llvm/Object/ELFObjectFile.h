#ifndef LLVM_OBJECT_ELFOBJECTFILE_H
#define LLVM_OBJECT_ELFOBJECTFILE_H

#include "llvm/Support/Error.h"
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
namespace object {

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
    T Binary_;

public:
    explicit OwningBinary(T Binary) : Binary_(std::move(Binary)) {}
    T *getBinary() { return &Binary_; }
    const T *getBinary() const { return &Binary_; }
};

class ObjectFile {
    std::vector<SectionRef> Sections_;

public:
    ObjectFile() {
        Sections_.emplace_back(0, std::numeric_limits<uint64_t>::max(), 0);
    }

    static Expected<OwningBinary<ObjectFile>> createObjectFile(const std::string &) {
        return OwningBinary<ObjectFile>(ObjectFile());
    }

    const std::vector<SectionRef> &sections() const { return Sections_; }
};

} // namespace object
} // namespace llvm

#endif
