#ifndef LLVM_IR_VALUE_H
#define LLVM_IR_VALUE_H

#include <liric/liric_compat.h>
#include "llvm/IR/Type.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class Value {
public:
    lc_value_t *impl() const {
        return const_cast<lc_value_t *>(
            reinterpret_cast<const lc_value_t *>(this));
    }

    static Value *wrap(lc_value_t *v) {
        return reinterpret_cast<Value *>(v);
    }

    Type *getType() const {
        return Type::wrap(impl()->type);
    }

    unsigned getValueID() const {
        return static_cast<unsigned>(impl()->kind);
    }

    void setName(StringRef name) { (void)name; }
    StringRef getName() const { return ""; }

    bool hasName() const { return false; }

    using use_iterator = Value **;
    bool use_empty() const { return true; }
    bool hasOneUse() const { return false; }

    void replaceAllUsesWith(Value *V) { (void)V; }
};

} // namespace llvm

#endif
