#ifndef LLVM_SUPPORT_ERROR_H
#define LLVM_SUPPORT_ERROR_H

#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace llvm {

class ErrorInfoBase {
public:
    virtual ~ErrorInfoBase() = default;
    virtual void log(raw_ostream &OS) const = 0;
    virtual std::string message() const {
        std::string msg;
        raw_string_ostream os(msg);
        log(os);
        return msg;
    }
};

class StringError : public ErrorInfoBase {
    std::string Msg;
public:
    StringError(const std::string &S, std::error_code = {}) : Msg(S) {}
    void log(raw_ostream &OS) const override { OS << Msg; }
    std::string message() const override { return Msg; }
};

class Error {
    std::unique_ptr<ErrorInfoBase> Payload;
    bool Checked = false;

public:
    Error() : Checked(true) {}
    Error(std::unique_ptr<ErrorInfoBase> P) : Payload(std::move(P)) {}

    Error(Error &&Other) : Payload(std::move(Other.Payload)), Checked(Other.Checked) {
        Other.Checked = true;
    }

    Error &operator=(Error &&Other) {
        Payload = std::move(Other.Payload);
        Checked = Other.Checked;
        Other.Checked = true;
        return *this;
    }

    ~Error() { (void)Checked; }

    explicit operator bool() {
        Checked = true;
        return Payload != nullptr;
    }

    static Error success() { return Error(); }

    void operator*() const {}

    std::string message() const {
        if (Payload) return Payload->message();
        return "";
    }
};

inline Error make_error(const std::string &Msg) {
    return Error(std::make_unique<StringError>(Msg));
}

template <typename T>
class Expected {
    union {
        T Val;
        Error Err;
    };
    bool HasError;

public:
    Expected(T V) : Val(std::move(V)), HasError(false) {}
    Expected(Error E) : Err(std::move(E)), HasError(true) {}

    Expected(Expected &&Other) : HasError(Other.HasError) {
        if (HasError)
            new (&Err) Error(std::move(Other.Err));
        else
            new (&Val) T(std::move(Other.Val));
    }

    ~Expected() {
        if (HasError) Err.~Error();
        else Val.~T();
    }

    explicit operator bool() const { return !HasError; }

    T &operator*() {
        assert(!HasError);
        return Val;
    }

    const T &operator*() const {
        assert(!HasError);
        return Val;
    }

    T *operator->() {
        assert(!HasError);
        return &Val;
    }

    const T *operator->() const {
        assert(!HasError);
        return &Val;
    }

    Error takeError() {
        if (HasError) return std::move(Err);
        return Error::success();
    }

    const T &get() const {
        assert(!HasError);
        return Val;
    }

    T &get() {
        assert(!HasError);
        return Val;
    }
};

template <typename T>
class Expected<T &> {
    T *ValPtr = nullptr;
    Error Err;
    bool HasError;

public:
    Expected(T &V) : ValPtr(&V), Err(Error::success()), HasError(false) {}
    Expected(Error E) : Err(std::move(E)), HasError(true) {}

    Expected(Expected &&Other) : HasError(Other.HasError) {
        if (HasError)
            new (&Err) Error(std::move(Other.Err));
        else
            ValPtr = Other.ValPtr;
    }

    ~Expected() = default;

    explicit operator bool() const { return !HasError; }

    T &operator*() {
        assert(!HasError);
        return *ValPtr;
    }

    T *operator->() {
        assert(!HasError);
        return ValPtr;
    }

    Error takeError() {
        if (HasError) return std::move(Err);
        return Error::success();
    }

    T &get() {
        assert(!HasError);
        return *ValPtr;
    }
};

template <typename T>
T cantFail(Expected<T> E) {
    assert(static_cast<bool>(E));
    return std::move(*E);
}

template <typename T>
T &cantFail(Expected<T &> E) {
    assert(static_cast<bool>(E));
    return *E;
}

inline void cantFail(Error E) {
    bool failed = static_cast<bool>(E);
    (void)failed;
    assert(!failed);
}

inline void logAllUnhandledErrors(Error E, raw_ostream &OS, const std::string &Banner = "") {
    if (E) {
        if (!Banner.empty()) OS << Banner;
        OS << E.message() << "\n";
    }
}

inline void consumeError(Error E) {
    bool b = static_cast<bool>(E);
    (void)b;
}

inline Error joinErrors(Error E1, Error E2) {
    bool b1 = static_cast<bool>(E1);
    bool b2 = static_cast<bool>(E2);
    (void)b1;
    (void)b2;
    return Error::success();
}

inline Error errorCodeToError(std::error_code EC) {
    if (EC) return make_error(EC.message());
    return Error::success();
}

template <typename... HandlerTs>
Error handleErrors(Error E, HandlerTs &&...) {
    bool b = static_cast<bool>(E);
    (void)b;
    return Error::success();
}

inline std::string toString(Error E) {
    std::string msg = E.message();
    (void)static_cast<bool>(E);
    return msg;
}

} // namespace llvm

#endif
