#ifndef LLVM_OBJECT_BINARY_H
#define LLVM_OBJECT_BINARY_H

#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include <string>

namespace llvm {
namespace object {

inline Expected<OwningBinary<Binary>> createBinary(const std::string &Path) {
    auto ObjOrErr = ObjectFile::createObjectFile(Path);
    if (!ObjOrErr)
        return ObjOrErr.takeError();
    OwningBinary<ObjectFile> Obj = std::move(*ObjOrErr);
    std::unique_ptr<Binary> Bin = std::move(Obj.takeBinary());
    return OwningBinary<Binary>(std::move(Bin));
}

} // namespace object
} // namespace llvm

#endif
