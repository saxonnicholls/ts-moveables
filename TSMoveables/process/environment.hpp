//
//  environment.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - which environment a process is running in: local,
//  staging or production. Code asks it whether to be strict, whether real
//  money or real customers are at stake, which endpoints to use:
//
//      auto cfg = snicholls::load_config({});
//      auto env = snicholls::environment_or(cfg, snicholls::run_environment::staging);
//      if (env.is_err()) { std::cerr << env.unwrap_err_unchecked().what(); return 78; }
//      if (*env == snicholls::run_environment::production) ...
//
//  It is read from the configuration key ENVIRONMENT, so a value in a .env
//  file counts as well as one in the process environment (the copy it
//  replaces read only getenv, so a .env value was silently ignored). Values are
//  case-insensitive: local (or dev, development), staging (or stage), and
//  production (or prod).
//
//    - An unknown value is an error, never a warning followed by a guess: a
//      typo in a deploy must not quietly run production as local.
//    - When ENVIRONMENT is unset the caller says what to assume. Until core
//      has a build mode to derive it from, the safe choice for a server is
//      staging: strict checks, but never "production" by accident.
//    - It is a value, not a frozen static, so a test builds a config with
//      the environment it wants.
//
//  After an earlier service's environment switch, fixed where it fell short. C++20 and POSIX, opt-in like fs/.
//

#ifndef ts_moveables_process_environment_hpp
#define ts_moveables_process_environment_hpp

#include <optional>
#include <string>
#include <string_view>

#include <snicholls/core/result.hpp>

#include "config.hpp"

namespace snicholls {

enum class run_environment
{
    local,
    staging,
    production,
};

[[nodiscard]] constexpr std::string_view to_string(run_environment e) noexcept
{
    switch (e) {
    case run_environment::local: return "local";
    case run_environment::staging: return "staging";
    case run_environment::production: return "production";
    }
    return "?";
}

// "Production", "prod", "STAGING", "dev" ...; nullopt for anything else
[[nodiscard]] inline std::optional<run_environment> parse_run_environment(std::string_view text) noexcept
{
    std::string lower;
    for (const char c : text) {
        if (c == ' ' || c == '\t') continue;
        lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (lower == "local" || lower == "dev" || lower == "development") return run_environment::local;
    if (lower == "staging" || lower == "stage") return run_environment::staging;
    if (lower == "production" || lower == "prod") return run_environment::production;
    return std::nullopt;
}

// The configured environment (key ENVIRONMENT, or `key`), `fallback` when unset, an error when unrecognised
[[nodiscard]] inline result<run_environment, config_error> environment_or(const config& cfg, run_environment fallback,
                                                                          std::string_view key = "ENVIRONMENT")
{
    const std::string* text = cfg.raw(key);
    if (text == nullptr || text->empty()) return ok(fallback);
    if (const std::optional<run_environment> e = parse_run_environment(*text)) return ok(*e);
    return err(config_error("snicholls: " + std::string(key) + " = \"" + cfg.mask(key, *text) +
                            "\" is not an environment: use local, staging or production"));
}

// Production and staging are where boot gates are strict
[[nodiscard]] constexpr bool is_strict(run_environment e) noexcept { return e != run_environment::local; }

} // namespace snicholls

#endif // ts_moveables_process_environment_hpp
