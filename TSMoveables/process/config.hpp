//
//  config.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - layered configuration: defaults < .env files <
//  the environment < the command line, each value knowing where it came from:
//
//      snicholls::config cfg = snicholls::load_config({.defaults = {{"PORT", "8080"}},
//                                                      .env_files = {".env", ".env.local"}});
//      cfg.require({"DATABASE_URL", "API_TOKEN"});      // one error naming every missing key
//      auto port    = cfg.get<std::uint16_t>("PORT");    // strict: "80x" is an error, not 80
//      auto timeout = cfg.get_or("TIMEOUT", std::chrono::milliseconds{250});   // "2s", "250ms"
//      auto token   = cfg.get<snicholls::secret<std::string>>("API_TOKEN");
//      std::cout << cfg.describe();                      // every key, its source, secrets masked
//
//  Values parse strictly through value_parser<T> (specialise it for your own
//  types): integers and floating point by from_chars, bool as true/false,
//  1/0, yes/no, on/off, durations with a unit, paths, std::optional (empty is
//  nullopt), std::vector ("a, b" or "[a, b]"), std::map<std::string, T>
//  ("k=v, k2=v2"), and core's secret<T>. A bad value is a config_error that
//  names the key and masks the value when it looks secret (is_secret_key,
//  mark_secret, or credentials in a URL). try_get returns a result instead of
//  throwing.
//
//  A config is a plain value: build it once at start-up and pass it around.
//  Reading the process environment (process_environment) and
//  export_to_environment (setenv) are not thread-safe - nothing in POSIX
//  makes them so - so do both before starting threads.
//
//  Ported from an earlier application framework's config.hpp, which had its own secret type (core's
//  is used here) and could bind() into reflected objects (dropped: it needs
//  that framework's field reflection). require, missing, try_get and the map parser
//  are new here. C++20 and POSIX, opt-in like fs/.
//

#ifndef ts_moveables_process_config_hpp
#define ts_moveables_process_config_hpp

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <initializer_list>

#include <snicholls/core/result.hpp>
#include <snicholls/core/secret.hpp>

#include "dotenv.hpp"

extern "C" char** environ;   ///< POSIX; declared here because not every \<unistd.h> does

namespace snicholls {

// ---------------------------------------------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------------------------------------------

// Thrown by value_parser; config turns it into config_error naming the key.
class bad_config_value : public std::invalid_argument
{
public:
    using std::invalid_argument::invalid_argument;
};

namespace detail::config_detail {

constexpr char ascii_lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr char ascii_upper(char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
struct is_vector : std::false_type {};
template <typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template <typename T>
struct is_duration : std::false_type {};
template <typename R, typename P>
struct is_duration<std::chrono::duration<R, P>> : std::true_type {};

template <typename T>
inline constexpr bool is_character_v =
    std::is_same_v<T, char> || std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> ||
    std::is_same_v<T, wchar_t> || std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> || std::is_same_v<T, char32_t>;

template <typename T>
T parse_number(std::string_view text)
{
    std::string_view s = text;
    if (s.starts_with('+'))
        s.remove_prefix(1);
    if (s.empty())
        throw bad_config_value("expected a number, got an empty value");
    T value{};
    if constexpr (std::is_floating_point_v<T>)
    {
        // Not from_chars: older Apple libc++ deletes its floating-point overloads. A stream in the classic
        // locale reads the same decimal and exponent forms, whatever locale the program has set
        if (s.front() == ' ' || s.front() == '\t')         // a stream would skip it; from_chars would not
            throw bad_config_value("expected a number");
        std::istringstream in{std::string(s)};
        in.imbue(std::locale::classic());
        in >> value;
        if (!in)
            throw bad_config_value(in.eof() && std::isinf(value) ? "number out of range" : "expected a number");
        if (in.peek() != std::char_traits<char>::eof())
            throw bad_config_value("expected a number");
        if (std::isinf(value))
            throw bad_config_value("number out of range");
        return value;
    }
    else
    {
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec == std::errc::result_out_of_range)
            throw bad_config_value("number out of range");
        if (ec != std::errc{} || end != s.data() + s.size())
            throw bad_config_value("expected an integer");
        return value;
    }
}

// "250ms" -> 250 milliseconds, converted to Duration exactly (a lossy conversion is an error).
template <typename Duration>
Duration parse_duration(std::string_view text)
{
    std::size_t digits = 0;
    while (digits < text.size() && ((text[digits] >= '0' && text[digits] <= '9') || (digits == 0 && text[digits] == '-')))
        ++digits;
    const std::string_view number = text.substr(0, digits);
    const std::string_view unit = trim(text.substr(digits));
    if (number.empty() || number == "-")
        throw bad_config_value("expected a duration such as 250ms, 30s, 5m");
    const std::int64_t count = parse_number<std::int64_t>(number);

    const auto convert = [&]<typename Period>(Period) -> Duration {
        using source = std::chrono::duration<std::int64_t, Period>;
        using wide = std::chrono::duration<long double, typename Duration::period>;
        const long double exact = std::chrono::duration_cast<wide>(source(count)).count();
        if (exact > static_cast<long double>(std::numeric_limits<typename Duration::rep>::max()) ||
            exact < static_cast<long double>(std::numeric_limits<typename Duration::rep>::lowest()))
            throw bad_config_value("duration out of range");
        const Duration result = std::chrono::duration_cast<Duration>(source(count));
        if (std::chrono::duration_cast<source>(result) != source(count))
            throw bad_config_value("duration is not a whole number of the target unit");
        return result;
    };
    if (unit == "ns")
        return convert(std::nano{});
    if (unit == "us")
        return convert(std::micro{});
    if (unit == "ms")
        return convert(std::milli{});
    if (unit == "s")
        return convert(std::ratio<1>{});
    if (unit == "m" || unit == "min")
        return convert(std::ratio<60>{});
    if (unit == "h")
        return convert(std::ratio<3600>{});
    throw bad_config_value("duration needs a unit: ns, us, ms, s, m, h");
}

} // namespace detail::config_detail

// value_parser<T>::parse(text) -> T. Specialise it for your own types.
template <typename T, typename = void>
struct value_parser;

template <>
struct value_parser<std::string>
{
    static std::string parse(std::string_view text) { return std::string(text); }
};

template <>
struct value_parser<std::filesystem::path>
{
    static std::filesystem::path parse(std::string_view text) { return std::filesystem::path(text); }
};

template <>
struct value_parser<bool>
{
    static bool parse(std::string_view text)
    {
        std::string lower(detail::config_detail::trim(text));
        std::transform(lower.begin(), lower.end(), lower.begin(), detail::config_detail::ascii_lower);
        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
            return true;
        if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
            return false;
        throw bad_config_value("expected a boolean (true/false, 1/0, yes/no, on/off)");
    }
};

template <typename T>
struct value_parser<T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool> && !detail::config_detail::is_character_v<T>>>
{
    static T parse(std::string_view text) { return detail::config_detail::parse_number<T>(detail::config_detail::trim(text)); }
};

template <typename R, typename P>
struct value_parser<std::chrono::duration<R, P>, std::enable_if_t<std::is_integral_v<R>>>
{
    static std::chrono::duration<R, P> parse(std::string_view text)
    {
        return detail::config_detail::parse_duration<std::chrono::duration<R, P>>(detail::config_detail::trim(text));
    }
};

template <typename T>
struct value_parser<std::optional<T>>
{
    static std::optional<T> parse(std::string_view text)
    {
        if (detail::config_detail::trim(text).empty())
            return std::nullopt;
        return value_parser<T>::parse(text);
    }
};

template <typename T, typename A>
struct value_parser<std::vector<T, A>>
{
    static std::vector<T, A> parse(std::string_view text)
    {
        std::string_view s = detail::config_detail::trim(text);
        if (s.starts_with('[') && s.ends_with(']'))
            s = detail::config_detail::trim(s.substr(1, s.size() - 2));
        std::vector<T, A> out;
        if (s.empty())
            return out;
        for (;;)
        {
            const std::size_t comma = s.find(',');
            const std::string_view item = detail::config_detail::trim(s.substr(0, comma));
            if (item.empty())
                throw bad_config_value("empty element in list");
            out.push_back(value_parser<T>::parse(item));
            if (comma == std::string_view::npos)
                break;
            s = s.substr(comma + 1);
        }
        return out;
    }
};

// "a=1, b=2" (optionally in braces) -> {{"a", 1}, {"b", 2}}
template <typename T, typename C, typename A>
struct value_parser<std::map<std::string, T, C, A>>
{
    static std::map<std::string, T, C, A> parse(std::string_view text)
    {
        std::string_view s = detail::config_detail::trim(text);
        if (s.starts_with('{') && s.ends_with('}'))
            s = detail::config_detail::trim(s.substr(1, s.size() - 2));
        std::map<std::string, T, C, A> out;
        if (s.empty())
            return out;
        for (;;)
        {
            const std::size_t comma = s.find(',');
            const std::string_view item = detail::config_detail::trim(s.substr(0, comma));
            const std::size_t eq = item.find('=');
            if (eq == std::string_view::npos || detail::config_detail::trim(item.substr(0, eq)).empty())
                throw bad_config_value("expected key=value pairs separated by commas");
            std::string key(detail::config_detail::trim(item.substr(0, eq)));
            if (!out.emplace(std::move(key), value_parser<T>::parse(detail::config_detail::trim(item.substr(eq + 1)))).second)
                throw bad_config_value("a key appears twice in the map");
            if (comma == std::string_view::npos)
                break;
            s = s.substr(comma + 1);
        }
        return out;
    }
};

template <typename T>
struct value_parser<secret<T>>
{
    static secret<T> parse(std::string_view text) { return secret<T>(value_parser<T>::parse(text)); }
};

template <typename T>
concept config_value = requires(std::string_view text) {
    { value_parser<std::remove_cv_t<T>>::parse(text) } -> std::convertible_to<std::remove_cv_t<T>>;
};

template <config_value T>
[[nodiscard]] T parse_value(std::string_view text)
{
    return value_parser<T>::parse(text);
}

// ---------------------------------------------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------------------------------------------

enum class config_source
{
    default_value,   ///< default value
    env_file,   ///< env file
    environment,   ///< environment
    command_line,   ///< command line
};

[[nodiscard]] constexpr std::string_view to_string(config_source source) noexcept
{
    switch (source)
    {
    case config_source::default_value: return "default";
    case config_source::env_file: return "env-file";
    case config_source::environment: return "environment";
    case config_source::command_line: return "command-line";
    }
    return "?";
}

struct config_entry
{
    std::string value;
    config_source source = config_source::default_value;
    std::string origin;   ///< ".env:12", "--port", "$PORT", "default"
};

class config
{
public:
    void set(std::string key, std::string value, config_source source = config_source::default_value,
             std::string origin = {})
    {
        if (source != config_source::environment)
            declared_.insert(key);
        entries_[std::move(key)] = config_entry{std::move(value), source, std::move(origin)};
    }

    [[nodiscard]] bool contains(std::string_view key) const { return entries_.find(key) != entries_.end(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    [[nodiscard]] const config_entry* entry(std::string_view key) const
    {
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const std::string* raw(std::string_view key) const
    {
        const config_entry* e = entry(key);
        return e != nullptr ? &e->value : nullptr;
    }

    [[nodiscard]] std::vector<std::string> keys() const
    {
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const auto& [key, entry] : entries_)
            out.push_back(key);
        return out;
    }

    template <config_value T>
    [[nodiscard]] T get(std::string_view key) const
    {
        const std::string* text = raw(key);
        if (text == nullptr)
            throw config_error("snicholls: missing configuration key " + std::string(key));
        return convert<T>(key, *text);
    }

    template <config_value T>
    [[nodiscard]] T get_or(std::string_view key, T fallback) const
    {
        const std::string* text = raw(key);
        return text == nullptr ? std::move(fallback) : convert<T>(key, *text);
    }

    template <config_value T>
    [[nodiscard]] std::optional<T> find(std::string_view key) const
    {
        const std::string* text = raw(key);
        if (text == nullptr)
            return std::nullopt;
        return convert<T>(key, *text);
    }

    // get() without the exception: the error says which key and why, with the value masked
    template <config_value T>
    [[nodiscard]] result<T, config_error> try_get(std::string_view key) const
    {
        try
        {
            return ok(get<T>(key));
        }
        catch (const config_error& e)
        {
            return err(e);
        }
    }

    // The keys of `keys` that are not set
    [[nodiscard]] std::vector<std::string> missing(std::initializer_list<std::string_view> keys) const
    {
        std::vector<std::string> out;
        for (const std::string_view key : keys)
            if (!contains(key))
                out.emplace_back(key);
        return out;
    }

    // Throws one config_error naming every missing key, so a deploy is fixed in one go, not one key per restart
    void require(std::initializer_list<std::string_view> keys) const
    {
        const std::vector<std::string> absent = missing(keys);
        if (absent.empty())
            return;
        std::string message = "snicholls: missing configuration key";
        message += absent.size() == 1 ? " " : "s ";
        for (std::size_t i = 0; i < absent.size(); ++i)
            message += (i == 0 ? "" : ", ") + absent[i];
        throw config_error(message);
    }

    [[nodiscard]] config profile(std::string_view prefix) const
    {
        std::string p(prefix);
        if (!p.empty() && p.back() != '_')
            p += '_';
        config out;
        for (const auto& [key, e] : entries_)
            if (key.size() > p.size() && key.starts_with(p))
            {
                std::string stripped = key.substr(p.size());
                if (is_secret(key))
                    out.secrets_.insert(stripped);
                if (declared_.contains(key))
                    out.declared_.insert(stripped);
                out.entries_[std::move(stripped)] = e;
            }
        return out;
    }

    // ---- secrets ---------------------------------------------------------------------------------------------

    void mark_secret(std::string key) { secrets_.insert(std::move(key)); }

    [[nodiscard]] bool is_secret(std::string_view key) const
    {
        return secrets_.find(key) != secrets_.end() || is_secret_key(key);
    }

    [[nodiscard]] std::string mask(std::string_view key, std::string_view value) const
    {
        if (is_secret(key))
            return std::string(masked_text);
        return mask_url_credentials(value);
    }

    [[nodiscard]] std::string display(std::string_view key) const
    {
        const std::string* text = raw(key);
        return text == nullptr ? std::string("<unset>") : mask(key, *text);
    }

    [[nodiscard]] std::string describe(bool include_environment = false) const
    {
        std::ostringstream out;
        for (const auto& [key, e] : entries_)
        {
            if (!include_environment && e.source == config_source::environment && !declared_.contains(key))
                continue;
            out << key << '=' << mask(key, e.value) << "    # " << to_string(e.source);
            if (!e.origin.empty())
                out << ' ' << e.origin;
            out << '\n';
        }
        return std::move(out).str();
    }

    // KEY for a prefix and a dotted path: ("db", "pool.max-size") -> DB_POOL_MAX_SIZE
    [[nodiscard]] static std::string key_for(std::string_view prefix, std::string_view path)
    {
        std::string key;
        for (const char c : prefix)
            key += detail::config_detail::ascii_upper(c);
        if (!key.empty() && key.back() != '_')
            key += '_';
        for (const char c : path)
            key += (c == '.' || c == '-') ? '_' : detail::config_detail::ascii_upper(c);
        return key;
    }

private:
    template <typename T>
    T convert(std::string_view key, const std::string& text) const
    {
        try
        {
            return value_parser<std::remove_cv_t<T>>::parse(text);
        }
        catch (const bad_config_value& e)
        {
            throw config_error("snicholls: configuration key " + std::string(key) + " = \"" + mask(key, text) +
                               "\": " + e.what());
        }
    }

    std::map<std::string, config_entry, std::less<>> entries_;
    std::set<std::string, std::less<>> secrets_;
    std::set<std::string, std::less<>> declared_;   // keys set by a non-environment source
};

// ---------------------------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------------------------

struct config_sources
{
    std::map<std::string, std::string> defaults;
    std::vector<std::filesystem::path> env_files{".env"};
    bool env_files_required = false;
    bool read_environment = true;
    std::optional<std::map<std::string, std::string>> environment;   ///< replaces the process environment (tests)
    std::vector<std::pair<std::string, std::string>> overrides;   ///< command line, in order
    std::vector<std::string> secret_keys;   ///< extra keys to mask
};

[[nodiscard]] inline std::map<std::string, std::string> process_environment()
{
    std::map<std::string, std::string> out;
    for (char** e = ::environ; e != nullptr && *e != nullptr; ++e)
    {
        const std::string_view entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq != std::string_view::npos && eq > 0)
            out.emplace(std::string(entry.substr(0, eq)), std::string(entry.substr(eq + 1)));
    }
    return out;
}

[[nodiscard]] inline config load_config(const config_sources& sources)
{
    config cfg;
    for (const std::string& key : sources.secret_keys)
        cfg.mark_secret(key);
    for (const auto& [key, value] : sources.defaults)
        cfg.set(key, value, config_source::default_value, "default");

    std::map<std::string, std::string> environment;
    if (sources.read_environment)
        environment = sources.environment ? *sources.environment : process_environment();

    // ${NAME} in a file: earlier files' keys, then the environment.
    const dotenv_lookup lookup = [&](std::string_view name) -> std::optional<std::string> {
        if (const config_entry* e = cfg.entry(name); e != nullptr && e->source == config_source::env_file)
            return e->value;
        if (const auto it = environment.find(std::string(name)); it != environment.end())
            return it->second;
        return std::nullopt;
    };
    for (const std::filesystem::path& file : sources.env_files)
    {
        const std::optional<std::vector<dotenv_entry>> entries = read_dotenv_file(file, lookup);
        if (!entries)
        {
            if (sources.env_files_required)
                throw config_error("snicholls: configuration file not found: " + file.string());
            continue;
        }
        for (const dotenv_entry& e : *entries)
            cfg.set(e.key, e.value, config_source::env_file, file.string() + ":" + std::to_string(e.line));
    }
    for (const auto& [key, value] : environment)
        cfg.set(key, value, config_source::environment, "$" + key);
    for (const auto& [key, value] : sources.overrides)
        cfg.set(key, value, config_source::command_line, "--" + key);
    return cfg;
}

inline std::size_t export_to_environment(const config& cfg, bool overwrite = false)
{
    std::size_t count = 0;
    for (const std::string& key : cfg.keys())
    {
        const config_entry* e = cfg.entry(key);
        if (e->source != config_source::env_file)
            continue;
        if (::setenv(key.c_str(), e->value.c_str(), overwrite ? 1 : 0) == 0)
            ++count;
    }
    return count;
}

} // namespace snicholls

#endif // ts_moveables_process_config_hpp
