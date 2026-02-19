#ifndef LLVM_IR_VALUE_H
#define LLVM_IR_VALUE_H

#include <liric/liric_compat.h>
#include "llvm/IR/Type.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

namespace llvm {

class raw_ostream;

class Value {
public:
    lc_value_t *impl() const {
        if (lc_value_t *wrapped = detail::lookup_value_wrapper(this)) {
            return wrapped;
        }
        return const_cast<lc_value_t *>(
            reinterpret_cast<const lc_value_t *>(this));
    }

    static Value *wrap(lc_value_t *v) {
        if (!v) {
            static lr_type_t poison_ty = { LR_TYPE_PTR, {} };
            static lc_value_t poison_val = { LC_VAL_CONST_UNDEF, &poison_ty, {} };
            return reinterpret_cast<Value *>(&poison_val);
        }
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

    void print(raw_ostream &OS, bool IsForDebug = false) const {
        (void)OS; (void)IsForDebug;
    }

    using use_iterator = Value **;
    bool use_empty() const { return true; }
    bool hasOneUse() const { return false; }

    void replaceAllUsesWith(Value *V) {
        if (!V) return;
        lc_value_replace_all_uses_with(impl(), V->impl());
    }
};

} // namespace llvm

#endif
