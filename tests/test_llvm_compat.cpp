#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#include <liric/liric_compat.h>

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    fprintf(stderr, "  %s...", #fn); \
    if (fn() == 0) { \
        tests_passed++; \
        fprintf(stderr, " ok\n"); \
    } else { \
        tests_failed++; \
        fprintf(stderr, "\n"); \
    } \
} while (0)

static int test_llvm_version() {
    TEST_ASSERT_EQ(LLVM_VERSION_MAJOR, 21, "version major");
    return 0;
}

static int test_context_and_module() {
    llvm::LLVMContext ctx;
    llvm::Module mod("test", ctx);

    TEST_ASSERT(mod.getCompat() != nullptr, "module created");
    TEST_ASSERT(mod.getIR() != nullptr, "ir module present");
    TEST_ASSERT(llvm::Module::getCurrentModule() == mod.getCompat(),
                "current module set");
    return 0;
}

static int test_basic_types() {
    llvm::LLVMContext ctx;
    llvm::Module mod("types", ctx);

    llvm::Type *voidTy = llvm::Type::getVoidTy(ctx);
    llvm::Type *floatTy = llvm::Type::getFloatTy(ctx);
    llvm::Type *doubleTy = llvm::Type::getDoubleTy(ctx);
    llvm::IntegerType *i1 = llvm::Type::getInt1Ty(ctx);
    llvm::IntegerType *i8 = llvm::Type::getInt8Ty(ctx);
    llvm::IntegerType *i16 = llvm::Type::getInt16Ty(ctx);
    llvm::IntegerType *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::IntegerType *i64 = llvm::Type::getInt64Ty(ctx);

    TEST_ASSERT(voidTy != nullptr, "void type");
    TEST_ASSERT(voidTy->isVoidTy(), "is void");
    TEST_ASSERT(floatTy->isFloatTy(), "is float");
    TEST_ASSERT(doubleTy->isDoubleTy(), "is double");
    TEST_ASSERT(i1->isIntegerTy(), "i1 is integer");
    TEST_ASSERT_EQ(i1->getBitWidth(), 1, "i1 width");
    TEST_ASSERT_EQ(i8->getBitWidth(), 8, "i8 width");
    TEST_ASSERT_EQ(i16->getBitWidth(), 16, "i16 width");
    TEST_ASSERT_EQ(i32->getBitWidth(), 32, "i32 width");
    TEST_ASSERT_EQ(i64->getBitWidth(), 64, "i64 width");

    llvm::PointerType *ptrTy = llvm::PointerType::getUnqual(ctx);
    TEST_ASSERT(ptrTy != nullptr, "ptr type");
    TEST_ASSERT(ptrTy->isPointerTy(), "is pointer");

    return 0;
}

static int test_type_context_stability_across_nested_modules() {
    llvm::LLVMContext ctx_a;
    llvm::Module mod_a("ctx_a", ctx_a);

    llvm::Type *a_i8 = llvm::Type::getInt8Ty(ctx_a);
    llvm::Type *a_i8_ptr = a_i8 ? a_i8->getPointerTo() : nullptr;
    TEST_ASSERT(a_i8 != nullptr, "ctx_a i8");
    TEST_ASSERT(a_i8_ptr != nullptr, "ctx_a i8*");

    {
        llvm::LLVMContext ctx_b;
        llvm::Module mod_b("ctx_b", ctx_b);
        llvm::Type *b_i8 = llvm::Type::getInt8Ty(ctx_b);
        llvm::Type *b_i8_ptr = b_i8 ? b_i8->getPointerTo() : nullptr;
        TEST_ASSERT(b_i8 != nullptr, "ctx_b i8");
        TEST_ASSERT(b_i8_ptr != nullptr, "ctx_b i8*");
        TEST_ASSERT(a_i8 != b_i8, "contexts must keep distinct i8 identities");
        TEST_ASSERT(a_i8_ptr != b_i8_ptr, "contexts must keep distinct i8* identities");
    }

    llvm::Type *a_i8_after = llvm::Type::getInt8Ty(ctx_a);
    llvm::Type *a_i8_ptr_after = a_i8_after ? a_i8_after->getPointerTo() : nullptr;
    TEST_ASSERT(a_i8_after == a_i8, "ctx_a i8 identity stable after nested module");
    TEST_ASSERT(a_i8_ptr_after == a_i8_ptr, "ctx_a i8* identity stable after nested module");
    return 0;
}

static int test_function_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("ftypes", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32, i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, params, false);

    TEST_ASSERT(ft != nullptr, "func type created");
    TEST_ASSERT_EQ(ft->getNumParams(), 2, "param count");
    TEST_ASSERT(!ft->isVarArg(), "not vararg");

    llvm::FunctionType *fv = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), false);
    TEST_ASSERT(fv != nullptr, "void func type");
    TEST_ASSERT(fv->getReturnType()->isVoidTy(), "returns void");

    return 0;
}

static int test_struct_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("structs", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);

    llvm::StructType *st = llvm::StructType::create(ctx, "my_struct");
    TEST_ASSERT(st != nullptr, "named struct created");
    TEST_ASSERT(st->isOpaque(), "initially opaque");

    llvm::Type *fields[] = {i32, i64};
    st->setBody(fields);
    TEST_ASSERT_EQ(st->getNumElements(), 2, "2 fields after setBody");
    TEST_ASSERT(st->getElementType(0)->isIntegerTy(32), "field 0 is i32");
    TEST_ASSERT(st->getElementType(1)->isIntegerTy(64), "field 1 is i64");

    llvm::StructType *lit = llvm::StructType::get(ctx, fields);
    TEST_ASSERT(lit != nullptr, "literal struct");
    TEST_ASSERT_EQ(lit->getNumElements(), 2, "literal 2 fields");

    return 0;
}

static int test_array_type() {
    llvm::LLVMContext ctx;
    llvm::Module mod("arrays", ctx);

    llvm::Type *i8 = llvm::Type::getInt8Ty(ctx);
    llvm::ArrayType *at = llvm::ArrayType::get(i8, 16);

    TEST_ASSERT(at != nullptr, "array type created");
    TEST_ASSERT(at->isArrayTy(), "is array");
    TEST_ASSERT_EQ(at->getNumElements(), 16, "16 elements");
    TEST_ASSERT(at->getElementType()->isIntegerTy(8), "element is i8");

    return 0;
}

static int test_constants() {
    llvm::LLVMContext ctx;
    llvm::Module mod("consts", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::ConstantInt *c42 = llvm::ConstantInt::get(i32, 42);

    TEST_ASSERT(c42 != nullptr, "const int created");
    TEST_ASSERT_EQ(c42->getSExtValue(), 42, "value is 42");
    TEST_ASSERT_EQ(c42->getZExtValue(), 42, "zext value is 42");

    llvm::ConstantInt *cn = llvm::ConstantInt::get(i32, -1, true);
    TEST_ASSERT_EQ(cn->getSExtValue(), -1, "signed -1");

    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::ConstantFP *cfp = llvm::ConstantFP::get(dblTy, 3.14);
    TEST_ASSERT(cfp != nullptr, "const fp created");

    llvm::PointerType *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::ConstantPointerNull *cpn = llvm::ConstantPointerNull::get(ptrTy);
    TEST_ASSERT(cpn != nullptr, "null pointer");

    llvm::UndefValue *uv = llvm::UndefValue::get(i32);
    TEST_ASSERT(uv != nullptr, "undef value");

    llvm::Constant *nv = llvm::Constant::getNullValue(i32);
    TEST_ASSERT(nv != nullptr, "null value");

    return 0;
}

static int test_function_creation() {
    llvm::LLVMContext ctx;
    llvm::Module mod("funcs", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("add", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");
    TEST_ASSERT_EQ(fn->arg_size(), 2, "2 args");

    llvm::Function *decl = mod.createFunction("ext_func", ft, true);
    TEST_ASSERT(decl != nullptr, "declaration created");

    return 0;
}

static int test_block_parent_tracking_across_decls() {
    llvm::LLVMContext ctx;
    llvm::Module mod("parent_tracking", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, params, false);

    llvm::Function *main_fn = mod.createFunction("main_fn", ft, false);
    llvm::Function *ext_decl = mod.createFunction("ext_decl", ft, true);
    TEST_ASSERT(ext_decl != nullptr, "decl created");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", nullptr);
    TEST_ASSERT(entry->impl_block() != nullptr, "implicit parent block created");
    TEST_ASSERT(entry->getParent() == main_fn, "decl must not clobber current function");

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(main_fn);
    builder.SetInsertPoint(entry);

    llvm::BasicBlock *insert_block = builder.GetInsertBlock();
    TEST_ASSERT(insert_block != nullptr, "insert block");
    TEST_ASSERT(insert_block->getParent() == main_fn, "insert block parent");

    llvm::Function *other_fn = mod.createFunction("other_fn", ft, false);
    TEST_ASSERT(other_fn != nullptr, "other function");
    builder.SetInsertPointForFunction(other_fn);

    llvm::Function *late_decl = mod.createFunction("late_decl", ft, true);
    TEST_ASSERT(late_decl != nullptr, "late decl");

    builder.SetInsertPoint(entry);
    llvm::BasicBlock *insert_before = llvm::BasicBlock::Create(ctx, "before", nullptr, entry);
    TEST_ASSERT(insert_before->impl_block() != nullptr, "insert-before block created");
    TEST_ASSERT(insert_before->getParent() == main_fn, "insert-before parent");

    return 0;
}

static int test_block_parent_recovery_from_ir_func_link() {
    llvm::LLVMContext ctx;
    llvm::Module mod("parent_recovery", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i32, false);
    llvm::Function *fn = mod.createFunction("parent_fn", ft, false);
    TEST_ASSERT(fn != nullptr, "function created");

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    TEST_ASSERT(entry->impl_block() != nullptr, "entry block created");

    llvm::detail::unregister_blocks_for_function(fn);
    TEST_ASSERT(entry->getParent() == fn, "parent recovered via block->func");

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPoint(entry);

    llvm::BasicBlock *insert_block = builder.GetInsertBlock();
    TEST_ASSERT(insert_block != nullptr, "insert block present");
    TEST_ASSERT(insert_block->getParent() == fn, "insert block parent recovered");

    builder.CreateRet(llvm::ConstantInt::get(i32, 0));
    return 0;
}

static int test_builder_syncs_module_from_insert_block() {
    llvm::LLVMContext ctx;
    llvm::IRBuilder<> builder(ctx);
    llvm::Module mod("builder_sync", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i8p = llvm::Type::getInt8PtrTy(ctx);

    llvm::Type *foo_params[] = {i8p};
    llvm::FunctionType *foo_ty = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), foo_params, false);
    llvm::Function *foo = llvm::Function::Create(
        foo_ty, llvm::GlobalValue::ExternalLinkage, "foo", mod);
    TEST_ASSERT(foo != nullptr, "foo decl");

    llvm::FunctionType *main_ty = llvm::FunctionType::get(i32, false);
    llvm::Function *main_fn = llvm::Function::Create(
        main_ty, llvm::GlobalValue::ExternalLinkage, "main", mod);
    TEST_ASSERT(main_fn != nullptr, "main function");
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", main_fn);
    TEST_ASSERT(entry->impl_block() != nullptr, "entry block");

    builder.SetInsertPoint(entry);
    llvm::Constant *str = builder.CreateGlobalStringPtr("I4", "serialization_info");
    TEST_ASSERT(str != nullptr, "global string");
    llvm::Value *foo_args[] = {str};
    builder.CreateCall(foo, llvm::ArrayRef<llvm::Value *>(foo_args, 1));
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));

    size_t ir_len = 0;
    char *ir = lc_module_sprint(mod.getCompat(), &ir_len);
    TEST_ASSERT(ir != nullptr, "module sprint");
    TEST_ASSERT(std::strstr(ir, "@serialization_info") != nullptr,
                "string global symbol present");
    TEST_ASSERT(std::strstr(ir, "call void @foo(ptr @serialization_info)") != nullptr,
                "call uses string symbol");
    free(ir);
    return 0;
}

static int test_irbuilder_arithmetic() {
    llvm::LLVMContext ctx;
    llvm::Module mod("arith", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_add", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    TEST_ASSERT(a != nullptr, "arg 0");
    TEST_ASSERT(b != nullptr, "arg 1");

    llvm::Value *sum = builder.CreateAdd(a, b, "sum");
    TEST_ASSERT(sum != nullptr, "add created");

    llvm::Value *diff = builder.CreateSub(sum, b, "diff");
    TEST_ASSERT(diff != nullptr, "sub created");

    llvm::Value *prod = builder.CreateMul(a, b, "prod");
    TEST_ASSERT(prod != nullptr, "mul created");

    builder.CreateRet(sum);

    return 0;
}

static int test_irbuilder_control_flow() {
    llvm::LLVMContext ctx;
    llvm::Module mod("cf", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_branch", ft, false);

    llvm::BasicBlock *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::BasicBlock *then_bb = llvm::BasicBlock::Create(ctx, "then", fn);
    llvm::BasicBlock *else_bb = llvm::BasicBlock::Create(ctx, "else", fn);
    llvm::BasicBlock *merge = llvm::BasicBlock::Create(ctx, "merge", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);

    builder.SetInsertPoint(entry);
    llvm::Value *arg = fn->getArg(0);
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value *cmp = builder.CreateICmpEQ(arg, zero, "cmp");
    builder.CreateCondBr(cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    llvm::Value *v1 = llvm::ConstantInt::get(i64, 1);
    builder.CreateBr(merge);

    builder.SetInsertPoint(else_bb);
    llvm::Value *v2 = llvm::ConstantInt::get(i64, 2);
    builder.CreateBr(merge);

    builder.SetInsertPoint(merge);
    llvm::PHINode *phi = builder.CreatePHI(i64, 2, "result");
    TEST_ASSERT(phi != nullptr, "phi created");
    phi->addIncoming(v1, then_bb);
    phi->addIncoming(v2, else_bb);
    phi->finalize();

    builder.CreateRet(phi);

    return 0;
}

static int test_irbuilder_memory() {
    llvm::LLVMContext ctx;
    llvm::Module mod("mem", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, false);

    llvm::Function *fn = mod.createFunction("test_mem", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::AllocaInst *alloca = builder.CreateAlloca(i64, nullptr, "x");
    TEST_ASSERT(alloca != nullptr, "alloca created");
    TEST_ASSERT(alloca->getAllocatedType() != nullptr, "alloca type set");

    llvm::Value *val = llvm::ConstantInt::get(i64, 42);
    builder.CreateStore(val, alloca);

    llvm::Value *loaded = builder.CreateLoad(i64, alloca, "loaded");
    TEST_ASSERT(loaded != nullptr, "load created");

    builder.CreateRet(loaded);
    return 0;
}

static int test_irbuilder_casts() {
    llvm::LLVMContext ctx;
    llvm::Module mod("casts", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_casts", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *arg = fn->getArg(0);
    llvm::Value *ext = builder.CreateSExt(arg, i64, "sext");
    TEST_ASSERT(ext != nullptr, "sext");

    llvm::Value *zext = builder.CreateZExt(arg, i64, "zext");
    TEST_ASSERT(zext != nullptr, "zext");

    llvm::Value *trunc = builder.CreateTrunc(ext, i32, "trunc");
    TEST_ASSERT(trunc != nullptr, "trunc");

    llvm::Value *fp = builder.CreateSIToFP(arg, dblTy, "sitofp");
    TEST_ASSERT(fp != nullptr, "sitofp");

    llvm::Value *intval = builder.CreateFPToSI(fp, i64, "fptosi");
    TEST_ASSERT(intval != nullptr, "fptosi");

    llvm::Value *ptr = builder.CreateIntToPtr(ext, ptrTy, "inttoptr");
    TEST_ASSERT(ptr != nullptr, "inttoptr");

    llvm::Value *back = builder.CreatePtrToInt(ptr, i64, "ptrtoint");
    TEST_ASSERT(back != nullptr, "ptrtoint");

    llvm::Value *sor = builder.CreateSExtOrTrunc(arg, i64, "sortrunc");
    TEST_ASSERT(sor != nullptr, "sextortrunc");

    builder.CreateRet(ext);
    return 0;
}

static int test_irbuilder_fp_ops() {
    llvm::LLVMContext ctx;
    llvm::Module mod("fpops", ctx);

    llvm::Type *dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type *params[] = {dblTy, dblTy};
    llvm::FunctionType *ft = llvm::FunctionType::get(dblTy, params, false);

    llvm::Function *fn = mod.createFunction("test_fp", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);

    llvm::Value *fadd = builder.CreateFAdd(a, b, "fadd");
    TEST_ASSERT(fadd != nullptr, "fadd");

    llvm::Value *fsub = builder.CreateFSub(a, b, "fsub");
    TEST_ASSERT(fsub != nullptr, "fsub");

    llvm::Value *fmul = builder.CreateFMul(a, b, "fmul");
    TEST_ASSERT(fmul != nullptr, "fmul");

    llvm::Value *fdiv = builder.CreateFDiv(a, b, "fdiv");
    TEST_ASSERT(fdiv != nullptr, "fdiv");

    llvm::Value *fneg = builder.CreateFNeg(a, "fneg");
    TEST_ASSERT(fneg != nullptr, "fneg");

    llvm::Value *cmp = builder.CreateFCmpOLT(a, b, "fcmp");
    TEST_ASSERT(cmp != nullptr, "fcmp_olt");

    builder.CreateRet(fadd);
    return 0;
}

static int test_casting_helpers() {
    llvm::LLVMContext ctx;
    llvm::Module mod("casting", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::ConstantInt *ci = llvm::ConstantInt::get(i32, 7);
    llvm::Value *v = ci;

    llvm::ConstantInt *back = llvm::dyn_cast<llvm::ConstantInt>(v);
    TEST_ASSERT(back != nullptr, "dyn_cast non-null");

    llvm::ConstantInt *casted = llvm::cast<llvm::ConstantInt>(v);
    TEST_ASSERT(casted != nullptr, "cast non-null");

    TEST_ASSERT(llvm::isa<llvm::Value>(v), "isa Value");
    TEST_ASSERT(llvm::isa<llvm::ConstantInt>(v), "isa ConstantInt");

    llvm::Value *null_val = nullptr;
    llvm::ConstantInt *null_cast = llvm::dyn_cast_or_null<llvm::ConstantInt>(null_val);
    TEST_ASSERT(null_cast == nullptr, "dyn_cast_or_null nullptr");

    return 0;
}

static int test_apint_apfloat() {
    llvm::APInt a(32, 42);
    TEST_ASSERT_EQ(a.getBitWidth(), 32, "apint width");
    TEST_ASSERT_EQ(a.getZExtValue(), 42, "apint value");

    llvm::APInt neg(32, static_cast<uint64_t>(-1), true);
    TEST_ASSERT_EQ(neg.getSExtValue(), -1, "apint signed");

    llvm::APFloat f(3.14);
    double d = f.convertToDouble();
    TEST_ASSERT(d > 3.13 && d < 3.15, "apfloat value");

    return 0;
}

static int test_data_layout() {
    llvm::LLVMContext ctx;
    llvm::Module mod("dl", ctx);

    const llvm::DataLayout &dl = mod.getDataLayout();
    TEST_ASSERT_EQ(dl.getPointerSize(), 8, "ptr size");
    TEST_ASSERT_EQ(dl.getPointerSizeInBits(), 64, "ptr bits");

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    TEST_ASSERT(dl.getTypeAllocSize(i32) > 0, "i32 alloc size > 0");

    return 0;
}

static int test_noop_passes() {
    llvm::LLVMContext ctx;
    llvm::Module mod("passes", ctx);

    llvm::PassBuilder pb;
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    return 0;
}

static int test_noop_verifier() {
    llvm::LLVMContext ctx;
    llvm::Module mod("verify", ctx);

    bool broken = llvm::verifyModule(mod);
    TEST_ASSERT(!broken, "module verifies");

    return 0;
}

static int test_noop_di_builder() {
    llvm::LLVMContext ctx;
    llvm::Module mod("debug", ctx);

    llvm::DIBuilder dib(mod);
    dib.finalize();

    return 0;
}

static int test_stringref_twine() {
    llvm::StringRef sr("hello");
    TEST_ASSERT_EQ(sr.size(), 5, "stringref size");
    TEST_ASSERT(sr == "hello", "stringref eq");
    TEST_ASSERT(sr != "world", "stringref ne");

    llvm::Twine tw("hello");
    std::string s = tw.str();
    TEST_ASSERT(s == "hello", "twine str");

    return 0;
}

static int test_twine_abi_layout_and_concat() {
    size_t expected_size = sizeof(void *) == 8 ? 40u : 20u;
    TEST_ASSERT_EQ(sizeof(llvm::Twine), expected_size, "twine ABI size");
    TEST_ASSERT_EQ(alignof(llvm::Twine), alignof(void *), "twine ABI alignment");

    llvm::Twine lhs("ab");
    llvm::Twine rhs("cd");
    std::string joined = (lhs + rhs).str();
    TEST_ASSERT(joined == "abcd", "twine concat");

    llvm::Twine null_twine = llvm::Twine::createNull();
    TEST_ASSERT(null_twine.str().empty(), "null twine renders empty");

    return 0;
}

static int test_arrayref() {
    int arr[] = {1, 2, 3};
    llvm::ArrayRef<int> ref(arr, 3);
    TEST_ASSERT_EQ(ref.size(), 3, "arrayref size");
    TEST_ASSERT_EQ(ref[0], 1, "arrayref idx 0");
    TEST_ASSERT_EQ(ref[2], 3, "arrayref idx 2");
    return 0;
}

static int test_irbuilder_gep_and_struct_gep() {
    llvm::LLVMContext ctx;
    llvm::Module mod("gep", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Type *params[] = {ptrTy};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_gep", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *ptr = fn->getArg(0);

    llvm::Type *fields[] = {i32, i64};
    llvm::StructType *st = llvm::StructType::get(ctx, fields);

    llvm::Value *sgep = builder.CreateStructGEP(st, ptr, 1, "sgep");
    TEST_ASSERT(sgep != nullptr, "struct gep");

    llvm::Value *idx = llvm::ConstantInt::get(i64, 0);
    llvm::Value *gep = builder.CreateGEP(i64, ptr, idx, "gep");
    TEST_ASSERT(gep != nullptr, "gep");

    llvm::Value *igep = builder.CreateInBoundsGEP(i64, ptr, idx, "igep");
    TEST_ASSERT(igep != nullptr, "inbounds gep");

    llvm::Value *loaded = builder.CreateLoad(i64, sgep, "val");
    builder.CreateRet(loaded);

    return 0;
}

static int test_irbuilder_select() {
    llvm::LLVMContext ctx;
    llvm::Module mod("select", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_sel", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    llvm::Value *cond = builder.CreateICmpSGT(a, zero, "cond");
    llvm::Value *sel = builder.CreateSelect(cond, a, b, "sel");
    TEST_ASSERT(sel != nullptr, "select created");

    builder.CreateRet(sel);
    return 0;
}

static int test_irbuilder_call() {
    llvm::LLVMContext ctx;
    llvm::Module mod("call", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *add_fn = mod.createFunction("add", ft, false);
    {
        llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", add_fn);
        llvm::IRBuilder<> b(ctx);
        b.SetModule(mod.getCompat());
        b.SetInsertPointForFunction(add_fn);
        b.SetInsertPoint(bb);
        llvm::Value *sum = b.CreateAdd(add_fn->getArg(0),
                                       add_fn->getArg(1));
        b.CreateRet(sum);
    }

    llvm::Function *caller = mod.createFunction("caller", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", caller);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(caller);
    builder.SetInsertPoint(bb);

    llvm::Value *a = caller->getArg(0);
    llvm::Value *b_arg = caller->getArg(1);
    llvm::Value *args[] = {a, b_arg};
    llvm::Value *result = builder.CreateCall(ft,
        llvm::Value::wrap(add_fn->getFuncVal()), args, "result");
    TEST_ASSERT(result != nullptr, "call result");
    builder.CreateRet(result);

    return 0;
}

static int test_irbuilder_extractvalue_insertvalue() {
    llvm::LLVMContext ctx;
    llvm::Module mod("aggr", ctx);

    llvm::Type *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *fields[] = {i32, i64};
    llvm::StructType *st = llvm::StructType::get(ctx, fields);

    llvm::Type *params[] = {i32};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_ev", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *undef = llvm::UndefValue::get(st);
    llvm::Value *arg = fn->getArg(0);
    unsigned idx0[] = {0};
    llvm::Value *inserted = builder.CreateInsertValue(undef, arg, idx0, "ins");
    TEST_ASSERT(inserted != nullptr, "insertvalue");

    unsigned idx1[] = {1};
    llvm::Value *extracted = builder.CreateExtractValue(inserted, idx1, "ext");
    TEST_ASSERT(extracted != nullptr, "extractvalue");

    llvm::Value *ret = builder.CreateSExt(arg, i64);
    builder.CreateRet(ret);

    return 0;
}

static int test_irbuilder_bitwise() {
    llvm::LLVMContext ctx;
    llvm::Module mod("bits", ctx);

    llvm::Type *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *params[] = {i64, i64};
    llvm::FunctionType *ft = llvm::FunctionType::get(i64, params, false);

    llvm::Function *fn = mod.createFunction("test_bits", ft, false);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(ctx, "entry", fn);

    llvm::IRBuilder<> builder(ctx);
    builder.SetModule(mod.getCompat());
    builder.SetInsertPointForFunction(fn);
    builder.SetInsertPoint(bb);

    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);

    llvm::Value *v_and = builder.CreateAnd(a, b, "and");
    TEST_ASSERT(v_and != nullptr, "and");

    llvm::Value *v_or = builder.CreateOr(a, b, "or");
    TEST_ASSERT(v_or != nullptr, "or");

    llvm::Value *v_xor = builder.CreateXor(a, b, "xor");
    TEST_ASSERT(v_xor != nullptr, "xor");

    llvm::Value *v_shl = builder.CreateShl(a, b, "shl");
    TEST_ASSERT(v_shl != nullptr, "shl");

    llvm::Value *v_lshr = builder.CreateLShr(a, b, "lshr");
    TEST_ASSERT(v_lshr != nullptr, "lshr");

    llvm::Value *v_ashr = builder.CreateAShr(a, b, "ashr");
    TEST_ASSERT(v_ashr != nullptr, "ashr");

    llvm::Value *v_not = builder.CreateNot(a, "not");
    TEST_ASSERT(v_not != nullptr, "not");

    builder.CreateRet(v_and);
    return 0;
}

static int test_target_select_noop() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    return 0;
}

static int test_jit_smoke_ret_42() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_smoke", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    auto *fty = llvm::FunctionType::get(i32, false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "ret42", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRet(llvm::ConstantInt::get(i32, 42));

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("ret42");
    TEST_ASSERT(fp != nullptr, "lookup ret42");
    int result = fp();
    TEST_ASSERT_EQ(result, 42, "ret42() == 42");
    return 0;
}

static int test_jit_smoke_add_args() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_add", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32, i32};
    auto *fty = llvm::FunctionType::get(i32, llvm::ArrayRef<llvm::Type *>(params, 2), false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "add_args", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    llvm::Value *a = fn->getArg(0);
    llvm::Value *b = fn->getArg(1);
    llvm::Value *sum = builder.CreateAdd(a, b, "sum");
    builder.CreateRet(sum);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int, int);
    fn_t fp = (fn_t)jit.lookup("add_args");
    TEST_ASSERT(fp != nullptr, "lookup add_args");
    int result = fp(17, 25);
    TEST_ASSERT_EQ(result, 42, "add_args(17,25) == 42");
    return 0;
}

static int test_jit_smoke_branch() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_branch", ctx);
    auto *i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Type *params[] = {i32};
    auto *fty = llvm::FunctionType::get(i32, llvm::ArrayRef<llvm::Type *>(params, 1), false);
    auto *fn = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                       "abs_val", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    auto *then_bb = llvm::BasicBlock::Create(ctx, "then", fn);
    auto *else_bb = llvm::BasicBlock::Create(ctx, "else", fn);
    auto *merge_bb = llvm::BasicBlock::Create(ctx, "merge", fn);

    llvm::IRBuilder<> builder(entry);
    llvm::Value *x = fn->getArg(0);
    llvm::Value *zero = llvm::ConstantInt::get(i32, 0);
    llvm::Value *cmp = builder.CreateICmpSLT(x, zero, "neg");
    builder.CreateCondBr(cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);
    llvm::Value *negx = builder.CreateSub(zero, x, "negx");
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(else_bb);
    builder.CreateBr(merge_bb);

    builder.SetInsertPoint(merge_bb);
    auto *phi = builder.CreatePHI(i32, 2, "result");
    phi->addIncoming(negx, then_bb);
    phi->addIncoming(x, else_bb);
    builder.CreateRet(phi);

    llvm::orc::LLJIT jit;
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef int (*fn_t)(int);
    fn_t fp = (fn_t)jit.lookup("abs_val");
    TEST_ASSERT(fp != nullptr, "lookup abs_val");
    TEST_ASSERT_EQ(fp(5), 5, "abs_val(5) == 5");
    TEST_ASSERT_EQ(fp(-7), 7, "abs_val(-7) == 7");
    TEST_ASSERT_EQ(fp(0), 0, "abs_val(0) == 0");
    return 0;
}

static int test_jit_smoke_indirect_bitcast_external_fp_call() {
    llvm::LLVMContext ctx;
    llvm::Module mod("jit_indirect_ext_fp", ctx);
    auto *dbl = llvm::Type::getDoubleTy(ctx);
    auto *i64 = llvm::Type::getInt64Ty(ctx);
    llvm::Type *sin_params[] = {dbl};
    auto *sin_ty = llvm::FunctionType::get(
        dbl, llvm::ArrayRef<llvm::Type *>(sin_params, 1), false);
    auto *sin_decl = llvm::Function::Create(
        sin_ty, llvm::GlobalValue::ExternalLinkage, "sin", mod);
    TEST_ASSERT(sin_decl != nullptr, "sin declaration");

    auto *caller_ty = llvm::FunctionType::get(i64, false);
    auto *caller = llvm::Function::Create(
        caller_ty, llvm::GlobalValue::ExternalLinkage, "call_sin_bitcast", mod);
    auto *entry = llvm::BasicBlock::Create(ctx, "entry", caller);
    llvm::IRBuilder<> builder(entry);

    llvm::Value *callee = builder.CreateBitCast(
        sin_decl, llvm::Type::getInt8PtrTy(ctx), "sin_ptr");
    TEST_ASSERT(callee != nullptr, "bitcasted callee");
    llvm::Value *arg = llvm::ConstantFP::get(dbl, 1.5707963267948966);
    llvm::Value *call_args[] = {arg};
    llvm::Value *s = builder.CreateCall(
        sin_ty, callee, llvm::ArrayRef<llvm::Value *>(call_args, 1), "s");
    TEST_ASSERT(s != nullptr, "sin call");
    llvm::Value *scaled = builder.CreateFMul(
        s, llvm::ConstantFP::get(dbl, 1000.0), "scaled");
    llvm::Value *as_i64 = builder.CreateFPToSI(scaled, i64, "as_i64");
    builder.CreateRet(as_i64);

    llvm::orc::LLJIT jit;
    double (*sin_fn)(double) = std::sin;
    void *sin_addr = nullptr;
    memcpy(&sin_addr, &sin_fn, sizeof(sin_addr));
    jit.addSymbol("sin", sin_addr);
    int rc = jit.addModule(mod);
    TEST_ASSERT_EQ(rc, 0, "addModule");
    typedef long long (*fn_t)(void);
    fn_t fp = (fn_t)jit.lookup("call_sin_bitcast");
    TEST_ASSERT(fp != nullptr, "lookup call_sin_bitcast");
    long long result = fp();
    TEST_ASSERT(result >= 999 && result <= 1001,
                "bitcasted external FP call uses correct ABI");
    return 0;
}

int main() {
    fprintf(stderr, "LLVM C++ compat test suite\n");
    fprintf(stderr, "==========================\n\n");

    fprintf(stderr, "Infrastructure tests:\n");
    RUN_TEST(test_llvm_version);
    RUN_TEST(test_stringref_twine);
    RUN_TEST(test_twine_abi_layout_and_concat);
    RUN_TEST(test_arrayref);
    RUN_TEST(test_apint_apfloat);
    RUN_TEST(test_casting_helpers);
    RUN_TEST(test_target_select_noop);

    fprintf(stderr, "\nModule and Type tests:\n");
    RUN_TEST(test_context_and_module);
    RUN_TEST(test_basic_types);
    RUN_TEST(test_type_context_stability_across_nested_modules);
    RUN_TEST(test_function_type);
    RUN_TEST(test_struct_type);
    RUN_TEST(test_array_type);
    RUN_TEST(test_data_layout);

    fprintf(stderr, "\nConstant tests:\n");
    RUN_TEST(test_constants);

    fprintf(stderr, "\nFunction tests:\n");
    RUN_TEST(test_function_creation);
    RUN_TEST(test_block_parent_tracking_across_decls);
    RUN_TEST(test_block_parent_recovery_from_ir_func_link);
    RUN_TEST(test_builder_syncs_module_from_insert_block);

    fprintf(stderr, "\nIRBuilder tests:\n");
    RUN_TEST(test_irbuilder_arithmetic);
    RUN_TEST(test_irbuilder_control_flow);
    RUN_TEST(test_irbuilder_memory);
    RUN_TEST(test_irbuilder_casts);
    RUN_TEST(test_irbuilder_fp_ops);
    RUN_TEST(test_irbuilder_gep_and_struct_gep);
    RUN_TEST(test_irbuilder_select);
    RUN_TEST(test_irbuilder_call);
    RUN_TEST(test_irbuilder_extractvalue_insertvalue);
    RUN_TEST(test_irbuilder_bitwise);

    fprintf(stderr, "\nNo-op verification tests:\n");
    RUN_TEST(test_noop_passes);
    RUN_TEST(test_noop_verifier);
    RUN_TEST(test_noop_di_builder);

    fprintf(stderr, "\nJIT smoke tests:\n");
    RUN_TEST(test_jit_smoke_ret_42);
    RUN_TEST(test_jit_smoke_add_args);
    RUN_TEST(test_jit_smoke_branch);
    RUN_TEST(test_jit_smoke_indirect_bitcast_external_fp_call);

    fprintf(stderr, "\n==========================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
