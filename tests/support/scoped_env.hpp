#pragma once

// RAII guard for process environment variables in tests. Restores the previous
// value -- or unsets the variable when it was previously absent -- on
// destruction, so a test's `setenv` never leaks into later tests that share the
// same process (gtest runs every test in one binary). Without this, a test that
// redirects `XDG_*`/`HOME` to a temp root silently pollutes
// `GlobalTestEnv`'s process-wide isolation and every test ordered after it.

#include <cstdlib>
#include <optional>
#include <string>

namespace ymh::test {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, std::string value) : name_(name) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
    ScopedEnvVar(ScopedEnvVar&&) = delete;
    ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
    std::string                name_;
    std::optional<std::string> previous_;
};

}  // namespace ymh::test
