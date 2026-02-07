#ifndef LLVM_IR_ARGUMENT_H
#define LLVM_IR_ARGUMENT_H

#include "llvm/IR/Value.h"

namespace llvm {

class Function;

class Argument : public Value {
public:
    unsigned getArgNo() const {
        return impl()->argument.param_idx;
    }

    void addAttr(unsigned) {}
};

} // namespace llvm

#endif
