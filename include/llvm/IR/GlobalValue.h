#ifndef LLVM_IR_GLOBALVALUE_H
#define LLVM_IR_GLOBALVALUE_H

#include "llvm/IR/Constants.h"
#include <unordered_map>

namespace llvm {

class Module;
class PointerType;

namespace detail {
    struct global_value_state {
        int linkage = 0;
        int visibility = 0;
        int unnamed_addr = 0;
    };

    inline thread_local std::unordered_map<const void *, global_value_state>
        global_value_states;

    inline global_value_state &ensure_global_value_state(const void *obj) {
        if (!obj) {
            static thread_local global_value_state fallback;
            return fallback;
        }
        return global_value_states[obj];
    }

    inline const global_value_state *lookup_global_value_state(const void *obj) {
        auto it = global_value_states.find(obj);
        if (it == global_value_states.end())
            return nullptr;
        return &it->second;
    }

    inline void unregister_global_value_state(const void *obj) {
        if (obj)
            global_value_states.erase(obj);
    }
} // namespace detail

class GlobalValue : public Constant {
public:
    enum LinkageTypes {
        ExternalLinkage = 0,
        AvailableExternallyLinkage,
        LinkOnceAnyLinkage,
        LinkOnceODRLinkage,
        WeakAnyLinkage,
        WeakODRLinkage,
        AppendingLinkage,
        InternalLinkage,
        PrivateLinkage,
        ExternalWeakLinkage,
        CommonLinkage,
    };

    enum VisibilityTypes {
        DefaultVisibility = 0,
        HiddenVisibility,
        ProtectedVisibility,
    };

    enum UnnamedAddr {
        None = 0,
        Local,
        Global,
    };

    Type *getValueType() const { return getType(); }

    void setLinkage(LinkageTypes lt) {
        detail::ensure_global_value_state(this).linkage = (int)lt;
    }
    LinkageTypes getLinkage() const {
        const detail::global_value_state *state =
            detail::lookup_global_value_state(this);
        if (!state)
            return ExternalLinkage;
        return (LinkageTypes)state->linkage;
    }

    void setVisibility(VisibilityTypes vt) {
        detail::ensure_global_value_state(this).visibility = (int)vt;
    }
    VisibilityTypes getVisibility() const {
        const detail::global_value_state *state =
            detail::lookup_global_value_state(this);
        if (!state)
            return DefaultVisibility;
        return (VisibilityTypes)state->visibility;
    }

    void setUnnamedAddr(UnnamedAddr ua) {
        detail::ensure_global_value_state(this).unnamed_addr = (int)ua;
    }
    UnnamedAddr getUnnamedAddr() const {
        const detail::global_value_state *state =
            detail::lookup_global_value_state(this);
        if (!state)
            return None;
        return (UnnamedAddr)state->unnamed_addr;
    }

    bool isDeclaration() const { return false; }
    bool hasExternalLinkage() const {
        return getLinkage() == ExternalLinkage;
    }

    void eraseFromParent() {}
};

} // namespace llvm

#endif
