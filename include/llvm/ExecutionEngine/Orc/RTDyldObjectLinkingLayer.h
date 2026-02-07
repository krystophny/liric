#ifndef LLVM_EXECUTIONENGINE_ORC_RTDYLDOBJECTLINKINGLAYER_H
#define LLVM_EXECUTIONENGINE_ORC_RTDYLDOBJECTLINKINGLAYER_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include "llvm/Support/MemoryBuffer.h"
#include <functional>
#include <memory>
#include <type_traits>

namespace llvm {

class SectionMemoryManager;

namespace orc {

class RTDyldObjectLinkingLayer : public ObjectLayer {
public:
    RTDyldObjectLinkingLayer(
        ExecutionSession &ES,
        std::function<std::unique_ptr<SectionMemoryManager>()> GetMemMgr) {
        (void)ES;
        (void)GetMemMgr;
    }

    template <typename F,
              typename = std::enable_if_t<
                  std::is_invocable_r_v<
                      std::unique_ptr<SectionMemoryManager>,
                      F, const MemoryBuffer &>>>
    RTDyldObjectLinkingLayer(ExecutionSession &ES, F &&GetMemMgr) {
        (void)ES;
        (void)GetMemMgr;
    }

    RTDyldObjectLinkingLayer(ExecutionSession &ES) { (void)ES; }

    void setOverrideObjectFlagsWithResponsibilityFlags(bool) {}
    void setAutoClaimResponsibilityForObjectSymbols(bool) {}
};

} // namespace orc
} // namespace llvm

#endif
