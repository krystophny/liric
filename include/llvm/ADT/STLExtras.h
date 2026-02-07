#ifndef LLVM_ADT_STLEXTRAS_H
#define LLVM_ADT_STLEXTRAS_H

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <utility>

namespace llvm {

template <typename T>
auto drop_begin(T &&Range, size_t N = 1) {
    auto it = std::begin(Range);
    std::advance(it, N);
    return it;
}

} // namespace llvm

#endif
