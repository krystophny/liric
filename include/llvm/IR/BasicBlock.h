#ifndef LLVM_IR_BASICBLOCK_H
#define LLVM_IR_BASICBLOCK_H

#include "llvm/IR/Value.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace llvm {

class Function;
class LLVMContext;
class Module;

class BasicBlock : public Value {
public:
    lr_block_t *impl_block() const {
        return lc_value_get_block(impl());
    }

    static BasicBlock *wrap(lc_value_t *v) {
        return reinterpret_cast<BasicBlock *>(v);
    }

    static BasicBlock *Create(LLVMContext &Context, const Twine &Name = "",
                              Function *Parent = nullptr,
                              BasicBlock *InsertBefore = nullptr);

    Function *getParent() const { return nullptr; }
    Module *getModule() const { return nullptr; }

    bool empty() const { return true; }

    void eraseFromParent() {}
    void moveAfter(BasicBlock *) {}
    void moveBefore(BasicBlock *) {}
};

} // namespace llvm

#endif
