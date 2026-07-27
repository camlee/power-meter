#include "semver.h"

#include <cctype>
#include <cstddef>
#include <cstring>

namespace semver {
namespace {

struct Parsed {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    const char* prerelease = nullptr;
    size_t prereleaseLength = 0;
};

bool parseNumber(const char*& cursor, uint32_t& result) {
    if (!cursor || !std::isdigit(static_cast<unsigned char>(*cursor))) return false;
    if (*cursor == '0' && std::isdigit(static_cast<unsigned char>(cursor[1]))) return false;
    uint64_t value = 0;
    do {
        value = value * 10 + static_cast<unsigned>(*cursor - '0');
        if (value > UINT32_MAX) return false;
        ++cursor;
    } while (std::isdigit(static_cast<unsigned char>(*cursor)));
    result = static_cast<uint32_t>(value);
    return true;
}

bool identifierChar(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '-';
}

bool validateIdentifiers(const char*& cursor, bool rejectNumericLeadingZero,
                         const char** startOut = nullptr, size_t* lengthOut = nullptr) {
    const char* start = cursor;
    while (true) {
        const char* identifier = cursor;
        bool numeric = true;
        while (identifierChar(*cursor)) {
            if (!std::isdigit(static_cast<unsigned char>(*cursor))) numeric = false;
            ++cursor;
        }
        if (cursor == identifier) return false;
        if (rejectNumericLeadingZero && numeric && cursor - identifier > 1 &&
            *identifier == '0') return false;
        if (*cursor != '.') break;
        ++cursor;
    }
    if (startOut) *startOut = start;
    if (lengthOut) *lengthOut = static_cast<size_t>(cursor - start);
    return true;
}

bool parse(const char* value, Parsed& out) {
    if (!value || !*value || std::strlen(value) > 95) return false;
    const char* cursor = value;
    if (!parseNumber(cursor, out.major) || *cursor++ != '.' ||
        !parseNumber(cursor, out.minor) || *cursor++ != '.' ||
        !parseNumber(cursor, out.patch)) return false;
    if (*cursor == '-') {
        ++cursor;
        if (!validateIdentifiers(cursor, true, &out.prerelease,
                                 &out.prereleaseLength)) return false;
    }
    if (*cursor == '+') {
        ++cursor;
        if (!validateIdentifiers(cursor, false)) return false;
    }
    return *cursor == '\0';
}

int compareNumber(uint32_t left, uint32_t right) {
    return left < right ? -1 : left > right ? 1 : 0;
}

bool segmentNumeric(const char* start, const char* end) {
    if (start == end) return false;
    for (const char* cursor = start; cursor < end; ++cursor) {
        if (!std::isdigit(static_cast<unsigned char>(*cursor))) return false;
    }
    return true;
}

int comparePrerelease(const Parsed& left, const Parsed& right) {
    if (!left.prerelease && !right.prerelease) return 0;
    if (!left.prerelease) return 1;
    if (!right.prerelease) return -1;

    const char* a = left.prerelease;
    const char* b = right.prerelease;
    const char* aEnd = a + left.prereleaseLength;
    const char* bEnd = b + right.prereleaseLength;
    while (a < aEnd && b < bEnd) {
        const char* aPartEnd = a;
        const char* bPartEnd = b;
        while (aPartEnd < aEnd && *aPartEnd != '.') ++aPartEnd;
        while (bPartEnd < bEnd && *bPartEnd != '.') ++bPartEnd;
        const bool aNumeric = segmentNumeric(a, aPartEnd);
        const bool bNumeric = segmentNumeric(b, bPartEnd);
        if (aNumeric != bNumeric) return aNumeric ? -1 : 1;
        const size_t aLength = static_cast<size_t>(aPartEnd - a);
        const size_t bLength = static_cast<size_t>(bPartEnd - b);
        if (aNumeric && aLength != bLength) return aLength < bLength ? -1 : 1;
        const size_t common = aLength < bLength ? aLength : bLength;
        const int lexical = std::memcmp(a, b, common);
        if (lexical != 0) return lexical < 0 ? -1 : 1;
        if (aLength != bLength) return aLength < bLength ? -1 : 1;
        a = aPartEnd < aEnd ? aPartEnd + 1 : aPartEnd;
        b = bPartEnd < bEnd ? bPartEnd + 1 : bPartEnd;
    }
    return a == aEnd ? (b == bEnd ? 0 : -1) : 1;
}

} // namespace

bool isValid(const char* value) {
    Parsed parsed{};
    return parse(value, parsed);
}

bool isStable(const char* value) {
    Parsed parsed{};
    return parse(value, parsed) && !parsed.prerelease;
}

int compare(const char* left, const char* right, bool& valid) {
    Parsed a{}, b{};
    valid = parse(left, a) && parse(right, b);
    if (!valid) return 0;
    int result = compareNumber(a.major, b.major);
    if (!result) result = compareNumber(a.minor, b.minor);
    if (!result) result = compareNumber(a.patch, b.patch);
    if (!result) result = comparePrerelease(a, b);
    return result;
}

} // namespace semver
