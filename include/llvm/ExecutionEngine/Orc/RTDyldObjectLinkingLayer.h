#ifndef LLVM_EXECUTIONENGINE_ORC_RTDYLDOBJECTLINKINGLAYER_H
#define LLVM_EXECUTIONENGINE_ORC_RTDYLDOBJECTLINKINGLAYER_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include <functional>
#include <memory>

namespace llvm {

class SectionMemoryManager;

namespace orc {

class RTDyldObjectLinkingLayer : public ObjectLayer {
public:
    RTDyldObjectLinkingLayer(
        ExecutionSession &ES,
        std::function<std::unique_ptr<SectionMemoryManager>()> GetMemMgr = {}) {
        (void)ES;
        (void)GetMemMgr;
    }

    void setOverrideObjectFlagsWithResponsibilityFlags(bool) {}
    void setAutoClaimResponsibilityForObjectSymbols(bool) {}
};

} // namespace orc
} // namespace llvm

#endif
