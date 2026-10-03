#ifndef LLVM_MC_MCSUBTARGETINFO_H
#define LLVM_MC_MCSUBTARGETINFO_H
#include "llvm/ADT/StringRef.h"
namespace liric_llvm {
class MCSubtargetInfo {
public:
    bool isCPUStringValid(StringRef cpu) const {
        return cpu.str() == "generic";
    }
};
}
#endif
