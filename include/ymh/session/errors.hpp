#pragma once

// Store error taxonomy (02-persistence.md §2.2). Callers and tests share these
// names; 01 pins `LeaseLost` by name and 01 S11 pins a store-open failure.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace ymh {

class StoreError : public std::runtime_error {
public:
    explicit StoreError(const std::string& what) : std::runtime_error(what) {}
};

// 11-m2-errata §3.4 (D8): the store-open failure surface. `Locked` is the
// sidecar `flock(LOCK_EX | LOCK_NB)` EWOULDBLOCK case (02 §5.1) that
// `WorkspaceRuntime::create` maps to `WorkspaceBusy`; every other open failure
// (missing root, mkdir/open failure, sqlite open, schema gate) is
// `Unavailable`.
enum class StoreOpenErrorCode : std::uint8_t {
    Locked,
    Unavailable,
};

class StoreOpenError : public StoreError {
public:
    StoreOpenError(StoreOpenErrorCode code, const std::string& what)
        : StoreError(what), code_(code) {}

    // Back-compat for the pre-errata call sites (read-only store, missing
    // database, ...): those are all non-lock open failures.
    explicit StoreOpenError(const std::string& what)
        : StoreOpenError(StoreOpenErrorCode::Unavailable, what) {}

    [[nodiscard]] StoreOpenErrorCode code() const noexcept { return code_; }

private:
    StoreOpenErrorCode code_;
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
