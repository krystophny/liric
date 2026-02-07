#ifndef LLVM_SUPPORT_CASTING_H
#define LLVM_SUPPORT_CASTING_H

#include <type_traits>

namespace llvm {

template <typename To, typename From>
inline bool isa(const From *Val) {
    (void)Val;
    return Val != nullptr;
}

template <typename To, typename From>
inline To *cast(From *Val) {
    return static_cast<To *>(Val);
}

template <typename To, typename From>
inline const To *cast(const From *Val) {
    return static_cast<const To *>(Val);
}

template <typename To, typename From>
inline To *dyn_cast(From *Val) {
    if (!Val) return nullptr;
    return static_cast<To *>(Val);
}

template <typename To, typename From>
inline const To *dyn_cast(const From *Val) {
    if (!Val) return nullptr;
    return static_cast<const To *>(Val);
}

template <typename To, typename From>
inline To *dyn_cast_or_null(From *Val) {
    if (!Val) return nullptr;
    return dyn_cast<To>(Val);
}

template <typename To, typename From>
inline To *cast_or_null(From *Val) {
    if (!Val) return nullptr;
    return cast<To>(Val);
}

} // namespace llvm

#endif
