#ifndef LLVM_EXECUTIONENGINE_ORC_LLJIT_H
#define LLVM_EXECUTIONENGINE_ORC_LLJIT_H

#include <liric/liric.h>
#include <liric/liric_compat.h>
#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm {

class Module;

namespace orc {

class LLJIT {
    lr_jit_t *jit_;

public:
    LLJIT() : jit_(lr_jit_create()) {}
    ~LLJIT() { if (jit_) lr_jit_destroy(jit_); }

    LLJIT(const LLJIT &) = delete;
    LLJIT &operator=(const LLJIT &) = delete;

    lr_jit_t *getJIT() const { return jit_; }

    inline int addModule(Module &M);

    void *lookup(StringRef Name) {
        std::string symbol_name = Name.str();
        return lr_jit_get_function(jit_, symbol_name.c_str());
    }

    void addSymbol(StringRef Name, void *Addr) {
        std::string symbol_name = Name.str();
        lr_jit_add_symbol(jit_, symbol_name.c_str(), Addr);
    }

    static const char *getHostTargetName() {
        return lr_jit_host_target_name();
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
    return lc_module_add_to_jit(M.getCompat(), jit_);
}

#endif
