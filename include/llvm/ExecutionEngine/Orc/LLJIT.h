#ifndef LLVM_EXECUTIONENGINE_ORC_LLJIT_H
#define LLVM_EXECUTIONENGINE_ORC_LLJIT_H

#include <liric/liric_compat.h>
#include "llvm/ADT/StringRef.h"
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

class Module;

namespace orc {

class LLJIT {
    LLVMLiricSessionStateRef session_;

public:
    LLJIT() : session_(LLVMLiricSessionCreate()) {}
    ~LLJIT() { if (session_) LLVMLiricSessionDispose(session_); }

    LLJIT(const LLJIT &) = delete;
    LLJIT &operator=(const LLJIT &) = delete;

    LLVMLiricSessionStateRef getLiricSession() const { return session_; }

    inline int addModule(Module &M);

    void *lookup(StringRef Name) {
        std::string symbol_name = Name.str();
        return LLVMLiricSessionLookup(session_, symbol_name.c_str());
    }

    void addSymbol(StringRef Name, void *Addr) {
        std::string symbol_name = Name.str();
        LLVMLiricSessionAddSymbol(session_, symbol_name.c_str(), Addr);
    }

    static const char *getHostTargetName() {
        return LLVMLiricHostTargetName();
    }
};

class LLJITBuilder {
public:
    LLJIT *create() { return new LLJIT(); }
};

} // namespace orc
} // namespace llvm

#include "llvm/IR/Module.h"

inline int llvm::orc::LLJIT::addModule(llvm::Module &M) {
    return LLVMLiricSessionAddCompatModule(session_, M.getCompat());
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
