#ifndef LLVM_SUPPORT_FILESYSTEM_H
#define LLVM_SUPPORT_FILESYSTEM_H

namespace llvm {
namespace sys {
namespace fs {

enum OpenFlags : unsigned {
    OF_None = 0,
    OF_Text = 1,
    OF_Append = 2,
    OF_Delete = 4,
};

} // namespace fs
} // namespace sys
} // namespace llvm

#endif
