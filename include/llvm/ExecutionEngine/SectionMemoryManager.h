#ifndef LLVM_EXECUTIONENGINE_SECTIONMEMORYMANAGER_H
#define LLVM_EXECUTIONENGINE_SECTIONMEMORYMANAGER_H

namespace llvm {

class SectionMemoryManager {
public:
    virtual ~SectionMemoryManager() = default;
};

} // namespace llvm

#endif
