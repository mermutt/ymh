#pragma once

// Store error taxonomy (02-persistence.md §2.2). Callers and tests share these
// names; 01 pins `LeaseLost` by name and 01 S11 pins a store-open failure.

#include <stdexcept>
#include <string>

namespace ymh {

class StoreError : public std::runtime_error {
public:
    explicit StoreError(const std::string& what) : std::runtime_error(what) {}
};

class StoreOpenError : public StoreError {
public:
    using StoreError::StoreError;
};

class LeaseLost : public StoreError {
public:
    using StoreError::StoreError;
};

class CorruptionError : public StoreError {
public:
    using StoreError::StoreError;
};

class SchemaVersionError : public StoreError {
public:
    using StoreError::StoreError;
};

class PayloadTooLarge : public StoreError {
public:
    using StoreError::StoreError;
};

class DependentSessionError : public StoreError {
public:
    using StoreError::StoreError;
};

} // namespace ymh
