#ifndef LLVM_IR_FUNCTION_H
#define LLVM_IR_FUNCTION_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace llvm {

class Module;

class Function : public GlobalValue {
    lc_value_t *func_val_;
    lc_module_compat_t *compat_mod_;

public:
    Function() : func_val_(nullptr), compat_mod_(nullptr) {}

    void setFuncVal(lc_value_t *fv) { func_val_ = fv; }
    lc_value_t *getFuncVal() const { return func_val_; }

    void setCompatMod(lc_module_compat_t *m) { compat_mod_ = m; }
    lc_module_compat_t *getCompatMod() const { return compat_mod_; }

    lr_func_t *getIRFunc() const {
        if (!func_val_) return nullptr;
        return lc_value_get_func(func_val_);
    }

    StringRef getName() const {
        lr_func_t *f = getIRFunc();
        return f ? f->name : "";
    }

    FunctionType *getFunctionType() const {
        lr_func_t *f = getIRFunc();
        if (!f) return nullptr;
        return FunctionType::wrap(f->type);
    }

    Type *getReturnType() const {
        lr_func_t *f = getIRFunc();
        if (!f) return nullptr;
        return Type::wrap(f->ret_type);
    }

    unsigned arg_size() const {
        return func_val_ ? lc_func_arg_count(func_val_) : 0;
    }

    class arg_iterator {
        lc_module_compat_t *mod_;
        lc_value_t *func_val_;
        unsigned idx_;

    public:
        arg_iterator(lc_module_compat_t *m, lc_value_t *fv, unsigned i)
            : mod_(m), func_val_(fv), idx_(i) {}

        Argument *operator*() const {
            lc_value_t *av = lc_func_get_arg(mod_, func_val_, idx_);
            return reinterpret_cast<Argument *>(av);
        }

        arg_iterator &operator++() { ++idx_; return *this; }
        bool operator!=(const arg_iterator &o) const { return idx_ != o.idx_; }
        bool operator==(const arg_iterator &o) const { return idx_ == o.idx_; }
    };

    arg_iterator arg_begin() {
        return arg_iterator(compat_mod_, func_val_, 0);
    }
    arg_iterator arg_end() {
        return arg_iterator(compat_mod_, func_val_, arg_size());
    }

    Argument *getArg(unsigned i) {
        lc_value_t *av = lc_func_get_arg(compat_mod_, func_val_, i);
        return reinterpret_cast<Argument *>(av);
    }

    void setCallingConv(CallingConv::ID CC) { (void)CC; }
    CallingConv::ID getCallingConv() const { return CallingConv::C; }

    void addFnAttr(Attribute::AttrKind K) { (void)K; }
    void addFnAttr(StringRef K, StringRef V = "") { (void)K; (void)V; }
    void addRetAttr(Attribute::AttrKind K) { (void)K; }
    void addParamAttr(unsigned, Attribute::AttrKind) {}
    void setAttributes(AttributeList) {}
    AttributeList getAttributes() const { return AttributeList(); }
    void setDoesNotReturn() {}
    void setDoesNotThrow() {}
    bool isIntrinsic() const { return false; }
    bool isDeclaration() const {
        lr_func_t *f = getIRFunc();
        return f ? f->is_decl : true;
    }
    bool empty() const {
        lr_func_t *f = getIRFunc();
        return f ? (f->first_block == nullptr) : true;
    }

    void eraseFromParent() {}

    using iterator = BasicBlock *;
    BasicBlock *begin() { return nullptr; }
    BasicBlock *end() { return nullptr; }
};

} // namespace llvm

#endif
