#ifndef LLVM_TARGETPARSER_TRIPLE_H
#define LLVM_TARGETPARSER_TRIPLE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include <string>

namespace llvm {

class Triple {
    std::string Data;

public:
    enum ArchType {
        UnknownArch,
        aarch64,
        x86_64,
    };

    enum OSType {
        UnknownOS,
        Darwin,
        Linux,
    };

    enum ObjectFormatType {
        UnknownObjectFormat,
        ELF,
        MachO,
    };

    Triple() {
#ifdef LLVM_DEFAULT_TARGET_TRIPLE
        Data = LLVM_DEFAULT_TARGET_TRIPLE;
#endif
    }

    Triple(StringRef Str) : Data(Str.str()) {}
    Triple(const std::string &Str) : Data(Str) {}
    Triple(const char *Str) : Data(Str) {}

    const std::string &str() const { return Data; }
    StringRef getTriple() const { return Data; }

    ArchType getArch() const {
        if (Data.find("aarch64") != std::string::npos) return aarch64;
        if (Data.find("x86_64") != std::string::npos) return x86_64;
        return UnknownArch;
    }

    OSType getOS() const {
        if (Data.find("darwin") != std::string::npos) return Darwin;
        if (Data.find("linux") != std::string::npos) return Linux;
        return UnknownOS;
    }

    bool isOSDarwin() const { return getOS() == Darwin; }
    bool isOSLinux() const { return getOS() == Linux; }

    bool isArch64Bit() const { return true; }
    bool isArch32Bit() const { return false; }

    bool isOSBinFormatCOFF() const { return false; }
    bool isOSBinFormatELF() const { return getObjectFormat() == ELF; }
    bool isOSBinFormatMachO() const { return getObjectFormat() == MachO; }

    ObjectFormatType getObjectFormat() const {
        if (isOSDarwin()) return MachO;
        return ELF;
    }

    StringRef getArchName() const {
        if (getArch() == aarch64) return "aarch64";
        if (getArch() == x86_64) return "x86_64";
        return "unknown";
    }

    std::string normalize() const { return Data; }

    operator StringRef() const { return StringRef(Data); }

    bool operator==(const Triple &Other) const { return Data == Other.Data; }
    bool operator!=(const Triple &Other) const { return Data != Other.Data; }
};

} // namespace llvm

#endif
