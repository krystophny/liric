#ifndef LLVM_EXECUTIONENGINE_ORC_CORE_H
#define LLVM_EXECUTIONENGINE_ORC_CORE_H

#include <llvm-c/LiricSession.h>
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/ExecutionEngine/Orc/CoreContainers.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace orc {

class DefinitionGenerator {
public:
    virtual ~DefinitionGenerator() = default;
};

class JITDylib {
    std::string Name;

public:
    JITDylib() : Name("main") {}
    JITDylib(const std::string &N) : Name(N) {}

    const std::string &getName() const { return Name; }

    void addGenerator(std::unique_ptr<DefinitionGenerator>) {}

    void setGenerator(std::unique_ptr<DefinitionGenerator>) {}
};

class ExecutionSession {
    LLVMLiricSessionStateRef session_;
    std::unique_ptr<ExecutorProcessControl> EPC;
    JITDylib MainJD;
    SymbolStringPool SSP;

public:
    ExecutionSession(std::unique_ptr<ExecutorProcessControl> epc)
        : session_(LLVMLiricSessionCreate()), EPC(std::move(epc)) {}

    ~ExecutionSession() {
        if (session_) LLVMLiricSessionDispose(session_);
    }

    ExecutionSession(const ExecutionSession &) = delete;
    ExecutionSession &operator=(const ExecutionSession &) = delete;

    LLVMLiricSessionStateRef getLiricSession() const { return session_; }

    int addCompatModule(lc_module_compat_t *mod) {
        return LLVMLiricSessionAddCompatModule(session_, mod);
    }

    void addSymbol(StringRef Name, void *Addr) {
        std::string symbol_name = Name.str();
        LLVMLiricSessionAddSymbol(session_, symbol_name.c_str(), Addr);
    }

    Expected<JITDylib &> createJITDylib(StringRef Name) {
        (void)Name;
        return MainJD;
    }

    JITDylib &getMainJITDylib() { return MainJD; }

    Expected<ExecutorSymbolDef> lookup(
        const std::vector<JITDylib *> &SearchOrder,
        SymbolStringPtr Name) {
        (void)SearchOrder;
        const std::string &name = *Name;
        void *addr = LLVMLiricSessionLookup(session_, name.c_str());
        if (!addr) {
            addr = dlsym(RTLD_DEFAULT, name.c_str());
        }
        if (!addr) {
            return make_error("Symbol not found: " + name);
        }
        ExecutorAddr ea = ExecutorAddr::fromPtr(addr);
        return ExecutorSymbolDef(ea, JITSymbolFlags());
    }

    ExecutorProcessControl &getExecutorProcessControl() { return *EPC; }

    SymbolStringPtr intern(StringRef Name) {
        return SymbolStringPtr(Name.str());
    }

    void reportError(Error Err) {
        logAllUnhandledErrors(std::move(Err), errs(), "JIT Error: ");
    }

    Error endSession() { return Error::success(); }
};

} // namespace orc
} // namespace llvm

#endif
