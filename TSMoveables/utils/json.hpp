//
//  utils/json.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - the JSON escaping every part of this library needs.
//
//  Small, and shared for a specific reason: it had been written twice, once in
//  the logger's JSON sink and once in the WebSocket broadcast hub, and the two
//  copies had already drifted. One handled \b and \f as short escapes and the
//  other fell through to  / ; one took a string_view and reserved,
//  the other took a const std::string&. Both produced legal JSON, which is
//  exactly why nobody noticed - a duplicate that is merely *different* rather
//  than wrong is the kind that survives, and then one copy gets a fix the
//  other does not.
//
//  Kept in `snicholls::utils`, not `snicholls::detail`. The latter was tried
//  first and collides: any translation unit doing `using namespace snicholls;`
//  plus `using namespace snicholls::http;` - which every benchmark and demo
//  here does - then sees two `detail` namespaces and every mention is
//  ambiguous. `utils` says the same thing about intent without shadowing a
//  name each module already uses for its own internals.
//

#ifndef ts_moveables_utils_json_hpp
#define ts_moveables_utils_json_hpp

#include <cstdio>
#include <string>
#include <string_view>

namespace snicholls {
namespace utils {

// Escape a byte string into the body of a JSON string literal - the surrounding
// quotes are the caller's. Enough of RFC 8259 to make any UTF-8 text a legal
// value: the two mandatory escapes, the five short forms, and \uXXXX for the
// remaining C0 controls.
//
// Bytes >= 0x20 pass through untouched, so valid UTF-8 stays valid UTF-8 and
// stays one byte per byte. Iterating as unsigned char is what makes that true
// on platforms where plain char is signed - a comparison against 0x20 on a
// signed char would treat every byte of a multi-byte sequence as a control.
inline void json_escape(std::string_view in, std::string& out)
{
    out.reserve(out.size() + in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c));
                out += b;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
}

} // namespace utils
} // namespace snicholls

#endif /* ts_moveables_utils_json_hpp */
