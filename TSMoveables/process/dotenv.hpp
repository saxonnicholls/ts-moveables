//
//  dotenv.hpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - .env files, parsed the way people write them:
//
//      auto entries = snicholls::read_dotenv_file(".env");   // nullopt when there is no file
//
//    - KEY=value, `export KEY=value`, blank lines and # comments; a " #"
//      starts a comment in an unquoted value, whose trailing blanks are trimmed
//    - 'single quotes' are literal and may span lines; "double quotes" take
//      \n \r \t \" \\ \$ escapes, may span lines, and interpolate
//    - ${NAME} from earlier in the file, else the lookup (usually the
//      environment), else empty; ${NAME:-default} when unset or empty;
//      ${NAME:?message} is an error when unset or empty
//    - CRLF line ends and a UTF-8 byte order mark are accepted
//
//  Errors throw config_error with the source and line ("prod.env:12: ..."),
//  and never quote a value, which may be a secret.
//
//  Ported from an earlier application framework's dotenv.hpp; ${NAME:?message} is new here. C++20 and
//  POSIX (it reads through fs/file.hpp), opt-in like fs/.
//

#ifndef ts_moveables_process_dotenv_hpp
#define ts_moveables_process_dotenv_hpp

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../fs/file.hpp"

namespace snicholls {

// A malformed .env file or config value, missing required keys. The message names the source and line, never a value
class config_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};


struct dotenv_entry
{
    std::string key;
    std::string value;
    std::size_t line = 0;   ///< 1-based line where the entry starts
};

using dotenv_lookup = std::function<std::optional<std::string>(std::string_view name)>;

namespace detail::env {

class dotenv_parser
{
public:
    dotenv_parser(std::string_view text, std::string_view source, const dotenv_lookup& lookup)
        : text_(text), source_(source), lookup_(lookup)
    {
        if (text_.starts_with("\xEF\xBB\xBF"))
            text_.remove_prefix(3);
    }

    std::vector<dotenv_entry> parse()
    {
        std::vector<dotenv_entry> entries;
        while (pos_ < text_.size())
        {
            skip_blanks();
            if (at_end_of_line())
            {
                next_line();
                continue;
            }
            if (peek() == '#')
            {
                skip_to_end_of_line();
                next_line();
                continue;
            }
            const std::size_t start_line = line_;
            std::string key = parse_key();
            skip_blanks();
            if (pos_ >= text_.size() || peek() != '=')
                fail(start_line, "expected KEY=VALUE after key '" + key + "'");
            ++pos_;
            skip_blanks();
            std::string value = parse_value(start_line);
            defined_[key] = value;
            entries.push_back({std::move(key), std::move(value), start_line});
            next_line();
        }
        return entries;
    }

private:
    [[noreturn]] void fail(std::size_t line, const std::string& what) const
    {
        throw config_error(std::string(source_) + ":" + std::to_string(line) + ": " + what);
    }

    char peek() const noexcept { return text_[pos_]; }

    bool at_end_of_line() const noexcept
    {
        return pos_ >= text_.size() || text_[pos_] == '\n' || (text_[pos_] == '\r' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '\n') ||
               (text_[pos_] == '\r' && pos_ + 1 == text_.size());
    }

    void skip_blanks() noexcept
    {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t'))
            ++pos_;
    }

    void skip_to_end_of_line() noexcept
    {
        while (!at_end_of_line())
            ++pos_;
    }

    // Consumes the line terminator (if any).
    void next_line() noexcept
    {
        if (pos_ < text_.size() && text_[pos_] == '\r')
            ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '\n')
        {
            ++pos_;
            ++line_;
        }
    }

    static bool key_start(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
    static bool key_char(char c) noexcept
    {
        return key_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
    }

    std::string parse_key()
    {
        if (text_.substr(pos_).starts_with("export") && pos_ + 6 < text_.size() &&
            (text_[pos_ + 6] == ' ' || text_[pos_ + 6] == '\t'))
        {
            pos_ += 6;
            skip_blanks();
        }
        if (pos_ >= text_.size() || !key_start(peek()))
            fail(line_, "expected a key ([A-Za-z_][A-Za-z0-9_.-]*)");
        const std::size_t begin = pos_;
        while (pos_ < text_.size() && key_char(peek()))
            ++pos_;
        return std::string(text_.substr(begin, pos_ - begin));
    }

    // After a closing quote only blanks and an optional comment may follow.
    void expect_line_end(std::size_t start_line)
    {
        skip_blanks();
        if (pos_ < text_.size() && peek() == '#')
            skip_to_end_of_line();
        if (!at_end_of_line())
            fail(start_line, "unexpected characters after closing quote");
    }

    std::string parse_value(std::size_t start_line)
    {
        if (pos_ < text_.size() && peek() == '\'')
        {
            ++pos_;
            const std::size_t close = text_.find('\'', pos_);
            if (close == std::string_view::npos)
                fail(start_line, "unterminated single-quoted value");
            const std::string_view raw = text_.substr(pos_, close - pos_);
            for (const char c : raw)
                line_ += c == '\n' ? 1 : 0;
            std::string value = normalise_newlines(raw);
            pos_ = close + 1;
            expect_line_end(start_line);
            return value;
        }
        if (pos_ < text_.size() && peek() == '"')
        {
            ++pos_;
            std::string value;
            for (;;)
            {
                if (pos_ >= text_.size())
                    fail(start_line, "unterminated double-quoted value");
                const char c = text_[pos_];
                if (c == '"')
                {
                    ++pos_;
                    break;
                }
                if (c == '\\' && pos_ + 1 < text_.size())
                {
                    const char e = text_[pos_ + 1];
                    pos_ += 2;
                    switch (e)
                    {
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    case '$': value += '$'; break;
                    default:
                        value += '\\';
                        value += e;
                    }
                    continue;
                }
                if (c == '$' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '{')
                {
                    value += interpolate(start_line);
                    continue;
                }
                if (c == '\r' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '\n')
                {
                    ++pos_;
                    continue;
                }
                if (c == '\n')
                    ++line_;
                value += c;
                ++pos_;
            }
            expect_line_end(start_line);
            return value;
        }
        // Unquoted: to end of line, " #" starts a comment, trailing blanks trimmed.
        std::string value;
        while (!at_end_of_line())
        {
            const char c = peek();
            if (c == '#' && (value.empty() || value.back() == ' ' || value.back() == '\t'))
            {
                skip_to_end_of_line();
                break;
            }
            if (c == '$' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '{')
            {
                value += interpolate(start_line);
                continue;
            }
            value += c;
            ++pos_;
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.pop_back();
        return value;
    }

    // At "${": NAME or NAME:-default. Undefined without a default is the empty string (shell semantics).
    std::string interpolate(std::size_t start_line)
    {
        const std::size_t close = text_.find('}', pos_ + 2);
        const std::size_t newline = text_.find('\n', pos_ + 2);
        if (close == std::string_view::npos || (newline != std::string_view::npos && newline < close))
            fail(start_line, "unterminated ${...}");
        const std::string_view inner = text_.substr(pos_ + 2, close - pos_ - 2);
        pos_ = close + 1;
        std::string_view name = inner;
        std::optional<std::string_view> fallback;
        std::optional<std::string_view> required;   // ${NAME:?message}: unset or empty is an error
        if (const std::size_t dash = inner.find(":-"); dash != std::string_view::npos)
        {
            name = inner.substr(0, dash);
            fallback = inner.substr(dash + 2);
        }
        else if (const std::size_t q = inner.find(":?"); q != std::string_view::npos)
        {
            name = inner.substr(0, q);
            required = inner.substr(q + 2);
        }
        if (name.empty() || !key_start(name.front()))
            fail(start_line, "invalid name in ${...}");
        const bool empty_counts_as_unset = fallback.has_value() || required.has_value();
        if (const auto it = defined_.find(std::string(name));
            it != defined_.end() && !(it->second.empty() && empty_counts_as_unset))
            return it->second;
        if (lookup_)
            if (std::optional<std::string> found = lookup_(name); found && !(found->empty() && empty_counts_as_unset))
                return *found;
        if (required)
            fail(start_line, std::string(name) + " is not set" +
                                 (required->empty() ? std::string() : ": " + std::string(*required)));
        return fallback ? std::string(*fallback) : std::string();
    }

    static std::string normalise_newlines(std::string_view in)
    {
        std::string out;
        out.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); ++i)
            if (!(in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n'))
                out += in[i];
        return out;
    }

    std::string_view text_;
    std::string_view source_;
    const dotenv_lookup& lookup_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::unordered_map<std::string, std::string> defined_;
};

} // namespace detail::env

[[nodiscard]] inline std::vector<dotenv_entry> parse_dotenv(std::string_view text, std::string_view source = "<dotenv>",
                                                            const dotenv_lookup& lookup = {})
{
    return detail::env::dotenv_parser(text, source, lookup).parse();
}

[[nodiscard]] inline std::optional<std::vector<dotenv_entry>> read_dotenv_file(const std::filesystem::path& path,
                                                                               const dotenv_lookup& lookup = {})
{
    auto text = read_file(path);
    if (text.is_err()) {
        if (text.unwrap_err_unchecked() == std::errc::no_such_file_or_directory)
            return std::nullopt;
        throw config_error("cannot read " + path.string() + ": " + text.unwrap_err_unchecked().message());
    }
    return parse_dotenv(*text, path.string(), lookup);
}

} // namespace snicholls

#endif // ts_moveables_process_dotenv_hpp
