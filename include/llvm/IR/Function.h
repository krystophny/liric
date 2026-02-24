#ifndef LLVM_IR_FUNCTION_H
#define LLVM_IR_FUNCTION_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/ADT/StringRef.h"
#include <liric/llvm_compat_c.h>
#include <vector>

namespace llvm {

class Module;

class Function : public GlobalValue {
public:
    class iterator {
        Function *owner_;
        lr_block_t *block_;

        BasicBlock *current_block() const {
            if (!owner_ || !owner_->compat_mod_ || !block_) return nullptr;
            lc_value_t *bv = lc_value_block_ref(owner_->compat_mod_, block_);
            if (!bv) return nullptr;
            detail::register_block_parent(block_, owner_);
            return BasicBlock::wrap(bv);
        }

    public:
        iterator(Function *owner = nullptr, lr_block_t *block = nullptr)
            : owner_(owner), block_(block) {}

        BasicBlock *operator*() const { return current_block(); }
        BasicBlock *operator->() const { return current_block(); }
        iterator &operator++() {
            if (block_) block_ = block_->next;
            return *this;
        }
        bool operator!=(const iterator &o) const { return block_ != o.block_; }
        bool operator==(const iterator &o) const { return block_ == o.block_; }
        operator BasicBlock *() const { return current_block(); }
    };

    class BasicBlockListType {
        Function *owner_;

    public:
        using iterator = Function::iterator;

        explicit BasicBlockListType(Function *owner = nullptr) : owner_(owner) {}

        void setOwner(Function *owner) { owner_ = owner; }

        iterator begin() {
            lr_func_t *f = owner_ ? owner_->getIRFunc() : nullptr;
            return iterator(owner_, f ? f->first_block : nullptr);
        }

        iterator end() { return iterator(owner_, nullptr); }

        unsigned size() const {
            lr_func_t *f = owner_ ? owner_->getIRFunc() : nullptr;
            return lr_llvm_compat_function_block_count(f);
        }

        void push_back(BasicBlock *bb) {
            if (!owner_) return;
            owner_->insert(owner_->end(), bb);
        }
    };

private:
    lc_value_t *func_val_;
    lc_module_compat_t *compat_mod_;
    BasicBlockListType block_list_;

public:
    Function() : func_val_(nullptr), compat_mod_(nullptr), block_list_(this) {}
    ~Function() {
        detail::unregister_global_value_state(this);
    }

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

        Argument &operator*() const {
            lc_value_t *av = lc_func_get_arg(mod_, func_val_, idx_);
            return *reinterpret_cast<Argument *>(av);
        }

        Argument *operator->() const {
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

    class arg_range {
        arg_iterator b_, e_;
    public:
        arg_range(arg_iterator b, arg_iterator e) : b_(b), e_(e) {}
        arg_iterator begin() const { return b_; }
        arg_iterator end() const { return e_; }
    };

    arg_range args() {
        return arg_range(arg_begin(), arg_end());
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

    static inline Function *Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                     const Twine &Name, Module &M);
    static inline Function *Create(FunctionType *Ty, GlobalValue::LinkageTypes Linkage,
                                     const Twine &Name, Module *M = nullptr);

    void eraseFromParent() {}
    void removeFromParent() {}

    void setSubprogram(void *) {}

    Module *getParent() const { return nullptr; }

    BasicBlock &getEntryBlock() const {
        static BasicBlock dummy;
        lr_func_t *f = getIRFunc();
        if (!f || !f->first_block || !compat_mod_) return dummy;
        lc_value_t *bv = lc_value_block_ref(compat_mod_, f->first_block);
        if (!bv) return dummy;
        detail::register_block_parent(f->first_block,
            const_cast<Function *>(this));
        return *BasicBlock::wrap(bv);
    }

    BasicBlockListType &getBasicBlockList() {
        block_list_.setOwner(this);
        return block_list_;
    }

    void insert(iterator insert_before, BasicBlock *bb) {
        insert(static_cast<BasicBlock *>(insert_before), bb);
    }

    void insert(BasicBlock *insert_before, BasicBlock *bb) {
        lr_func_t *f = getIRFunc();
        if (!f || !compat_mod_ || !bb) return;

        lr_block_t *block = bb->impl_block();
        lr_block_t *anchor = insert_before ? insert_before->impl_block() : nullptr;
        if (!block) return;
        if (!lr_llvm_compat_function_insert_block(compat_mod_, f, block, anchor))
            return;
        detail::register_block_parent(block, this);
    }

    iterator begin() {
        lr_func_t *f = getIRFunc();
        return iterator(this, f ? f->first_block : nullptr);
    }

    iterator end() { return iterator(this, nullptr); }
};

} // namespace llvm

#endif
