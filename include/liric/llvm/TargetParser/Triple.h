#ifndef LLVM_TARGETPARSER_TRIPLE_H
#define LLVM_TARGETPARSER_TRIPLE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include <string>
#include <vector>

namespace liric_llvm {
class Triple {
    std::string Data;
    static std::vector<std::string> split(const std::string &text) {
        std::vector<std::string> fields;
        size_t begin = 0;
        while (begin <= text.size()) {
            size_t end = text.find('-', begin);
            fields.push_back(text.substr(begin, end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return fields;
    }
    std::string component(size_t index) const {
        auto fields = split(Data);
        return index < fields.size() ? fields[index] : "";
    }
    static bool versioned(const std::string &text, const std::string &name) {
        return text == name || (text.compare(0, name.size(), name) == 0
            && text.size() > name.size() && text[name.size()] >= '0'
            && text[name.size()] <= '9');
    }
    static bool os_component(const std::string &text) {
        return versioned(text, "linux") || versioned(text, "darwin")
            || versioned(text, "macosx") || text == "windows"
            || text == "win32" || text == "emscripten" || text == "wasi";
    }
public:
    enum ArchType { UnknownArch, aarch64, x86_64, x86, wasm32, wasm64 };
    enum OSType { UnknownOS, Darwin, Linux, Win32, Emscripten, WASI };
    enum EnvironmentType { UnknownEnvironment, GNU, GNUX32, Musl, MSVC };
    enum ObjectFormatType { UnknownObjectFormat, ELF, MachO, COFF, Wasm };

    Triple() = default;
    Triple(StringRef text): Data(text.str()) {}
    Triple(const std::string &text): Data(text) {}
    Triple(const char *text): Data(text) {}
    const std::string &str() const { return Data; }
    StringRef getTriple() const { return Data; }
    static std::string normalize(StringRef text) {
        auto fields = split(text.str());
        if (!fields.empty()) {
            if (fields[0] == "amd64") fields[0] = "x86_64";
            if (fields[0] == "arm64") fields[0] = "aarch64";
        }
        if (fields.size() > 1 && os_component(fields[1]))
            fields.insert(fields.begin() + 1, "unknown");
        std::string result;
        for (const auto &field: fields) {
            if (!result.empty()) result += '-';
            result += field;
        }
        return result;
    }
    std::string normalize() const { return normalize(Data); }
    ArchType getArch() const {
        auto arch = component(0);
        if (arch == "aarch64" || arch == "arm64") return aarch64;
        if (arch == "x86_64" || arch == "amd64") return x86_64;
        if (arch == "i386" || arch == "i486" || arch == "i586"
                || arch == "i686" || arch == "x86") return x86;
        if (arch == "wasm32") return wasm32;
        if (arch == "wasm64") return wasm64;
        return UnknownArch;
    }
    OSType getOS() const {
        auto os = component(2);
        if (versioned(os, "darwin") || versioned(os, "macosx")) return Darwin;
        if (versioned(os, "linux")) return Linux;
        if (os == "windows" || os == "win32") return Win32;
        if (os == "emscripten") return Emscripten;
        if (os == "wasi") return WASI;
        return UnknownOS;
    }
    EnvironmentType getEnvironment() const {
        auto env = component(3);
        if (env == "gnu") return GNU;
        if (env == "gnux32") return GNUX32;
        if (env == "musl") return Musl;
        if (env == "msvc") return MSVC;
        return UnknownEnvironment;
    }
    std::string getEnvironmentName() const { return component(3); }
    bool hasSupportedComponents() const {
        auto fields = split(Data);
        return fields.size() <= 5 && (getEnvironment() != UnknownEnvironment
            || getEnvironmentName().empty() || getEnvironmentName() == "unknown");
    }
    bool isOSDarwin() const { return getOS() == Darwin; }
    bool isOSLinux() const { return getOS() == Linux; }
    bool isAArch64() const { return getArch() == aarch64; }
    bool isArch64Bit() const {
        return getArch() == aarch64 || getArch() == x86_64 || getArch() == wasm64;
    }
    bool isArch32Bit() const { return getArch() == x86 || getArch() == wasm32; }
    ObjectFormatType getObjectFormat() const {
        auto format = component(4);
        if (!format.empty()) {
            if (format == "elf") return ELF;
            if (format == "macho") return MachO;
            if (format == "coff") return COFF;
            if (format == "wasm") return Wasm;
            return UnknownObjectFormat;
        }
        if (isOSDarwin()) return MachO;
        if (getOS() == Win32) return COFF;
        if (getArch() == wasm32 || getArch() == wasm64) return Wasm;
        if (isOSLinux()) return ELF;
        return UnknownObjectFormat;
    }
    bool isOSBinFormatCOFF() const { return getObjectFormat() == COFF; }
    bool isOSBinFormatELF() const { return getObjectFormat() == ELF; }
    bool isOSBinFormatMachO() const { return getObjectFormat() == MachO; }
    StringRef getArchName() const {
        switch (getArch()) {
            case aarch64: return "aarch64";
            case x86_64: return "x86_64";
            case x86: return "i386";
            case wasm32: return "wasm32";
            case wasm64: return "wasm64";
            default: return "unknown";
        }
    }
    operator StringRef() const { return Data; }
    bool operator==(const Triple &other) const { return Data == other.Data; }
    bool operator!=(const Triple &other) const { return Data != other.Data; }
};
} // namespace liric_llvm
#endif
