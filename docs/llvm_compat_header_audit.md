# LLVM Compat Header Audit

Date: 2026-02-24

- Audited headers: 94
- Headers with non-trivial logic markers: 57

## Migrated to C compat backend in this pass
- include/llvm/IR/LLVMContext.h (registry maps delegated to C)
- include/llvm/IR/BasicBlock.h (erase/move algorithms delegated to C)
- include/llvm/IR/Function.h (block-list helpers delegated to C)
- include/llvm/IR/GlobalValue.h (state registry delegated to C)
- include/llvm/IR/Module.h (linkage+type-name helpers delegated to C)
- include/llvm/IR/IRBuilder.h (intrinsic name resolution delegated to C)
- include/llvm/ADT/StringRef.h (strlen/memcmp delegated to C)
- include/llvm/ADT/Twine.h (reduced to thin value wrapper, formatting via C)
- include/llvm/Object/ObjectFile.h
- include/llvm/Object/Binary.h
- include/llvm/DebugInfo/Symbolize/Symbolize.h
- include/llvm/DebugInfo/DWARF/DWARFContext.h

## Header-by-header inventory

| Header | Logic Hits | Status |
|---|---:|---|
| `include/llvm/ADT/APFloat.h` | 6 | contains inline/template logic |
| `include/llvm/ADT/APInt.h` | 0 | thin/declaration-only |
| `include/llvm/ADT/ArrayRef.h` | 5 | contains inline/template logic |
| `include/llvm/ADT/SmallVector.h` | 2 | contains inline/template logic |
| `include/llvm/ADT/STLExtras.h` | 3 | contains inline/template logic |
| `include/llvm/ADT/StringRef.h` | 6 | delegates core logic to C compat backend |
| `include/llvm/ADT/Twine.h` | 14 | delegates core logic to C compat backend |
| `include/llvm/Analysis/Passes.h` | 0 | thin/declaration-only |
| `include/llvm/Analysis/TargetLibraryInfo.h` | 1 | contains inline/template logic |
| `include/llvm/Analysis/TargetTransformInfo.h` | 0 | thin/declaration-only |
| `include/llvm/AsmParser/Parser.h` | 9 | contains inline/template logic |
| `include/llvm/Config/AsmParsers.def` | 0 | thin/declaration-only |
| `include/llvm/Config/AsmPrinters.def` | 0 | thin/declaration-only |
| `include/llvm/Config/Disassemblers.def` | 0 | thin/declaration-only |
| `include/llvm/Config/llvm-config.h` | 0 | thin/declaration-only |
| `include/llvm/Config/TargetMCAs.def` | 0 | thin/declaration-only |
| `include/llvm/Config/Targets.def` | 0 | thin/declaration-only |
| `include/llvm/DebugInfo/DWARF/DWARFContext.h` | 15 | delegates core logic to C compat backend |
| `include/llvm/DebugInfo/Symbolize/Symbolize.h` | 3 | delegates core logic to C compat backend |
| `include/llvm/ExecutionEngine/ExecutionEngine.h` | 3 | contains inline/template logic |
| `include/llvm/ExecutionEngine/GenericValue.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/JITSymbol.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/MCJIT.h` | 1 | contains inline/template logic |
| `include/llvm/ExecutionEngine/ObjectCache.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/Orc/CompileUtils.h` | 4 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/Core.h` | 12 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/CoreContainers.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/Orc/ExecutionUtils.h` | 6 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/ExecutorProcessControl.h` | 2 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/IRCompileLayer.h` | 6 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h` | 3 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/Layer.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/Orc/LLJIT.h` | 4 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/Mangling.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h` | 5 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h` | 5 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h` | 1 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h` | 0 | thin/declaration-only |
| `include/llvm/ExecutionEngine/Orc/SymbolStringPool.h` | 3 | contains inline/template logic |
| `include/llvm/ExecutionEngine/Orc/ThreadSafeModule.h` | 14 | contains inline/template logic |
| `include/llvm/ExecutionEngine/SectionMemoryManager.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Argument.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Attributes.h` | 0 | thin/declaration-only |
| `include/llvm/IR/BasicBlock.h` | 0 | delegates core logic to C compat backend |
| `include/llvm/IR/CallingConv.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Constants.h` | 10 | contains inline/template logic |
| `include/llvm/IR/DataLayout.h` | 6 | contains inline/template logic |
| `include/llvm/IR/DerivedTypes.h` | 1 | contains inline/template logic |
| `include/llvm/IR/DIBuilder.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Function.h` | 2 | delegates core logic to C compat backend |
| `include/llvm/IR/GlobalValue.h` | 1 | delegates core logic to C compat backend |
| `include/llvm/IR/GlobalVariable.h` | 1 | contains inline/template logic |
| `include/llvm/IR/InstrTypes.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Instructions.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Intrinsics.h` | 0 | thin/declaration-only |
| `include/llvm/IR/IRBuilder.h` | 24 | delegates core logic to C compat backend |
| `include/llvm/IR/LegacyPassManager.h` | 0 | thin/declaration-only |
| `include/llvm/IR/LLVMContext.h` | 28 | delegates core logic to C compat backend |
| `include/llvm/IR/Module.h` | 69 | delegates core logic to C compat backend |
| `include/llvm/IR/Type.h` | 1 | contains inline/template logic |
| `include/llvm/IR/Value.h` | 0 | thin/declaration-only |
| `include/llvm/IR/Verifier.h` | 2 | contains inline/template logic |
| `include/llvm/MC/TargetRegistry.h` | 5 | contains inline/template logic |
| `include/llvm/Object/Binary.h` | 4 | delegates core logic to C compat backend |
| `include/llvm/Object/ELFObjectFile.h` | 0 | thin/declaration-only |
| `include/llvm/Object/ObjectFile.h` | 14 | delegates core logic to C compat backend |
| `include/llvm/Passes/OptimizationLevel.h` | 6 | contains inline/template logic |
| `include/llvm/Passes/PassBuilder.h` | 6 | contains inline/template logic |
| `include/llvm/Support/Alignment.h` | 0 | thin/declaration-only |
| `include/llvm/Support/Casting.h` | 25 | contains inline/template logic |
| `include/llvm/Support/CodeGen.h` | 0 | thin/declaration-only |
| `include/llvm/Support/CommandLine.h` | 1 | contains inline/template logic |
| `include/llvm/Support/Compiler.h` | 0 | thin/declaration-only |
| `include/llvm/Support/Error.h` | 33 | contains inline/template logic |
| `include/llvm/Support/ErrorHandling.h` | 2 | contains inline/template logic |
| `include/llvm/Support/FileSystem.h` | 0 | thin/declaration-only |
| `include/llvm/Support/ManagedStatic.h` | 2 | contains inline/template logic |
| `include/llvm/Support/MemoryBuffer.h` | 8 | contains inline/template logic |
| `include/llvm/Support/Path.h` | 6 | contains inline/template logic |
| `include/llvm/Support/raw_ostream.h` | 17 | contains inline/template logic |
| `include/llvm/Support/SourceMgr.h` | 3 | contains inline/template logic |
| `include/llvm/Support/TargetSelect.h` | 8 | contains inline/template logic |
| `include/llvm/Target/TargetMachine.h` | 3 | contains inline/template logic |
| `include/llvm/Target/TargetOptions.h` | 0 | thin/declaration-only |
| `include/llvm/TargetParser/Triple.h` | 8 | contains inline/template logic |
| `include/llvm/Transforms/InstCombine/InstCombine.h` | 1 | contains inline/template logic |
| `include/llvm/Transforms/Instrumentation/AddressSanitizer.h` | 0 | thin/declaration-only |
| `include/llvm/Transforms/Instrumentation/ThreadSanitizer.h` | 0 | thin/declaration-only |
| `include/llvm/Transforms/IPO.h` | 2 | contains inline/template logic |
| `include/llvm/Transforms/IPO/AlwaysInliner.h` | 1 | contains inline/template logic |
| `include/llvm/Transforms/Scalar.h` | 0 | thin/declaration-only |
| `include/llvm/Transforms/Scalar/GVN.h` | 1 | contains inline/template logic |
| `include/llvm/Transforms/Scalar/InstSimplifyPass.h` | 0 | thin/declaration-only |
| `include/llvm/Transforms/Utils/Mem2Reg.h` | 1 | contains inline/template logic |

## Remaining high-impact headers for further migration
- include/llvm/IR/Constants.h
- include/llvm/Support/raw_ostream.h
- include/llvm/Support/Error.h
- include/llvm/Support/Casting.h
