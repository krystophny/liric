#ifndef LLVM_SUPPORT_MANAGEDSTATIC_H
#define LLVM_SUPPORT_MANAGEDSTATIC_H

namespace llvm {

inline void llvm_shutdown() {}

template <class C>
class ManagedStatic {
    mutable C *Ptr = nullptr;

public:
    C &operator*() {
        if (!Ptr) Ptr = new C();
        return *Ptr;
    }

    C *operator->() {
        if (!Ptr) Ptr = new C();
        return Ptr;
    }

    ~ManagedStatic() { delete Ptr; }
};

} // namespace llvm

#endif
