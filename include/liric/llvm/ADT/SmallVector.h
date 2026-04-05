#ifndef LLVM_ADT_SMALLVECTOR_H
#define LLVM_ADT_SMALLVECTOR_H

#include <vector>

namespace liric_llvm {

template <typename T, unsigned N = 0>
using SmallVector = std::vector<T>;

} // namespace liric_llvm

#endif
