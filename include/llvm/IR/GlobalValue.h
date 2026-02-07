#ifndef LLVM_IR_GLOBALVALUE_H
#define LLVM_IR_GLOBALVALUE_H

#include "llvm/IR/Constants.h"

namespace llvm {

class Module;
class PointerType;

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

    void setLinkage(LinkageTypes lt) { (void)lt; }
    LinkageTypes getLinkage() const { return ExternalLinkage; }

    void setVisibility(VisibilityTypes vt) { (void)vt; }
    VisibilityTypes getVisibility() const { return DefaultVisibility; }

    void setUnnamedAddr(UnnamedAddr ua) { (void)ua; }
    UnnamedAddr getUnnamedAddr() const { return None; }

    bool isDeclaration() const { return false; }
    bool hasExternalLinkage() const { return true; }

    void eraseFromParent() {}
};

} // namespace llvm

#endif
