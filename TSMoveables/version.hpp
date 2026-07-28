//
//  version.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - which version of this you are compiling against.
//
//  This exists because of how the library is meant to be consumed. CMake knows
//  the version and exports it for `find_package`, but the pitch in the README
//  is "copy one file into your project" - and someone who dropped
//  single_include/ts_moveables.hpp into a tree with no build system at all has
//  no CMake to ask. Without these macros there is no way for them to find out
//  what they have, or to write code that works against two versions.
//
//      #if SNICHOLLS_VERSION >= SNICHOLLS_VERSION_CHECK(1, 1, 0)
//          // use something that only exists from 1.1 onwards
//      #endif
//
//      std::cout << snicholls::version_string();      // "1.0.0"
//
//  The number here, the `project(... VERSION)` in CMakeLists.txt, and the git
//  tag are three copies of one fact, so they are checked against each other by
//  scripts/check_version.py on every CI job. Three places holding the same
//  number is exactly how a number goes quietly wrong.
//

#ifndef ts_moveables_version_hpp
#define ts_moveables_version_hpp

#define SNICHOLLS_VERSION_MAJOR 1
#define SNICHOLLS_VERSION_MINOR 0
#define SNICHOLLS_VERSION_PATCH 0

// Comparable in the preprocessor. Two decimal digits each for minor and patch,
// which is plenty and keeps the number readable: 1.0.0 is 10000, 1.2.3 is
// 10203.
#define SNICHOLLS_VERSION_CHECK(major, minor, patch) \
    ((major) * 10000 + (minor) * 100 + (patch))

#define SNICHOLLS_VERSION                                                    \
    SNICHOLLS_VERSION_CHECK(SNICHOLLS_VERSION_MAJOR, SNICHOLLS_VERSION_MINOR, \
                            SNICHOLLS_VERSION_PATCH)

#define SNICHOLLS_VERSION_STRINGIFY_(x) #x
#define SNICHOLLS_VERSION_STRINGIFY(x) SNICHOLLS_VERSION_STRINGIFY_(x)

#define SNICHOLLS_VERSION_STRING                       \
    SNICHOLLS_VERSION_STRINGIFY(SNICHOLLS_VERSION_MAJOR) "." \
    SNICHOLLS_VERSION_STRINGIFY(SNICHOLLS_VERSION_MINOR) "." \
    SNICHOLLS_VERSION_STRINGIFY(SNICHOLLS_VERSION_PATCH)

namespace snicholls {

// constexpr so it costs nothing to ask, and can be used in a static_assert
constexpr int version_major() noexcept { return SNICHOLLS_VERSION_MAJOR; }
constexpr int version_minor() noexcept { return SNICHOLLS_VERSION_MINOR; }
constexpr int version_patch() noexcept { return SNICHOLLS_VERSION_PATCH; }
constexpr int version() noexcept { return SNICHOLLS_VERSION; }

constexpr const char* version_string() noexcept { return SNICHOLLS_VERSION_STRING; }

} // namespace snicholls

#endif /* ts_moveables_version_hpp */
