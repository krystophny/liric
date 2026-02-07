#ifndef LLVM_EXECUTIONENGINE_ORC_EXECUTIONUTILS_H
#define LLVM_EXECUTIONENGINE_ORC_EXECUTIONUTILS_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/Support/Error.h"
#include <functional>
#include <memory>

namespace llvm {
namespace orc {

class DynamicLibrarySearchGenerator : public DefinitionGenerator {
public:
    static Expected<std::unique_ptr<DynamicLibrarySearchGenerator>>
    GetForCurrentProcess(
        char GlobalPrefix,
        std::function<bool(const SymbolStringPtr &)> Filter = {}) {
        (void)GlobalPrefix;
        (void)Filter;
        return std::make_unique<DynamicLibrarySearchGenerator>();
    }

    static Expected<std::unique_ptr<DynamicLibrarySearchGenerator>>
    Load(const char *FileName, char GlobalPrefix,
         std::function<bool(const SymbolStringPtr &)> Filter = {}) {
        (void)FileName;
        (void)GlobalPrefix;
        (void)Filter;
        return std::make_unique<DynamicLibrarySearchGenerator>();
    }
};

} // namespace orc
} // namespace llvm

#endif
