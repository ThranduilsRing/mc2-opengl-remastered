#pragma once

#include "Gates/gate_cold.h"
#include <cstdlib>
#include <optional>
#include <string>

// An environment override belongs to one test; later tests must see the prior value and cache state.
class ContractEnvironment {
public:
    ContractEnvironment(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        assign(value);
    }
    ContractEnvironment(const ContractEnvironment&) = delete;
    ContractEnvironment& operator=(const ContractEnvironment&) = delete;
    ~ContractEnvironment() { assign(old_ ? old_->c_str() : nullptr); }
private:
    void assign(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : "");
#else
        if (value) setenv(name_.c_str(), value, 1);
        else unsetenv(name_.c_str());
#endif
        mc2::gate::invalidateAll();
    }
    std::string name_;
    std::optional<std::string> old_;
};
