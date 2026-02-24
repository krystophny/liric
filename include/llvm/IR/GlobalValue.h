#ifndef LLVM_IR_GLOBALVALUE_H
#define LLVM_IR_GLOBALVALUE_H

#include "llvm/IR/Constants.h"
#include <liric/llvm_compat_c.h>

namespace llvm {

class Module;
class PointerType;

namespace detail {
    inline void unregister_global_value_state(const void *obj) {
        lr_llvm_compat_unregister_global_value_state(obj);
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
        lr_llvm_compat_global_value_set_linkage(this, (int)lt);
    }
    LinkageTypes getLinkage() const {
        int linkage = 0;
        if (!lr_llvm_compat_global_value_get_linkage(this, &linkage))
            return ExternalLinkage;
        return (LinkageTypes)linkage;
    }

    void setVisibility(VisibilityTypes vt) {
        lr_llvm_compat_global_value_set_visibility(this, (int)vt);
    }
    VisibilityTypes getVisibility() const {
        int visibility = 0;
        if (!lr_llvm_compat_global_value_get_visibility(this, &visibility))
            return DefaultVisibility;
        return (VisibilityTypes)visibility;
    }

    void setUnnamedAddr(UnnamedAddr ua) {
        lr_llvm_compat_global_value_set_unnamed_addr(this, (int)ua);
    }
    UnnamedAddr getUnnamedAddr() const {
        int unnamed_addr = 0;
        if (!lr_llvm_compat_global_value_get_unnamed_addr(this, &unnamed_addr))
            return None;
        return (UnnamedAddr)unnamed_addr;
    }

    bool isDeclaration() const { return false; }
    bool hasExternalLinkage() const {
        return getLinkage() == ExternalLinkage;
    }

    void eraseFromParent() {}
};

} // namespace llvm

#endif
