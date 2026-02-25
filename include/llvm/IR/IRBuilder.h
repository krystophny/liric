#ifndef LLVM_IR_IRBUILDER_H
#define LLVM_IR_IRBUILDER_H

#include <liric/liric_compat.h>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/Support/Alignment.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include <liric/llvm_compat_c.h>
#include <vector>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(hidden)
#endif

namespace llvm {

class NoFolder {
public:
    Value *CreateAdd(Value *, Value *, const Twine & = "") { return nullptr; }
};

template <typename FolderTy = NoFolder, typename InserterTy = void>
class IRBuilder {
    lc_module_compat_t *mod_;
    lr_block_t *block_;
    lr_func_t *func_;
    LLVMContext &ctx_;

    lc_module_compat_t *M() const {
        if (mod_) return mod_;
        return Module::getCurrentModule();
    }
    lr_block_t *B() const { return block_; }
    lr_func_t *F() const { return func_; }

    static Type *pickIntrinsicOverloadType(ArrayRef<Type *> Types,
                                           ArrayRef<Value *> Args) {
        if (!Types.empty() && Types[0]) return Types[0];
        if (!Args.empty() && Args[0]) return Args[0]->getType();
        return nullptr;
    }

    static std::string intrinsicNameForID(Intrinsic::ID ID,
                                          ArrayRef<Type *> Types,
                                          ArrayRef<Value *> Args) {
        Type *over_ty = pickIntrinsicOverloadType(Types, Args);
        Type *powi_i_ty = nullptr;
        char name[128];
        if (Args.size() > 1 && Args[1]) powi_i_ty = Args[1]->getType();
        else if (Types.size() > 1 && Types[1]) powi_i_ty = Types[1];
        unsigned powi_bits = 32;
        if (powi_i_ty && powi_i_ty->isIntegerTy())
            powi_bits = powi_i_ty->getIntegerBitWidth();

        if (lr_llvm_compat_intrinsic_name(static_cast<unsigned>(ID),
                                          over_ty && over_ty->isFloatTy(),
                                          over_ty && over_ty->isDoubleTy(),
                                          over_ty && over_ty->isIntegerTy(),
                                          over_ty ? over_ty->getIntegerBitWidth() : 0,
                                          powi_bits,
                                          name, sizeof(name)) == 0) {
            return "";
        }
        return std::string(name);
    }

public:
    explicit IRBuilder(LLVMContext &C)
        : mod_(Module::getCurrentModule()), block_(nullptr), func_(nullptr), ctx_(C) {}

    explicit IRBuilder(BasicBlock *BB)
        : mod_(Module::getCurrentModule()), block_(nullptr), func_(nullptr),
          ctx_(LLVMContext::getGlobal()) {
        SetInsertPoint(BB);
    }

    IRBuilder(BasicBlock *BB, LLVMContext &C)
        : mod_(Module::getCurrentModule()), block_(nullptr), func_(nullptr), ctx_(C) {
        SetInsertPoint(BB);
    }

    LLVMContext &getContext() const { return ctx_; }

    void SetModule(lc_module_compat_t *mod) { mod_ = mod; }
    void SetModule(Module *mod) { mod_ = mod ? mod->getCompat() : nullptr; }

    void SetInsertPoint(BasicBlock *BB) {
        if (!BB) {
            block_ = nullptr;
            func_ = nullptr;
            detail::insertion_point_active_ref() = false;
            detail::current_function_ref() = nullptr;
            return;
        }
        block_ = BB->impl_block();
        detail::insertion_point_active_ref() = true;
        if (block_ && M()) {
            lc_block_attach(M(), block_);
        }
        lr_func_t *f = lc_value_get_block_func(BB->impl());
        if (!f) {
            Function *parent = detail::lookup_block_parent(block_);
            if (parent) {
                f = parent->getIRFunc();
                mod_ = parent->getCompatMod();
                detail::current_function_ref() = parent;
            }
        }
        func_ = f ? f : nullptr;
        if (func_) {
            Function *fn = detail::lookup_function_wrapper(func_);
            if (fn) {
                mod_ = fn->getCompatMod();
                detail::current_function_ref() = fn;
                detail::register_block_parent(block_, fn);
            }
        }
    }

    void SetInsertPoint(BasicBlock *BB, BasicBlock::iterator) {
        SetInsertPoint(BB);
    }

    void SetInsertPoint(BasicBlock *BB, BasicBlock::use_iterator) {
        SetInsertPoint(BB);
    }

    BasicBlock *GetInsertBlock() const {
        if (!block_) return nullptr;
        lc_module_compat_t *mod = mod_ ? mod_ : Module::getCurrentModule();
        if (!mod) return nullptr;
        lc_value_t *bv = lc_value_block_ref(mod, block_);
        if (func_) {
            Function *fn = detail::lookup_function_wrapper(func_);
            if (fn) detail::register_block_parent(block_, fn);
        }
        return BasicBlock::wrap(bv);
    }

    void SetFunction(lr_func_t *f) {
        func_ = f;
        detail::insertion_point_active_ref() = (f != nullptr);
        Function *fn = detail::lookup_function_wrapper(func_);
        if (fn) {
            mod_ = fn->getCompatMod();
            detail::current_function_ref() = fn;
        }
    }

    void SetInsertPointForFunction(Function *fn) {
        if (!fn) return;
        mod_ = fn->getCompatMod();
        func_ = fn->getIRFunc();
        block_ = nullptr;
        detail::insertion_point_active_ref() = true;
        detail::current_function_ref() = fn;
    }

    void ClearInsertionPoint() {
        block_ = nullptr;
        func_ = nullptr;
        detail::insertion_point_active_ref() = false;
        detail::current_function_ref() = nullptr;
    }

    void SetCurrentDebugLocation(const DebugLoc &) {}

    Type *getVoidTy() { return Type::getVoidTy(ctx_); }
    Type *getFloatTy() { return Type::getFloatTy(ctx_); }
    Type *getDoubleTy() { return Type::getDoubleTy(ctx_); }
    IntegerType *getInt1Ty() { return Type::getInt1Ty(ctx_); }
    IntegerType *getInt8Ty() { return Type::getInt8Ty(ctx_); }
    IntegerType *getInt16Ty() { return Type::getInt16Ty(ctx_); }
    IntegerType *getInt32Ty() { return Type::getInt32Ty(ctx_); }
    IntegerType *getInt64Ty() { return Type::getInt64Ty(ctx_); }

    ConstantInt *getInt1(bool V) {
        return ConstantInt::get(getInt1Ty(), V ? 1 : 0);
    }
    ConstantInt *getInt8(uint8_t V) {
        return ConstantInt::get(getInt8Ty(), V);
    }
    ConstantInt *getInt16(uint16_t V) {
        return ConstantInt::get(getInt16Ty(), V);
    }
    ConstantInt *getInt32(uint32_t V) {
        return ConstantInt::get(getInt32Ty(), V);
    }
    ConstantInt *getInt64(uint64_t V) {
        return ConstantInt::get(getInt64Ty(), V);
    }
    ConstantInt *getTrue() { return getInt1(true); }
    ConstantInt *getFalse() { return getInt1(false); }

    Value *CreateAdd(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_add(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateNSWAdd(Value *LHS, Value *RHS, const Twine &Name = "") {
        return CreateAdd(LHS, RHS, Name);
    }

    Value *CreateNUWAdd(Value *LHS, Value *RHS, const Twine &Name = "") {
        return CreateAdd(LHS, RHS, Name);
    }

    Value *CreateSub(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_sub(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateNSWSub(Value *LHS, Value *RHS, const Twine &Name = "") {
        return CreateSub(LHS, RHS, Name);
    }

    Value *CreateMul(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_mul(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateNSWMul(Value *LHS, Value *RHS, const Twine &Name = "") {
        return CreateMul(LHS, RHS, Name);
    }

    Value *CreateSDiv(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_sdiv(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateSRem(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_srem(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateUDiv(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_udiv(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateURem(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_urem(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateNeg(Value *V, const Twine &Name = "") {
        return Value::wrap(lc_create_neg(M(), B(), F(),
                           V->impl(), Name.c_str()));
    }

    Value *CreateNSWNeg(Value *V, const Twine &Name = "") {
        return CreateNeg(V, Name);
    }

    Value *CreateAnd(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_and(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateOr(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_or(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateOr(ArrayRef<Value *> Ops, const Twine &Name = "") {
        if (Ops.empty()) return Constant::getNullValue(getInt1Ty());
        Value *result = Ops[0];
        for (size_t i = 1; i < Ops.size(); i++) {
            result = CreateOr(result, Ops[i], Name);
        }
        return result;
    }

    Value *CreateXor(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_xor(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateShl(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_shl(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateShl(Value *LHS, uint64_t RHS, const Twine &Name = "") {
        Value *r = ConstantInt::get(LHS->getType(), RHS);
        return CreateShl(LHS, r, Name);
    }

    Value *CreateLShr(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_lshr(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateLShr(Value *LHS, uint64_t RHS, const Twine &Name = "") {
        Value *r = ConstantInt::get(LHS->getType(), RHS);
        return CreateLShr(LHS, r, Name);
    }

    Value *CreateAShr(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_ashr(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateAShr(Value *LHS, uint64_t RHS, const Twine &Name = "") {
        Value *r = ConstantInt::get(LHS->getType(), RHS);
        return CreateAShr(LHS, r, Name);
    }

    Value *CreateNot(Value *V, const Twine &Name = "") {
        return Value::wrap(lc_create_not(M(), B(), F(),
                           V->impl(), Name.c_str()));
    }

    Value *CreateFAdd(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fadd(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFSub(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fsub(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFMul(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fmul(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFDiv(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fdiv(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFNeg(Value *V, const Twine &Name = "") {
        return Value::wrap(lc_create_fneg(M(), B(), F(),
                           V->impl(), Name.c_str()));
    }

    Value *CreateICmpEQ(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_eq(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpNE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_ne(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpSLT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_slt(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpSLE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_sle(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpSGT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_sgt(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpSGE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_sge(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpULT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_ult(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpUGE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_uge(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpUGT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_ugt(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmpULE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_icmp_ule(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateICmp(CmpInst::Predicate P, Value *LHS, Value *RHS,
                      const Twine &Name = "") {
        switch (P) {
        case CmpInst::ICMP_EQ:  return CreateICmpEQ(LHS, RHS, Name);
        case CmpInst::ICMP_NE:  return CreateICmpNE(LHS, RHS, Name);
        case CmpInst::ICMP_SGT: return CreateICmpSGT(LHS, RHS, Name);
        case CmpInst::ICMP_SGE: return CreateICmpSGE(LHS, RHS, Name);
        case CmpInst::ICMP_SLT: return CreateICmpSLT(LHS, RHS, Name);
        case CmpInst::ICMP_SLE: return CreateICmpSLE(LHS, RHS, Name);
        case CmpInst::ICMP_UGT: return CreateICmpUGT(LHS, RHS, Name);
        case CmpInst::ICMP_UGE: return CreateICmpUGE(LHS, RHS, Name);
        case CmpInst::ICMP_ULT: return CreateICmpULT(LHS, RHS, Name);
        case CmpInst::ICMP_ULE: return CreateICmpULE(LHS, RHS, Name);
        default: return CreateICmpEQ(LHS, RHS, Name);
        }
    }

    Value *CreateFCmpOEQ(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_oeq(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpONE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_one(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpOLT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_olt(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpOLE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_ole(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpOGT(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_ogt(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpOGE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_oge(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpUNE(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_une(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpUEQ(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_ueq(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpORD(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_ord(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmpUNO(Value *LHS, Value *RHS, const Twine &Name = "") {
        return Value::wrap(lc_create_fcmp_uno(M(), B(), F(),
                           LHS->impl(), RHS->impl(), Name.c_str()));
    }

    Value *CreateFCmp(CmpInst::Predicate P, Value *LHS, Value *RHS,
                      const Twine &Name = "") {
        switch (P) {
        case CmpInst::FCMP_OEQ: return CreateFCmpOEQ(LHS, RHS, Name);
        case CmpInst::FCMP_ONE: return CreateFCmpONE(LHS, RHS, Name);
        case CmpInst::FCMP_OLT: return CreateFCmpOLT(LHS, RHS, Name);
        case CmpInst::FCMP_OLE: return CreateFCmpOLE(LHS, RHS, Name);
        case CmpInst::FCMP_OGT: return CreateFCmpOGT(LHS, RHS, Name);
        case CmpInst::FCMP_OGE: return CreateFCmpOGE(LHS, RHS, Name);
        case CmpInst::FCMP_UNE: return CreateFCmpUNE(LHS, RHS, Name);
        case CmpInst::FCMP_UEQ: return CreateFCmpUEQ(LHS, RHS, Name);
        case CmpInst::FCMP_ORD: return CreateFCmpORD(LHS, RHS, Name);
        case CmpInst::FCMP_UNO: return CreateFCmpUNO(LHS, RHS, Name);
        default: return CreateFCmpOEQ(LHS, RHS, Name);
        }
    }

    AllocaInst *CreateAlloca(Type *Ty, Value *ArraySize = nullptr,
                             const Twine &Name = "") {
        lc_alloca_inst_t *ai = lc_create_alloca(
            M(), B(), F(), Ty->impl(),
            ArraySize ? ArraySize->impl() : nullptr, Name.c_str());
        if (!ai) return static_cast<AllocaInst *>(Value::wrap(nullptr));
        lc_value_t *result = ai->result;
        free(ai);
        return AllocaInst::wrap(result);
    }

    AllocaInst *CreateAlloca(Type *Ty, unsigned AddrSpace,
                             Value *ArraySize = nullptr,
                             const Twine &Name = "") {
        (void)AddrSpace;
        return CreateAlloca(Ty, ArraySize, Name);
    }

    Value *CreateLoad(Type *Ty, Value *Ptr, const Twine &Name = "") {
        return Value::wrap(lc_create_load(M(), B(), F(),
                           Ty->impl(), Ptr->impl(), Name.c_str()));
    }

    Value *CreateLoad(Type *Ty, Value *Ptr, bool isVolatile,
                      const Twine &Name = "") {
        (void)isVolatile;
        return CreateLoad(Ty, Ptr, Name);
    }

    Value *CreateStore(Value *Val, Value *Ptr, bool isVolatile = false) {
        (void)isVolatile;
        lc_create_store(M(), B(), Val->impl(), Ptr->impl());
        return nullptr;
    }

    Value *CreateGEP(Type *Ty, Value *Ptr, ArrayRef<Value *> IdxList,
                     const Twine &Name = "") {
        std::vector<lc_value_t *> indices(IdxList.size());
        for (size_t i = 0; i < IdxList.size(); i++) {
            indices[i] = IdxList[i]->impl();
        }
        return Value::wrap(lc_create_gep(
            M(), B(), F(), Ty->impl(), Ptr->impl(),
            indices.data(),
            static_cast<unsigned>(indices.size()), Name.c_str()));
    }

    Value *CreateGEP(Type *Ty, Value *Ptr, Value *Idx,
                     const Twine &Name = "") {
        lc_value_t *idx = Idx->impl();
        return Value::wrap(lc_create_gep(
            M(), B(), F(), Ty->impl(), Ptr->impl(),
            &idx, 1, Name.c_str()));
    }

    Value *CreateInBoundsGEP(Type *Ty, Value *Ptr, ArrayRef<Value *> IdxList,
                             const Twine &Name = "") {
        std::vector<lc_value_t *> indices(IdxList.size());
        for (size_t i = 0; i < IdxList.size(); i++) {
            indices[i] = IdxList[i]->impl();
        }
        return Value::wrap(lc_create_inbounds_gep(
            M(), B(), F(), Ty->impl(), Ptr->impl(),
            indices.data(),
            static_cast<unsigned>(indices.size()), Name.c_str()));
    }

    Value *CreateInBoundsGEP(Type *Ty, Value *Ptr, Value *Idx,
                             const Twine &Name = "") {
        lc_value_t *idx = Idx->impl();
        return Value::wrap(lc_create_inbounds_gep(
            M(), B(), F(), Ty->impl(), Ptr->impl(),
            &idx, 1, Name.c_str()));
    }

    Value *CreateStructGEP(Type *Ty, Value *Ptr, unsigned Idx,
                           const Twine &Name = "") {
        return Value::wrap(lc_create_struct_gep(
            M(), B(), F(), Ty->impl(), Ptr->impl(),
            Idx, Name.c_str()));
    }

    Value *CreateConstGEP2_32(Type *Ty, Value *Ptr,
                              unsigned Idx0, unsigned Idx1,
                              const Twine &Name = "") {
        Value *i0 = ConstantInt::get(getInt32Ty(), Idx0);
        Value *i1 = ConstantInt::get(getInt32Ty(), Idx1);
        Value *idxs[2] = { i0, i1 };
        return CreateGEP(Ty, Ptr, ArrayRef<Value *>(idxs, 2), Name);
    }

    ReturnInst *CreateRet(Value *V) {
        lc_create_ret(M(), B(), V->impl());
        return nullptr;
    }

    ReturnInst *CreateRetVoid() {
        lc_create_ret_void(M(), B());
        return nullptr;
    }

    BranchInst *CreateBr(BasicBlock *Dest) {
        lr_block_t *dest_block = Dest ? Dest->impl_block() : nullptr;
        if (!B()) return nullptr;
        if (dest_block && M() && !dest_block->func && B()->func) {
            (void)lc_block_bind_func(M(), dest_block, B()->func);
        }
        if (dest_block && M()) {
            lc_block_attach(M(), dest_block);
        }
        if (!dest_block) {
            lc_create_unreachable(M(), B());
            return nullptr;
        }
        lc_create_br(M(), B(), dest_block);
        return nullptr;
    }

    BranchInst *CreateCondBr(Value *Cond, BasicBlock *True,
                             BasicBlock *False) {
        lr_block_t *true_block = True ? True->impl_block() : nullptr;
        lr_block_t *false_block = False ? False->impl_block() : nullptr;
        if (!B()) return nullptr;
        if (M() && B()->func) {
            if (true_block && !true_block->func) {
                (void)lc_block_bind_func(M(), true_block, B()->func);
            }
            if (false_block && !false_block->func) {
                (void)lc_block_bind_func(M(), false_block, B()->func);
            }
        }
        if (true_block && M()) {
            lc_block_attach(M(), true_block);
        }
        if (false_block && M()) {
            lc_block_attach(M(), false_block);
        }
        if (!Cond || !true_block || !false_block) {
            lc_create_unreachable(M(), B());
            return nullptr;
        }
        lc_create_cond_br(M(), B(), Cond->impl(), true_block, false_block);
        return nullptr;
    }

    SwitchInst *CreateSwitch(Value *V, BasicBlock *Default,
                             unsigned NumCases = 10) {
        (void)NumCases;
        if (!M() || !B() || !F() || !V || !Default || !Default->impl_block()) {
            return nullptr;
        }
        lc_switch_builder_t *builder = lc_switch_builder_create(
            M(), B(), F(), V->impl(), Default->impl_block());
        if (!builder) {
            return nullptr;
        }
        return new SwitchInst(builder);
    }

    void CreateUnreachable() {
        lc_create_unreachable(M(), B());
    }

    Value *CreateCall(FunctionType *FTy, Value *Callee,
                      ArrayRef<Value *> Args = {},
                      const Twine &Name = "") {
        std::vector<lc_value_t *> args(Args.size());
        for (size_t i = 0; i < Args.size(); i++) {
            args[i] = Args[i] ? Args[i]->impl() : nullptr;
        }
        return Value::wrap(lc_create_call(
            M(), B(), F(), FTy ? FTy->impl() : nullptr,
            Callee ? Callee->impl() : nullptr,
            args.empty() ? nullptr : args.data(),
            static_cast<unsigned>(args.size()), Name.c_str()));
    }

    Value *CreateCall(Function *Callee, ArrayRef<Value *> Args = {},
                      const Twine &Name = "") {
        FunctionType *fty = Callee ? Callee->getFunctionType() : nullptr;
        lc_value_t *fv = Callee ? Callee->getFuncVal() : nullptr;
        return CreateCall(fty, fv ? Value::wrap(fv) : nullptr, Args, Name);
    }

    PHINode *CreatePHI(Type *Ty, unsigned NumReservedValues,
                       const Twine &Name = "") {
        (void)NumReservedValues;
        lc_phi_node_t *phi = lc_create_phi(M(), B(), F(),
                                            Ty->impl(), Name.c_str());
        return PHINode::wrap(phi->result);
    }

    Value *CreateSelect(Value *C, Value *True, Value *False,
                        const Twine &Name = "") {
        return Value::wrap(lc_create_select(
            M(), B(), F(), C->impl(), True->impl(),
            False->impl(), Name.c_str()));
    }

    Value *CreateSExt(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_sext(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateZExt(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_zext(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateTrunc(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_trunc(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateBitCast(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_bitcast(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreatePtrToInt(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_ptrtoint(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateIntToPtr(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_inttoptr(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateSIToFP(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_sitofp(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateUIToFP(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_uitofp(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateFPToSI(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_fptosi(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateFPToUI(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_fptoui(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateFPExt(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_fpext(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateFPTrunc(Value *V, Type *DestTy, const Twine &Name = "") {
        return Value::wrap(lc_create_fptrunc(M(), B(), F(),
                           V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateSExtOrTrunc(Value *V, Type *DestTy,
                             const Twine &Name = "") {
        return Value::wrap(lc_create_sext_or_trunc(
            M(), B(), F(), V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateZExtOrTrunc(Value *V, Type *DestTy,
                             const Twine &Name = "") {
        return Value::wrap(lc_create_zext_or_trunc(
            M(), B(), F(), V->impl(), DestTy->impl(), Name.c_str()));
    }

    Value *CreateSExtOrBitCast(Value *V, Type *DestTy,
                               const Twine &Name = "") {
        unsigned src_bits = V->getType()->getScalarSizeInBits();
        unsigned dst_bits = DestTy->getScalarSizeInBits();
        if (src_bits < dst_bits) return CreateSExt(V, DestTy, Name);
        return CreateBitCast(V, DestTy, Name);
    }

    Value *CreateExtractValue(Value *Agg, ArrayRef<unsigned> Idxs,
                              const Twine &Name = "") {
        std::vector<unsigned> idx_vec(Idxs.begin(), Idxs.end());
        return Value::wrap(lc_create_extractvalue(
            M(), B(), F(), Agg->impl(),
            idx_vec.data(),
            static_cast<unsigned>(idx_vec.size()), Name.c_str()));
    }

    Value *CreateInsertValue(Value *Agg, Value *Val,
                             ArrayRef<unsigned> Idxs,
                             const Twine &Name = "") {
        std::vector<unsigned> idx_vec(Idxs.begin(), Idxs.end());
        return Value::wrap(lc_create_insertvalue(
            M(), B(), F(), Agg->impl(), Val->impl(),
            idx_vec.data(),
            static_cast<unsigned>(idx_vec.size()), Name.c_str()));
    }

    Value *CreateExtractElement(Value *Vec, Value *Idx,
                                const Twine &Name = "") {
        if (auto *CI = static_cast<ConstantInt *>(Idx)) {
            unsigned idx_val = static_cast<unsigned>(CI->getZExtValue());
            return CreateExtractValue(Vec, {idx_val}, Name);
        }
        return CreateExtractValue(Vec, {0}, Name);
    }

    Value *CreateExtractElement(Value *Vec, uint64_t Idx,
                                const Twine &Name = "") {
        return CreateExtractValue(Vec, {static_cast<unsigned>(Idx)}, Name);
    }

    Value *CreateInsertElement(Value *Vec, Value *NewElt, Value *Idx,
                               const Twine &Name = "") {
        if (auto *CI = static_cast<ConstantInt *>(Idx)) {
            unsigned idx_val = static_cast<unsigned>(CI->getZExtValue());
            return CreateInsertValue(Vec, NewElt, {idx_val}, Name);
        }
        return CreateInsertValue(Vec, NewElt, {0}, Name);
    }

    Value *CreateInsertElement(Value *Vec, Value *NewElt, uint64_t Idx,
                               const Twine &Name = "") {
        return CreateInsertValue(Vec, NewElt, {static_cast<unsigned>(Idx)}, Name);
    }

    Value *CreateMemCpy(Value *Dst, unsigned DstAlign,
                        Value *Src, unsigned SrcAlign,
                        Value *Size, bool isVolatile = false) {
        (void)DstAlign; (void)SrcAlign; (void)isVolatile;
        lc_create_memcpy(M(), B(), F(),
                         Dst->impl(), Src->impl(), Size->impl());
        return nullptr;
    }

    Value *CreateMemCpy(Value *Dst, unsigned DstAlign,
                        Value *Src, unsigned SrcAlign,
                        uint64_t Size, bool isVolatile = false) {
        Value *sz = ConstantInt::get(Type::getInt64Ty(ctx_), Size);
        return CreateMemCpy(Dst, DstAlign, Src, SrcAlign, sz, isVolatile);
    }

    Value *CreateMemCpy(Value *Dst, MaybeAlign DstAlign,
                        Value *Src, MaybeAlign SrcAlign,
                        Value *Size, bool isVolatile = false) {
        return CreateMemCpy(Dst, (unsigned)DstAlign.valueOrOne(),
                           Src, (unsigned)SrcAlign.valueOrOne(),
                           Size, isVolatile);
    }

    Value *CreateMemCpy(Value *Dst, MaybeAlign DstAlign,
                        Value *Src, MaybeAlign SrcAlign,
                        uint64_t Size, bool isVolatile = false) {
        return CreateMemCpy(Dst, (unsigned)DstAlign.valueOrOne(),
                           Src, (unsigned)SrcAlign.valueOrOne(),
                           Size, isVolatile);
    }

    Value *CreateMemMove(Value *Dst, unsigned DstAlign,
                         Value *Src, unsigned SrcAlign,
                         Value *Size, bool isVolatile = false) {
        (void)DstAlign; (void)SrcAlign; (void)isVolatile;
        lc_create_memmove(M(), B(), F(),
                          Dst->impl(), Src->impl(), Size->impl());
        return nullptr;
    }

    Value *CreateMemMove(Value *Dst, unsigned DstAlign,
                         Value *Src, unsigned SrcAlign,
                         uint64_t Size, bool isVolatile = false) {
        Value *sz = ConstantInt::get(Type::getInt64Ty(ctx_), Size);
        return CreateMemMove(Dst, DstAlign, Src, SrcAlign, sz, isVolatile);
    }

    Value *CreateMemMove(Value *Dst, MaybeAlign DstAlign,
                         Value *Src, MaybeAlign SrcAlign,
                         Value *Size, bool isVolatile = false) {
        return CreateMemMove(Dst, (unsigned)DstAlign.valueOrOne(),
                            Src, (unsigned)SrcAlign.valueOrOne(),
                            Size, isVolatile);
    }

    Value *CreateMemSet(Value *Ptr, Value *Val, Value *Size,
                        unsigned Align, bool isVolatile = false) {
        (void)Align; (void)isVolatile;
        lc_create_memset(M(), B(), F(),
                         Ptr->impl(), Val->impl(), Size->impl());
        return nullptr;
    }

    Value *CreateMemSet(Value *Ptr, Value *Val, uint64_t Size,
                        unsigned Align, bool isVolatile = false) {
        Value *sz = ConstantInt::get(Type::getInt64Ty(ctx_), Size);
        return CreateMemSet(Ptr, Val, sz, Align, isVolatile);
    }

    Value *CreateMemSet(Value *Ptr, Value *Val, Value *Size,
                        MaybeAlign Align, bool isVolatile = false) {
        return CreateMemSet(Ptr, Val, Size, (unsigned)Align.valueOrOne(),
                           isVolatile);
    }

    Value *CreateMemSet(Value *Ptr, Value *Val, uint64_t Size,
                        MaybeAlign Align, bool isVolatile = false) {
        return CreateMemSet(Ptr, Val, Size, (unsigned)Align.valueOrOne(),
                           isVolatile);
    }

    Value *CreatePointerCast(Value *V, Type *DestTy,
                             const Twine &Name = "") {
        return CreateBitCast(V, DestTy, Name);
    }

    Value *CreateIntCast(Value *V, Type *DestTy, bool isSigned,
                         const Twine &Name = "") {
        if (isSigned) return CreateSExtOrTrunc(V, DestTy, Name);
        return CreateZExtOrTrunc(V, DestTy, Name);
    }

    Value *CreateFPCast(Value *V, Type *DestTy, const Twine &Name = "") {
        if (V->getType()->getScalarSizeInBits() <
            DestTy->getScalarSizeInBits()) {
            return CreateFPExt(V, DestTy, Name);
        }
        return CreateFPTrunc(V, DestTy, Name);
    }

    Value *CreateIsNull(Value *Arg, const Twine &Name = "") {
        return CreateICmpEQ(Arg,
            Constant::getNullValue(Arg->getType()), Name);
    }

    Value *CreateIsNotNull(Value *Arg, const Twine &Name = "") {
        return CreateICmpNE(Arg,
            Constant::getNullValue(Arg->getType()), Name);
    }

    Constant *CreateGlobalStringPtr(StringRef Str, const Twine &Name = "",
                                    unsigned AddressSpace = 0) {
        (void)AddressSpace;
        lc_module_compat_t *mod = M();
        if (!mod) return nullptr;
        std::string data(Str.data(), Str.size());
        data.push_back('\0');
        lr_type_t *elem_ty = lc_get_int_type(mod, 8);
        lr_type_t *arr_ty = lr_type_array_new(lc_module_get_ir(mod), elem_ty,
                                              data.size());
        std::string generated_name;
        std::string actual_name;
        std::string explicit_name = Name.str();
        if (!explicit_name.empty()) {
            actual_name = Module::linkageScopedGlobalName(
                mod, explicit_name, GlobalValue::PrivateLinkage);
        } else {
            static thread_local unsigned long long str_id = 0;
            generated_name = ".str." + std::to_string(str_id++);
            actual_name = Module::linkageScopedGlobalName(
                mod, generated_name, GlobalValue::PrivateLinkage);
        }
        lc_value_t *gv = lc_global_create(
            mod, actual_name.c_str(), arr_ty, true, data.data(), data.size());
        if (!explicit_name.empty() && gv && gv->kind == LC_VAL_GLOBAL &&
            gv->global.name && explicit_name != gv->global.name) {
            detail::register_global_alias(mod, explicit_name, gv->global.name);
        }
        return static_cast<Constant *>(Value::wrap(gv));
    }

    Value *CreateGlobalString(StringRef Str, const Twine &Name = "",
                              unsigned AddressSpace = 0) {
        return CreateGlobalStringPtr(Str, Name, AddressSpace);
    }

    Value *CreateIntrinsic(Intrinsic::ID ID, ArrayRef<Type *> Types,
                           ArrayRef<Value *> Args,
                           const Twine &Name = "") {
        std::string intrinsic_name = intrinsicNameForID(ID, Types, Args);
        if (intrinsic_name.empty()) return nullptr;

        lc_module_compat_t *mod = M();
        lr_module_t *m = lc_module_get_ir(mod);

        lr_type_t *ret_ty = lc_get_void_type(mod);
        if (!Args.empty() && Args[0]) {
            ret_ty = Args[0]->getType()->impl();
        }
        if (!Types.empty() && Types[0]) {
            ret_ty = Types[0]->impl();
        }

        std::vector<lr_type_t *> param_types(Args.size());
        for (size_t i = 0; i < Args.size(); i++) {
            param_types[i] = Args[i]->getType()->impl();
        }

        lr_type_t *ft = lr_type_func_new(
            m, ret_ty,
            param_types.empty() ? nullptr : param_types.data(),
            static_cast<uint32_t>(param_types.size()), false);

        lc_value_t *callee_val = lc_global_lookup_or_create(
            mod, intrinsic_name.c_str(), ft);

        std::vector<lc_value_t *> args(Args.size());
        for (size_t i = 0; i < Args.size(); i++) {
            args[i] = Args[i]->impl();
        }

        return Value::wrap(lc_create_call(
            mod, B(), F(), ft, callee_val,
            args.empty() ? nullptr : args.data(),
            static_cast<unsigned>(args.size()), Name.c_str()));
    }

    Value *CreateUnaryIntrinsic(Intrinsic::ID ID, Value *V,
                                const Twine &Name = "") {
        Type *ty = V->getType();
        return CreateIntrinsic(ID, {ty}, {V}, Name);
    }
};

} // namespace llvm

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif
