#ifndef STRING_UTIL_H
#define STRING_UTIL_H

#include <string_view>
#include <string>

namespace flexql_util {

inline bool ieq_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = static_cast<char>(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = static_cast<char>(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

inline std::string to_lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace flexql_util

#endif // STRING_UTIL_H
