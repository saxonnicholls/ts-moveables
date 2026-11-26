//
//  concepts/detect.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Whether this compilation has C++20 concepts, for the interface twins in concepts/: TS_MOVEABLES_HAS_CONCEPTS is 1
//  when both the language feature (from the compiler) and the library one (from <version>) are there. ts-moveables
//  still builds as C++17, where each twin offers its C++17 trait alone.
//

#ifndef concepts_detect_hpp
#define concepts_detect_hpp

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif
#if defined(__cpp_concepts) && __cpp_concepts >= 201907L && defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
#define TS_MOVEABLES_HAS_CONCEPTS 1
#else
#define TS_MOVEABLES_HAS_CONCEPTS 0
#endif

#if TS_MOVEABLES_HAS_CONCEPTS
#include <concepts>
#endif

#endif /* concepts_detect_hpp */
