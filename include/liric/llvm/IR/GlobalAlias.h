#ifndef LLVM_IR_GLOBALALIAS_H
#define LLVM_IR_GLOBALALIAS_H
#include "llvm/IR/GlobalValue.h"
#include <stdexcept>
namespace liric_llvm {
class GlobalAlias : public GlobalValue {
    GlobalAlias() = delete;
public:
    static GlobalAlias *create(Type *, unsigned, LinkageTypes, StringRef,
                               Constant *, Module *) {
        throw std::runtime_error("liric: global aliases are not supported");
    }
    Constant *getAliasee() const {
        throw std::runtime_error("liric: global aliases are not supported");
    }
    static bool classof(const Value *) { return false; }
};
}
#endif
