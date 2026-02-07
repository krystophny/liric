#ifndef LLVM_SUPPORT_RAW_OSTREAM_H
#define LLVM_SUPPORT_RAW_OSTREAM_H

#include "llvm/ADT/StringRef.h"
#include <cstdio>
#include <string>

namespace llvm {

class raw_ostream {
public:
    virtual ~raw_ostream() = default;
    virtual raw_ostream &write(const char *ptr, size_t size) = 0;

    raw_ostream &operator<<(const char *s) {
        return write(s, std::strlen(s));
    }
    raw_ostream &operator<<(const std::string &s) {
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(StringRef s) {
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(int v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(unsigned v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(long v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(unsigned long v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(double v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }

    virtual void flush() {}
};

class raw_fd_ostream : public raw_ostream {
    FILE *f_;

public:
    raw_fd_ostream(int fd, bool shouldClose, bool unbuffered = false) {
        (void)shouldClose;
        (void)unbuffered;
        if (fd == 1) f_ = stdout;
        else if (fd == 2) f_ = stderr;
        else f_ = stderr;
    }

    raw_ostream &write(const char *ptr, size_t size) override {
        fwrite(ptr, 1, size, f_);
        return *this;
    }
    void flush() override { fflush(f_); }
};

class raw_string_ostream : public raw_ostream {
    std::string &str_;

public:
    explicit raw_string_ostream(std::string &s) : str_(s) {}
    raw_ostream &write(const char *ptr, size_t size) override {
        str_.append(ptr, size);
        return *this;
    }
    std::string &str() { return str_; }
};

inline raw_fd_ostream &errs() {
    static raw_fd_ostream s(2, false);
    return s;
}

inline raw_fd_ostream &outs() {
    static raw_fd_ostream s(1, false);
    return s;
}

} // namespace llvm

#endif
