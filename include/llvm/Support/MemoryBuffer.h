#ifndef LLVM_SUPPORT_MEMORYBUFFER_H
#define LLVM_SUPPORT_MEMORYBUFFER_H

#include "llvm/ADT/StringRef.h"
#include <memory>
#include <string>
#include <system_error>

namespace llvm {

class MemoryBuffer {
    std::string data_;
    std::string name_;

public:
    MemoryBuffer(std::string data, std::string name)
        : data_(std::move(data)), name_(std::move(name)) {}

    StringRef getBuffer() const { return data_; }
    StringRef getBufferIdentifier() const { return name_; }
    const char *getBufferStart() const { return data_.data(); }
    const char *getBufferEnd() const { return data_.data() + data_.size(); }
    size_t getBufferSize() const { return data_.size(); }

    static std::unique_ptr<MemoryBuffer> getMemBuffer(StringRef InputData,
                                                       StringRef BufferName = "") {
        return std::make_unique<MemoryBuffer>(InputData.str(), BufferName.str());
    }

    static std::unique_ptr<MemoryBuffer> getMemBufferCopy(StringRef InputData,
                                                           StringRef BufferName = "") {
        return std::make_unique<MemoryBuffer>(InputData.str(), BufferName.str());
    }
};

} // namespace llvm

#endif
