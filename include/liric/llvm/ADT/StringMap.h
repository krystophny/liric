#ifndef LLVM_ADT_STRINGMAP_H
#define LLVM_ADT_STRINGMAP_H

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace liric_llvm {
class MallocAllocator {};

template<class ValueTy, class AllocatorTy = MallocAllocator>
class StringMap {
public:
    class Entry {
        std::string key_;
        ValueTy value_{};
    public:
        explicit Entry(StringRef key): key_(key.str()) {}
        StringRef getKey() const { return key_; }
        ValueTy &getValue() { return value_; }
        const ValueTy &getValue() const { return value_; }
    };
    using iterator = typename std::vector<Entry>::iterator;
    using const_iterator = typename std::vector<Entry>::const_iterator;
    ValueTy &operator[](StringRef key) {
        for (auto &entry: entries_) {
            if (entry.getKey().str() == key.str()) return entry.getValue();
        }
        entries_.emplace_back(key);
        return entries_.back().getValue();
    }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
private:
    std::vector<Entry> entries_;
};
} // namespace liric_llvm
#endif
