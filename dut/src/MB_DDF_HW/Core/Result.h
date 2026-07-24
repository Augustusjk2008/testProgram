#pragma once

#include "MB_DDF_HW/Core/Status.h"

#include <utility>

namespace MB_DDF::HW {

template <typename T>
class Result {
public:
    Result(T value) : ok_(true), value_(std::move(value)), status_(Status::ok()) {}
    Result(Status status) : ok_(false), value_(), status_(std::move(status)) {}

    bool is_ok() const { return ok_; }
    explicit operator bool() const { return ok_; }

    T& value() { return value_; }
    const T& value() const { return value_; }

    const Status& status() const { return status_; }

private:
    bool ok_{false};
    T value_{};
    Status status_{};
};

template <>
class Result<void> {
public:
    Result() : status_(Status::ok()) {}
    Result(Status status) : status_(std::move(status)) {}

    bool is_ok() const { return status_.is_ok(); }
    explicit operator bool() const { return is_ok(); }

    const Status& status() const { return status_; }

private:
    Status status_{};
};

} // namespace MB_DDF::HW
