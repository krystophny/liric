#ifndef LLVM_EXECUTIONENGINE_OBJECTCACHE_H
#define LLVM_EXECUTIONENGINE_OBJECTCACHE_H

#include <memory>

namespace llvm {

class MemoryBuffer;
class Module;

class ObjectCache {
public:
    virtual ~ObjectCache() = default;
};

} // namespace llvm

#endif
