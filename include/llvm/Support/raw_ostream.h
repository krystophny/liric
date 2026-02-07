#ifndef LLVM_SUPPORT_RAW_OSTREAM_H
#define LLVM_SUPPORT_RAW_OSTREAM_H

#include "llvm/ADT/StringRef.h"
#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace llvm {

class raw_ostream {
public:
    virtual ~raw_ostream() = default;
    virtual raw_ostream &write(const char *ptr, size_t size) = 0;

    raw_ostream &operator<<(const char *s) {
        return write(s, std::strlen(s));
    }
    raw_ostream &operator<<(char c) {
        return write(&c, 1);
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
    raw_ostream &operator<<(long long v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(unsigned long long v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(double v) {
        std::string s = std::to_string(v);
        return write(s.data(), s.size());
    }
    raw_ostream &operator<<(const void *p) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%p", p);
        return write(buf, std::strlen(buf));
    }

    virtual void flush() {}

    virtual uint64_t tell() const { return 0; }

    raw_ostream &indent(unsigned NumSpaces) {
        for (unsigned i = 0; i < NumSpaces; ++i) write(" ", 1);
        return *this;
    }
};

class raw_pwrite_stream : public raw_ostream {
public:
    virtual void pwrite(const char *Ptr, size_t Size, uint64_t Offset) {
        (void)Ptr; (void)Size; (void)Offset;
    }
};

class raw_fd_ostream : public raw_pwrite_stream {
    FILE *f_;
    bool owns_;

public:
    raw_fd_ostream(int fd, bool shouldClose, bool unbuffered = false)
        : owns_(false) {
        (void)shouldClose;
        (void)unbuffered;
        if (fd == 1) f_ = stdout;
        else if (fd == 2) f_ = stderr;
        else f_ = stderr;
    }

    raw_fd_ostream(StringRef filename, std::error_code &EC,
                   unsigned flags = 0)
        : owns_(true) {
        (void)flags;
        f_ = fopen(std::string(filename).c_str(), "w");
        if (!f_) {
            EC = std::make_error_code(std::errc::no_such_file_or_directory);
            f_ = stderr;
            owns_ = false;
        }
    }

    ~raw_fd_ostream() override {
        if (owns_ && f_) fclose(f_);
    }

    raw_ostream &write(const char *ptr, size_t size) override {
        fwrite(ptr, 1, size, f_);
        return *this;
    }
    void flush() override { fflush(f_); }
};

class raw_string_ostream : public raw_pwrite_stream {
    std::string &str_;

public:
    explicit raw_string_ostream(std::string &s) : str_(s) {}
    raw_ostream &write(const char *ptr, size_t size) override {
        str_.append(ptr, size);
        return *this;
    }
    std::string &str() { return str_; }
};

class raw_svector_ostream : public raw_pwrite_stream {
    std::string internal_;

public:
    template <typename VecT>
    explicit raw_svector_ostream(VecT &) {}

    raw_ostream &write(const char *ptr, size_t size) override {
        internal_.append(ptr, size);
        return *this;
    }

    StringRef str() const { return StringRef(internal_); }
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
