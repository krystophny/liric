#ifndef LLVM_ADT_ARRAYREF_H
#define LLVM_ADT_ARRAYREF_H

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace llvm {

template <typename T>
class ArrayRef {
    const T *data_;
    size_t len_;

public:
    ArrayRef() : data_(nullptr), len_(0) {}
    ArrayRef(const T *data, size_t len) : data_(data), len_(len) {}
    ArrayRef(const T *begin, const T *end) : data_(begin), len_(end - begin) {}
    ArrayRef(const std::vector<T> &v) : data_(v.data()), len_(v.size()) {}
    ArrayRef(std::initializer_list<T> il) : data_(il.begin()), len_(il.size()) {}
    template <size_t N>
    ArrayRef(const T (&arr)[N]) : data_(arr), len_(N) {}

    const T *data() const { return data_; }
    size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }

    const T *begin() const { return data_; }
    const T *end() const { return data_ + len_; }

    const T &operator[](size_t i) const { return data_[i]; }
    const T &front() const { return data_[0]; }
    const T &back() const { return data_[len_ - 1]; }
};

template <typename T>
class MutableArrayRef : public ArrayRef<T> {
    T *data_;

public:
    MutableArrayRef() : ArrayRef<T>(), data_(nullptr) {}
    MutableArrayRef(T *data, size_t len) : ArrayRef<T>(data, len), data_(data) {}
    T *data() const { return data_; }
    T &operator[](size_t i) const { return data_[i]; }
};

} // namespace llvm

#endif
