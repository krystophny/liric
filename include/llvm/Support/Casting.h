#ifndef LLVM_SUPPORT_CASTING_H
#define LLVM_SUPPORT_CASTING_H

#include <cassert>
#include <type_traits>

namespace llvm {

namespace detail {

template <typename To, typename From, typename = void>
struct has_classof : std::false_type {};

template <typename To, typename From>
struct has_classof<To, From,
    std::void_t<decltype(To::classof(std::declval<const From *>()))>>
    : std::true_type {};

template <typename To, typename From>
bool isa_impl(const From *Val, std::true_type) {
    return To::classof(Val);
}

template <typename To, typename From>
bool isa_impl(const From *, std::false_type) {
    return std::is_base_of_v<To, From> || std::is_base_of_v<From, To>
        || std::is_same_v<To, From>;
}

} // namespace detail

template <typename To, typename From>
inline bool isa(const From *Val) {
    if (!Val) return false;
    return detail::isa_impl<To>(Val, detail::has_classof<To, From>{});
}

template <typename To, typename From>
inline To *cast(From *Val) {
    assert(Val && "cast called on nullptr");
    assert(isa<To>(Val) && "cast<To>() argument of incompatible type");
    return static_cast<To *>(Val);
}

template <typename To, typename From>
inline const To *cast(const From *Val) {
    assert(Val && "cast called on nullptr");
    assert(isa<To>(Val) && "cast<To>() argument of incompatible type");
    return static_cast<const To *>(Val);
}

template <typename To, typename From>
inline To *dyn_cast(From *Val) {
    if (!Val) return nullptr;
    if (!isa<To>(Val)) return nullptr;
    return static_cast<To *>(Val);
}

template <typename To, typename From>
inline const To *dyn_cast(const From *Val) {
    if (!Val) return nullptr;
    if (!isa<To>(Val)) return nullptr;
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
