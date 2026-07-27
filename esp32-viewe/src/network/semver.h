#pragma once

#include <stdint.h>

namespace semver {

// Strict Semantic Versioning 2.0 validation and precedence comparison.
// `compare` returns -1, 0, or 1 and sets valid=false if either input is invalid.
bool isValid(const char* value);
bool isStable(const char* value);
int compare(const char* left, const char* right, bool& valid);

} // namespace semver
