#include <minify/Minify.h>
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace minify {
namespace {

bool ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

std::string shorten_js_integer(const std::string& token) {
    if (token.size() < 3 || token[0] != '0') return token;
    unsigned base = 0;
    if (token[1] == 'x' || token[1] == 'X') base = 16;
    else if (token[1] == 'b' || token[1] == 'B') base = 2;
    else if (token[1] == 'o' || token[1] == 'O') base = 8;
    else return token;

    std::uint64_t value = 0;
    for (std::size_t i = 2; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return token;  // Separators, BigInt suffixes and malformed input.
        if (digit >= base || value > (UINT64_MAX - digit) / base) return token;
        value = value * base + digit;
    }
    const std::string decimal = std::to_string(value);
    return decimal.size() < token.size() ? decimal : token;
}

std::string quote_js_string(const std::string& token, char target) {
    const char source = token.front();
    std::string result;
    result.reserve(token.size() + 2);
    result.push_back(target);
    for (std::size_t i = 1; i + 1 < token.size(); ++i) {
        const char c = token[i];
        if (c == '\\' && i + 2 < token.size()) {
            const char escaped = token[++i];
            if (escaped == '\'' || escaped == '"') {
                if (escaped == target) result.push_back('\\');
                result.push_back(escaped);
            } else {
                result.push_back('\\');
                result.push_back(escaped);
            }
        } else {
            if (c == target && target != source) result.push_back('\\');
            result.push_back(c);
        }
    }
    result.push_back(target);
    return result;
}

std::string shorten_js_string(const std::string& token) {
    const std::string single = quote_js_string(token, '\'');
    const std::string dual = quote_js_string(token, '"');
    if (single.size() < dual.size()) return single;
    if (dual.size() < single.size()) return dual;
    return token.front() == '\'' ? single : dual;
}

enum class JsTokenKind { Identifier, Punctuator, Number, String, Regex, Template };

struct JsToken {
    JsTokenKind kind;
    std::string text;
    std::size_t begin;
    std::size_t end;
};

struct JsReplacement {
    std::size_t begin;
    std::size_t end;
    std::string text;
};

std::size_t matching_js_token(const std::vector<JsToken>& tokens,
                              std::size_t open, const char* left,
                              const char* right) {
    std::size_t depth = 0;
    for (std::size_t i = open; i < tokens.size(); ++i) {
        if (tokens[i].text == left) ++depth;
        else if (tokens[i].text == right && --depth == 0) return i;
    }
    return tokens.size();
}

std::string js_short_name(std::size_t index) {
    if (index == 0) return "$";
    if (index == 1) return "_";
    index -= 2;
    if (index < 26) return std::string(1, static_cast<char>('a' + index));
    index -= 26;
    if (index < 26) return std::string(1, static_cast<char>('A' + index));
    index -= 26;
    return std::string(1, static_cast<char>('a' + (index / 26) % 26)) +
           static_cast<char>('a' + index % 26);
}

[[maybe_unused]] std::vector<JsReplacement> plan_js_parameter_mangling(
    const std::vector<JsToken>& tokens) {
    std::vector<JsReplacement> replacements;
    static const std::unordered_set<std::string> unsafe_scope_words = {
        "eval", "with", "arguments", "catch", "class", "let", "const"
    };
    static const std::unordered_set<std::string> reserved_words = {
        "await", "break", "case", "catch", "class", "const", "continue",
        "debugger", "default", "delete", "do", "else", "enum", "export",
        "extends", "false", "finally", "for", "function", "if", "import",
        "in", "instanceof", "let", "new", "null", "return", "static",
        "super", "switch", "this", "throw", "true", "try", "typeof",
        "var", "void", "while", "with", "yield"
    };

    for (std::size_t f = 0; f < tokens.size(); ++f) {
        if (tokens[f].text != "function") continue;
        std::size_t cursor = f + 1;
        if (cursor < tokens.size() && tokens[cursor].text == "*") ++cursor;
        if (cursor < tokens.size() && tokens[cursor].text != "(") ++cursor;
        if (cursor >= tokens.size() || tokens[cursor].text != "(") continue;
        const std::size_t close_params = matching_js_token(tokens, cursor, "(", ")");
        if (close_params == tokens.size() || close_params + 1 >= tokens.size() ||
            tokens[close_params + 1].text != "{") continue;
        const std::size_t close_body =
            matching_js_token(tokens, close_params + 1, "{", "}");
        if (close_body == tokens.size()) continue;

        std::vector<std::string> params;
        bool simple_params = true;
        for (std::size_t p = cursor + 1; p < close_params; ++p) {
            if ((p - cursor) % 2 == 1) {
                const std::string& name = tokens[p].text;
                if (name.empty() || reserved_words.count(name) != 0 ||
                    !(std::isalpha(static_cast<unsigned char>(name[0])) ||
                      name[0] == '_' || name[0] == '$')) {
                    simple_params = false;
                    break;
                }
                params.push_back(name);
            } else if (tokens[p].text != ",") {
                simple_params = false;
                break;
            }
        }
        if (!simple_params || params.empty()) continue;

        bool unsafe_scope = false;
        std::unordered_set<std::string> occupied;
        for (const auto& param : params) occupied.insert(param);
        for (std::size_t p = close_params + 2; p < close_body; ++p) {
            occupied.insert(tokens[p].text);
            if (unsafe_scope_words.count(tokens[p].text) != 0 ||
                tokens[p].text == "function" ||
                (tokens[p].text == "=" && p + 1 < close_body &&
                 tokens[p + 1].text == ">")) {
                unsafe_scope = true;
            }
        }
        if (unsafe_scope) continue;

        std::size_t next_name = 0;
        std::unordered_set<std::string> handled;
        for (std::size_t parameter = 0; parameter < params.size(); ++parameter) {
            const std::string& original = params[parameter];
            if (!handled.insert(original).second) continue;
            std::string replacement;
            do replacement = js_short_name(next_name++);
            while (occupied.count(replacement) != 0 ||
                   reserved_words.count(replacement) != 0);
            if (replacement.size() >= original.size()) continue;

            bool shorthand = false;
            for (std::size_t p = close_params + 2; p < close_body; ++p) {
                if (tokens[p].text != original) continue;
                const std::string previous = p ? tokens[p - 1].text : std::string();
                const std::string next = p + 1 < tokens.size()
                    ? tokens[p + 1].text : std::string();
                if ((previous == "{" || previous == ",") &&
                    (next == "}" || next == ",")) shorthand = true;
            }
            if (shorthand) continue;

            for (std::size_t declaration = 0; declaration < params.size(); ++declaration) {
                if (params[declaration] == original) {
                    const JsToken& token = tokens[cursor + 1 + declaration * 2];
                    replacements.push_back({token.begin, token.end, replacement});
                }
            }
            for (std::size_t p = close_params + 2; p < close_body; ++p) {
                if (tokens[p].text != original) continue;
                const std::string previous = p ? tokens[p - 1].text : std::string();
                const std::string next = p + 1 < tokens.size()
                    ? tokens[p + 1].text : std::string();
                if (previous == "." || previous == "#" || previous == "break" ||
                    previous == "continue" || next == ":" ||
                    (next == "(" && (previous == "{" || previous == "," ||
                                     previous == "get" || previous == "set" ||
                                     previous == "async" || previous == "*"))) continue;
                replacements.push_back(
                    {tokens[p].begin, tokens[p].end, replacement});
            }
            occupied.insert(replacement);
        }
        f = close_body;
    }
    return replacements;
}

bool js_line_terminator_in(const std::string& input, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        if (input[i] == '\n' || input[i] == '\r') return true;
        if (i + 2 < end && static_cast<unsigned char>(input[i]) == 0xe2 &&
            static_cast<unsigned char>(input[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(input[i + 2]) == 0xa8 ||
             static_cast<unsigned char>(input[i + 2]) == 0xa9)) return true;
    }
    return false;
}

bool copy_template_literal(const std::string& input, std::size_t& i,
                           std::string& output, std::string& error,
                           const std::function<void(JsTokenKind, std::size_t,
                                                    std::size_t)>& record = {}) {
    struct Frame {
        bool expression;
        std::size_t braces;
        bool can_start_regex;
        std::size_t segment_begin;
    };
    std::vector<Frame> stack;
    const std::size_t template_begin = i;
    output.push_back(input[i++]);
    stack.push_back({false, 0, true, template_begin});
    while (i < input.size()) {
        char c = input[i++];
        output.push_back(c);
        Frame& frame = stack.back();
        if (!frame.expression) {
            if (c == '\\' && i < input.size()) output.push_back(input[i++]);
            else if (c == '`') {
                if (record) record(JsTokenKind::Template, frame.segment_begin, i);
                stack.pop_back();
                if (stack.empty()) return true;
            } else if (c == '$' && i < input.size() && input[i] == '{') {
                output.push_back(input[i++]);
                if (record) record(JsTokenKind::Template, frame.segment_begin, i);
                frame.expression = true;
                frame.braces = 1;
                frame.can_start_regex = true;
            }
            continue;
        }
        if (ws(c)) {
            // Formatting whitespace between tokens preserves the regex
            // context; only the surrounding tokens change it.
            continue;
        }
        if (c == '\'' || c == '"') {
            const std::size_t begin = i - 1;
            const char quote = c;
            bool escaped = false;
            while (i < input.size()) {
                char q = input[i++]; output.push_back(q);
                if (escaped) escaped = false;
                else if (q == '\\') escaped = true;
                else if (q == quote) break;
            }
            if (record) record(JsTokenKind::String, begin, i);
            frame.can_start_regex = false;
        } else if (c == '`') {
            stack.push_back({false, 0, true, i - 1});
        } else if (c == '/' && i < input.size() && input[i] == '/') {
            output.push_back(input[i++]);
            while (i < input.size()) { char q=input[i++]; output.push_back(q); if (q=='\n'||q=='\r') break; }
            frame.can_start_regex = true;
        } else if (c == '/' && i < input.size() && input[i] == '*') {
            output.push_back(input[i++]);
            while (i < input.size()) { char q=input[i++]; output.push_back(q); if (q=='*'&&i<input.size()&&input[i]=='/') { output.push_back(input[i++]); break; } }
            // A block comment leaves the regex context unchanged.
        } else if (c == '/' && frame.can_start_regex) {
            const std::size_t begin = i - 1;
            // A regular-expression literal is copied verbatim so that `//`,
            // `{`, `}` and backticks inside it cannot corrupt expression or
            // template frame state.
            bool escaped = false, in_class = false, closed = false;
            while (i < input.size()) {
                char q = input[i++]; output.push_back(q);
                if (escaped) { escaped = false; continue; }
                if (q == '\\') { escaped = true; continue; }
                if (q == '[') in_class = true;
                else if (q == ']') in_class = false;
                else if (q == '/' && !in_class) { closed = true; break; }
                else if (q == '\n' || q == '\r') break;
            }
            if (!closed) { error = "unterminated JavaScript regular expression"; output.clear(); return false; }
            while (i < input.size() && std::isalpha(static_cast<unsigned char>(input[i])))
                output.push_back(input[i++]);
            if (record) record(JsTokenKind::Regex, begin, i);
            frame.can_start_regex = false;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            const std::size_t begin = i - 1;
            while (i < input.size() &&
                   (std::isalnum(static_cast<unsigned char>(input[i])) ||
                    input[i] == '_' || input[i] == '$')) {
                output.push_back(input[i]);
                ++i;
            }
            static const std::unordered_set<std::string> prefix_words = {
                "return","throw","case","delete","void","typeof","new",
                "in","instanceof","yield","await","else","do"
            };
            frame.can_start_regex =
                prefix_words.count(input.substr(begin, i - begin)) != 0;
            if (record) record(JsTokenKind::Identifier, begin, i);
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t begin = i - 1;
            bool exponent = false;
            while (i < input.size()) {
                const unsigned char u = static_cast<unsigned char>(input[i]);
                const char q = input[i];
                if (std::isalnum(u) || q == '.' || q == '_') {
                    exponent = q == 'e' || q == 'E';
                    output.push_back(q);
                    ++i;
                    continue;
                }
                if ((q == '+' || q == '-') && exponent) {
                    exponent = false;
                    output.push_back(q);
                    ++i;
                    continue;
                }
                break;
            }
            if (record) record(JsTokenKind::Number, begin, i);
            frame.can_start_regex = false;
        } else if (c == '{') {
            if (record) record(JsTokenKind::Punctuator, i - 1, i);
            ++frame.braces;
            frame.can_start_regex = true;
        } else if (c == '}' && --frame.braces == 0) {
            frame.expression = false;
            frame.can_start_regex = false;
            frame.segment_begin = i - 1;
        } else if (c == ';' || c == ',' || c == ':' || c == '(' || c == '[' ||
                   c == '=' || c == '!' || c == '?' || c == '&' || c == '|' ||
                   c == '+' || c == '-' || c == '*' || c == '%' || c == '<' ||
                   c == '>' || c == '~' || c == '^') {
            if (record) record(JsTokenKind::Punctuator, i - 1, i);
            frame.can_start_regex = true;
        } else {
            if (record) record(JsTokenKind::Punctuator, i - 1, i);
            frame.can_start_regex = false;
        }
    }
    error = "unterminated JavaScript template literal";
    output.clear();
    return false;
}

bool word_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Be deliberately conservative for UTF-8: non-ASCII bytes may belong to a
    // JavaScript/CSS identifier, so removing adjacent whitespace could merge
    // tokens even though this lightweight scanner does not decode Unicode IDs.
    return u >= 0x80 || std::isalnum(u) || c == '_' || c == '$' || c == '-' || c == '\\';
}

bool js_needs_separator(char left, char right, bool after_regex,
                        bool preserve_jsx_boundary) {
    const unsigned char next = static_cast<unsigned char>(right);
    return (word_char(left) && word_char(right)) ||
           (left == '+' && right == '+') ||
           (left == '-' && right == '-') ||
           (left == '/' && (right == '/' || right == '*')) ||
           (after_regex && (std::isalpha(next) || right == '_' || right == '$' ||
                            next >= 0x80)) ||
           (left == '*' && right == '/') ||
           (preserve_jsx_boundary && left == '<' &&
            (std::isalpha(next) || right == '>' || right == '/')) ||
           (std::isdigit(static_cast<unsigned char>(left)) && right == '.');
}

enum class JsLineTerminatorAction {
    DropAfterDelimiter,
    PreserveRestrictedProduction,
    PreserveExpressionContinuation,
    PreserveAsiBoundary
};

JsLineTerminatorAction classify_js_line_terminator(char left,
                                                   const std::string& previous_token,
                                                   char next) {
    if (left == ';' || left == '{')
        return JsLineTerminatorAction::DropAfterDelimiter;
    static const std::unordered_set<std::string> restricted = {
        "return", "throw", "break", "continue", "yield", "await", "async"
    };
    if (restricted.count(previous_token) != 0 || next == '+' || next == '-')
        return JsLineTerminatorAction::PreserveRestrictedProduction;
    if (next == '(' || next == '[' || next == '/' || next == '`' || next == '.')
        return JsLineTerminatorAction::PreserveExpressionContinuation;
    return JsLineTerminatorAction::PreserveAsiBoundary;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool starts_ci(const std::string& s, std::size_t pos, const std::string& needle) {
    if (pos + needle.size() > s.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
            std::tolower(static_cast<unsigned char>(needle[i]))) return false;
    return true;
}

bool css_needs_space(char left, char right) {
    if ((word_char(left) && word_char(right)) ||
        (left == '/' && right == '*') || (left == '*' && right == '/')) return true;

    // In CSS nesting, authored whitespace after '&' is the descendant
    // combinator. Preserve it before a following compound selector.
    if (left == '&' && (word_char(right) || right == '.' || right == '#' ||
                        right == '[' || right == '*' || right == ':' || right == '\\')) return true;

    // The nesting selector can also appear on the right side of a descendant
    // combinator (`.ancestor &`). Joining it to the preceding selector changes
    // the compound selector just as surely as joining `& .child` does.
    if (right == '&' && (word_char(left) || left == ')' || left == ']' || left == '*')) return true;

    // A leading decimal, class/ID/attribute selector, universal selector or
    // parenthesized construct can begin a distinct CSS token even though it is
    // not word-like. Joining it to the previous token can invalidate a value
    // list or turn a descendant selector into a compound selector.
    if ((word_char(left) || left == '*' || left == '\'' || left == '"') &&
        (right == '.' || right == '#' || right == '[' || right == '(' || right == '*')) return true;

    // Function/attribute/percentage results followed by another value token
    // require separation (for example transform lists and color percentages).
    if ((left == ')' || left == ']' || left == '%' || left == '*' ||
         left == '\'' || left == '"') &&
        (word_char(right) || right == '.' || right == '#' || right == '[' ||
         right == '(' || right == '*' || right == '\'' || right == '"')) return true;

    // Adjacent strings are separate component values, as are identifiers or
    // functions followed by a string (font-family and generated-content lists
    // are common examples). Concatenating them is not CSS token compaction.
    if ((word_char(left) || left == ')' || left == ']' || left == '%') &&
        (right == '\'' || right == '"')) return true;

    return false;
}

void emit_pending_css_space(std::string& out, bool& pending, char next) {
    if (!pending) return;
    if (!out.empty() && !ws(out.back()) && css_needs_space(out.back(), next)) out.push_back(' ');
    pending = false;
}

bool css_colon_precedes_rule_block(const std::string& input, std::size_t colon) {
    std::size_t parens = 0;
    std::size_t brackets = 0;
    bool quoted = false;
    bool escaped = false;
    char quote = 0;
    for (std::size_t i = colon + 1; i < input.size(); ++i) {
        const char c = input[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quoted = false;
            continue;
        }
        if (c == '\'' || c == '"') { quoted = true; quote = c; continue; }
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            const auto end = input.find("*/", i + 2);
            if (end == std::string::npos) return false;
            i = end + 1;
            continue;
        }
        if (c == '(') { ++parens; continue; }
        if (c == ')' && parens) { --parens; continue; }
        if (c == '[') { ++brackets; continue; }
        if (c == ']' && brackets) { --brackets; continue; }
        if (parens || brackets) continue;
        if (c == '{') return true;
        if (c == ';' || c == '}') return false;
    }
    return false;
}

} // namespace

bool json(const std::string& input, std::string& output, std::string& error) {
    json::Document document;
    if (!json::Document::parse(input, document, error)) {
        error = "invalid JSON: " + error;
        output.clear();
        return false;
    }

    output.clear();
    output.reserve(input.size());
    bool quoted = false;
    bool escaped = false;
    for (char c : input) {
        if (quoted) {
            output.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
            output.push_back(c);
        } else if (!ws(c)) {
            output.push_back(c);
        }
    }
    error.clear();
    return true;
}

bool css(const std::string& input, std::string& output, std::string& error) {
    output.clear();
    output.reserve(input.size());
    bool pending_space = false;
    bool preserve_final_bad_string_newline = false;
    std::size_t bracket_depth = 0;
    std::vector<bool> brace_is_component_value;
    bool just_closed_component_value = false;
    // CSS escape whitespace tracking. A hex escape (backslash + 1..6 hex
    // digits) consumes one trailing whitespace as its terminator, and an
    // escaped whitespace character is an identifier character, not removable
    // formatting. Collapsing the whitespace run after either merges what a
    // browser tokenizes as separate identifiers into a single identifier.
    bool in_hex_escape = false;
    unsigned hex_escape_digits = 0;
    bool escaped_ws = false;

    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i];

        if (c == '\'' || c == '"') {
            emit_pending_css_space(output, pending_space, c);
            in_hex_escape = false;
            hex_escape_digits = 0;
            escaped_ws = false;
            const char quote = c;
            output.push_back(c);
            ++i;
            bool escaped = false;
            while (i < input.size()) {
                const char q = input[i++];
                output.push_back(q);
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (q == '\\') {
                    escaped = true;
                    continue;
                }
                if (q == quote) break;
                // CSS Syntax recovers an unescaped newline as a bad-string
                // token rather than making the whole stylesheet invalid. The
                // newline itself is significant because it terminates the
                // token, so preserve it and resume scanning after it.
                if (q == '\n' || q == '\r' || q == '\f') {
                    preserve_final_bad_string_newline = i == input.size();
                    break;
                }
            }
            // EOF also terminates a CSS string token with a parse error but
            // without rejecting the stylesheet. Preserve the recoverable
            // source rather than turning browser-accepted CSS into a Minify++
            // hard error.
            continue;
        }

        // A CSS escape consumes the following code point. Copy it as a unit
        // so escaped whitespace is not mistaken for removable formatting.
        if (c == '\\' && i + 1 < input.size()) {
            emit_pending_css_space(output, pending_space, c);
            output.push_back(c);
            const char escaped = input[i + 1];
            output.push_back(escaped);
            if (escaped == ' ' || escaped == '\t' || escaped == '\f') {
                // An escaped whitespace character is an identifier code point.
                escaped_ws = true;
                in_hex_escape = false;
                hex_escape_digits = 0;
            } else if (std::isxdigit(static_cast<unsigned char>(escaped))) {
                // Hex escape: track following digits so a trailing whitespace
                // terminator and any separator can be preserved.
                in_hex_escape = true;
                hex_escape_digits = 1;
                escaped_ws = false;
            } else {
                in_hex_escape = false;
                hex_escape_digits = 0;
                escaped_ws = false;
            }
            i += 2;
            continue;
        }

        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            in_hex_escape = false;
            hex_escape_digits = 0;
            escaped_ws = false;
            const bool preserve = i + 2 < input.size() && input[i + 2] == '!';
            const auto end = input.find("*/", i + 2);
            if (end == std::string::npos) {
                // CSS comments are implicitly closed by EOF. Ordinary
                // comments can therefore be removed to EOF; preserved license
                // comments remain byte-for-byte intact and are likewise
                // browser-recoverable without a closing delimiter.
                if (preserve) {
                    emit_pending_css_space(output, pending_space, '/');
                    output.append(input, i, std::string::npos);
                } else {
                    pending_space = true;
                }
                i = input.size();
                continue;
            }
            if (preserve) {
                emit_pending_css_space(output, pending_space, '/');
                output.append(input, i, end + 2 - i);
            } else {
                pending_space = true;
            }
            i = end + 2;
            continue;
        }

        if (ws(c)) {
            if (in_hex_escape || escaped_ws) {
                // A hex escape consumes the first whitespace as its terminator;
                // any further whitespace is a real token separator. An escaped
                // whitespace character is itself an identifier character, so
                // the whitespace following it is a separator. Collapsing this
                // run to a single space would merge distinct browser tokens.
                std::size_t run = 1;
                while (i + run < input.size() && ws(input[i + run])) ++run;
                if (in_hex_escape) {
                    output.push_back(' ');
                    if (run >= 2) output.push_back(' ');
                } else {
                    output.push_back(' ');
                }
                pending_space = false;
                in_hex_escape = false;
                hex_escape_digits = 0;
                escaped_ws = false;
                i += run;
                continue;
            }
            pending_space = true;
            ++i;
            continue;
        }

        const bool punctuation = c == '{' || c == '}' || c == ':' ||
                                 c == ';' || c == ',';
        if (punctuation) {
            in_hex_escape = false;
            hex_escape_digits = 0;
            escaped_ws = false;
            // Whitespace before ':' can distinguish a descendant pseudo-class
            // selector (`.a :hover`) from a compound selector (`.a:hover`).
            // Preserve authored spacing universally rather than guessing
            // declaration context in this intentionally lightweight scanner.
            const bool preserve_before_colon = c == ':' && pending_space &&
                                               css_colon_precedes_rule_block(input, i) &&
                                               !output.empty() && output.back() != ' ';
            const bool preserve_before_block = c == '{' && pending_space &&
                                               !output.empty() && output.back() == ')';
            pending_space = false;
            while (!output.empty() && output.back() == ' ') output.pop_back();
            if (preserve_before_colon || preserve_before_block) output.push_back(' ');
            const bool opens_component_value = c == '{' && !output.empty() && output.back() == ':';
            output.push_back(c);
            if (c == '{') {
                brace_is_component_value.push_back(opens_component_value);
                just_closed_component_value = false;
            } else if (c == '}') {
                just_closed_component_value = !brace_is_component_value.empty() && brace_is_component_value.back();
                if (!brace_is_component_value.empty()) brace_is_component_value.pop_back();
            } else {
                just_closed_component_value = false;
            }
        } else {
            if (in_hex_escape) {
                if (std::isxdigit(static_cast<unsigned char>(c)) && hex_escape_digits < 6) {
                    ++hex_escape_digits;
                } else {
                    in_hex_escape = false;
                    hex_escape_digits = 0;
                }
            }
            escaped_ws = false;
            if (pending_space && just_closed_component_value && !output.empty() && output.back() == '}') {
                output.push_back(' ');
                pending_space = false;
            }

            // Whitespace around an attribute-selector namespace separator can
            // distinguish a valid selector from an invalid one.
            if (pending_space && bracket_depth != 0 &&
                (c == '|' || (!output.empty() && output.back() == '|'))) {
                if (!output.empty() && output.back() != ' ') output.push_back(' ');
                pending_space = false;
            }

            // CSS math functions require whitespace around binary + and -.
            // Preserve authored whitespace adjacent to these operators rather
            // than trying to parse the full evolving CSS value grammar.
            if (pending_space && (c == '+' || c == '-') && !output.empty() && output.back() != ' ')
                output.push_back(' ');
            else if (pending_space && !output.empty() &&
                     (output.back() == '+' || output.back() == '-') && output.back() != ' ')
                output.push_back(' ');
            else
                emit_pending_css_space(output, pending_space, c);
            pending_space = false;
            output.push_back(c);
            if (c == '[') ++bracket_depth;
            else if (c == ']' && bracket_depth != 0) --bracket_depth;
            just_closed_component_value = false;
        }
        ++i;
    }

    if (!preserve_final_bad_string_newline) {
        // An escaped trailing whitespace is an identifier character and must
        // survive the trim; only removable formatting whitespace is dropped.
        while (!output.empty() && ws(output.back()) &&
               !(output.size() >= 2 && output[output.size() - 2] == '\\')) {
            output.pop_back();
        }
    }
    error.clear();
    return true;
}

static bool follows_attribute_equals(const std::string& input, std::size_t pos,
                                     std::size_t tag_start) {
    while (pos > tag_start + 1 && ws(input[pos - 1])) --pos;
    return pos > tag_start + 1 && input[pos - 1] == '=';
}

static bool tag_has_preserved_whitespace_style(const std::string& tag) {
    std::string compact;
    compact.reserve(tag.size());
    for (unsigned char c : tag)
        if (!ws(static_cast<char>(c))) compact.push_back(static_cast<char>(std::tolower(c)));
    const auto p = compact.find("white-space:");
    if (p == std::string::npos) return false;
    const auto value = p + 12;
    return compact.compare(value, 3, "pre") == 0 ||
           compact.compare(value, 12, "break-spaces") == 0;
}

bool html(const std::string& input, std::string& output, std::string& error) {
    output.clear();
    output.reserve(input.size());

    bool pending_space = false;
    std::string raw_tag;
    bool raw_tag_can_nest = false;
    std::size_t raw_tag_depth = 0;
    bool preserve_final_whitespace = false;

    for (std::size_t i = 0; i < input.size();) {
        if (!raw_tag.empty()) {
            const std::string close = "</" + raw_tag;
            std::size_t p = i;
            bool found = false;
            for (; p < input.size(); ++p) {
                if (raw_tag_can_nest && starts_ci(input, p, "<" + raw_tag)) {
                    const std::size_t after = p + raw_tag.size() + 1;
                    if (after < input.size() && (input[after] == '>' || input[after] == '/' || ws(input[after])))
                        ++raw_tag_depth;
                }
                if (starts_ci(input, p, close)) {
                    const std::size_t after = p + close.size();
                    if (after < input.size() && (input[after] == '>' || ws(input[after]))) {
                        if (raw_tag_can_nest && raw_tag_depth > 1) {
                            --raw_tag_depth;
                        } else {
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                output.append(input, i, std::string::npos);
                preserve_final_whitespace = true;
                i = input.size();
                break;
            }
            output.append(input, i, p - i);
            i = p;
            raw_tag.clear();
            raw_tag_can_nest = false;
            raw_tag_depth = 0;
            continue;
        }

        if (input.compare(i, 4, "<!--") == 0) {
            const auto end = input.find("-->", i + 4);
            if (end == std::string::npos) {
                // HTML recovers an ordinary comment at EOF. Preserve special
                // conditional/SSI forms; an ordinary trailing comment is
                // removable just like a terminated ordinary comment.
                const bool preserve = starts_ci(input, i, "<!--[if") ||
                                      input.compare(i, 5, "<!--#") == 0 ||
                                      input.compare(i, 5, "<!--!") == 0;
                if (preserve) output.append(input, i, std::string::npos);
                i = input.size();
                break;
            }
            const bool preserve = starts_ci(input, i, "<!--[if") ||
                                  input.compare(i, 5, "<!--#") == 0 ||
                                  input.compare(i, 5, "<!--!") == 0;
            if (preserve) {
                if (pending_space && !output.empty()) output.push_back(' ');
                pending_space = false;
                output.append(input, i, end + 3 - i);
            } else {
                // Ordinary comments are zero-width. Preserve whitespace that
                // actually occurred around the comment, but do not invent any.
            }
            i = end + 3;
            continue;
        }

        if (input[i] == '<') {
            // A second '<' cannot belong to the current tag opener. Browsers
            // emit the first one as text and reconsume the second, which may
            // begin a real raw-text element such as <<script>.
            if (i + 1 < input.size() && input[i + 1] == '<') {
                if (pending_space && !output.empty()) output.push_back(' ');
                pending_space = false;
                output.push_back('<');
                ++i;
                continue;
            }
            // Whitespace between elements is a real HTML text node and can
            // become observable when CSS changes display/layout. Collapse it,
            // but do not erase it simply because the next token is a tag.
            if (pending_space && !output.empty() && !ws(output.back()))
                output.push_back(' ');
            pending_space = false;

            std::size_t j = i + 1;
            bool quoted = false;
            char quote = 0;
            for (; j < input.size(); ++j) {
                const char c = input[j];
                if (quoted) {
                    if (c == '\\') ++j;
                    else if (c == quote) quoted = false;
                } else if ((c == '\'' || c == '"') && follows_attribute_equals(input, j, i)) {
                    // A quote only opens a quoted attribute value immediately
                    // after '='. Browsers keep a stray quote in an unquoted
                    // value as data, even though it is a parse error.
                    quoted = true; quote = c;
                } else if (c == '>') {
                    ++j;
                    break;
                }
            }
            if (quoted || j > input.size() || input[j - 1] != '>') {
                error = "unterminated HTML tag";
                output.clear();
                return false;
            }

            // Collapse whitespace inside tags, preserving quoted attribute values.
            bool tag_space = false;
            bool in_quote = false;
            char tag_quote = 0;
            for (std::size_t k = i; k < j; ++k) {
                char c = input[k];
                if (in_quote) {
                    output.push_back(c);
                    if (c == '\\' && k + 1 < j) output.push_back(input[++k]);
                    else if (c == tag_quote) in_quote = false;
                } else if ((c == '\'' || c == '"') && follows_attribute_equals(input, k, i)) {
                    if (tag_space && !output.empty() && output.back() != '<' && output.back() != ' ')
                        output.push_back(' ');
                    tag_space = false;
                    in_quote = true; tag_quote = c; output.push_back(c);
                } else if (ws(c)) {
                    tag_space = true;
                } else {
                    const bool after_tag_prefix = !output.empty() &&
                        (output.back() == '<' ||
                         (output.back() == '/' && output.size() >= 2 && output[output.size() - 2] == '<'));
                    if (tag_space && !output.empty() &&
                        (after_tag_prefix || c != '>'))
                        output.push_back(' ');
                    tag_space = false;
                    output.push_back(c);
                }
            }

            // Detect raw-text/preformatted elements from opening tags.
            std::size_t n = i + 1;
            while (n < j && ws(input[n])) ++n;
            if (n < j && input[n] != '/' && input[n] != '!' && input[n] != '?') {
                std::size_t e = n;
                while (e < j && (std::isalnum(static_cast<unsigned char>(input[e])) ||
                                 input[e] == '-' || input[e] == ':')) ++e;
                const std::string name = lower(input.substr(n, e - n));
                if (name == "pre" || name == "textarea" || name == "script" ||
                    name == "style" || name == "xmp" || name == "listing" ||
                    name == "iframe") {
                    raw_tag = name;
                } else if (name == "plaintext") {
                    raw_tag = name; // no closing tag exists; preserve to EOF
                } else if (name == "svg" || name == "math") {
                    // Foreign-content parsing has different script and
                    // self-closing rules. Preserve the subtree verbatim.
                    raw_tag = name;
                    raw_tag_can_nest = true;
                    raw_tag_depth = 1;
                } else if (!name.empty() && tag_has_preserved_whitespace_style(input.substr(i, j - i))) {
                    raw_tag = name;
                    raw_tag_can_nest = true;
                    raw_tag_depth = 1;
                }
            }
            i = j;
            continue;
        }

        if (ws(input[i])) {
            pending_space = true;
            ++i;
            continue;
        }

        if (pending_space && !output.empty()) output.push_back(' ');
        pending_space = false;
        output.push_back(input[i++]);
    }

    if (!preserve_final_whitespace)
        while (!output.empty() && ws(output.back())) output.pop_back();
    while (!output.empty() && ws(output.front())) output.erase(output.begin());
    error.clear();
    return true;
}

// JavaScript minification is intentionally conservative. It removes comments,
// redundant horizontal whitespace, and line terminators at boundaries which
// already carry an unambiguous statement/block delimiter. Other significant
// line terminators remain available to automatic semicolon insertion.
static bool minify_javascript(const std::string& input, std::string& output,
                              std::string& error, bool preserve_jsx_boundaries,
                              bool collect_tokens = false) {
    output.clear();
    output.reserve(input.size());

    bool pending_space = false;
    bool pending_newline = false;
    bool regex_boundary = false;
    bool can_start_regex = true;
    bool pending_control_paren = false;
    std::vector<bool> control_parens;
    std::vector<bool> block_braces;
    std::string last_token;
    [[maybe_unused]] std::vector<JsToken> tokens;
    // Structured and aggressive policy requests exercise the same non-mutating
    // inventory while those modes intentionally retain conservative output.
    // Positions refer to the source, never to a partially rewritten output.
    if (collect_tokens) tokens.reserve(input.size() / 4);
    auto record_token = [&](JsTokenKind kind, std::size_t begin, std::size_t end) {
        if (collect_tokens)
            tokens.push_back({kind, input.substr(begin, end - begin), begin, end});
    };
    std::string before_semicolon_token;
    bool pending_class_brace = false;
    bool pending_class_expression = false;
    bool pending_function_brace = false;
    bool pending_function_expression = false;
    bool pending_async_expression = false;

    auto emit_pending = [&](char next) {
        if (pending_newline) {
            const JsLineTerminatorAction action = output.empty()
                ? JsLineTerminatorAction::PreserveAsiBoundary
                : classify_js_line_terminator(output.back(), last_token, next);
            // A terminator after an explicit semicolon, an opening brace, or a
            // opening brace cannot supply ASI semantics. A closing brace is
            // not enough: it may close an async/function/class expression,
            // and the following line can depend on ASI. Preserve every other
            // newline, including all boundaries following `}`.
            if (action != JsLineTerminatorAction::DropAfterDelimiter &&
                !output.empty() && output.back() != '\n')
                output.push_back('\n');
        } else if (pending_space && !output.empty() &&
                   js_needs_separator(output.back(), next, regex_boundary,
                                      preserve_jsx_boundaries)) {
            output.push_back(' ');
        }
        pending_space = pending_newline = false;
        regex_boundary = false;
    };

    auto copy_quoted = [&](std::size_t& i, char quote) {
        emit_pending(quote);
        const std::size_t begin = i++;
        bool escaped = false;
        while (i < input.size()) {
            const char q = input[i++];
            if (escaped) escaped = false;
            else if (q == '\\') escaped = true;
            else if (q == quote) {
                const std::string token = input.substr(begin, i - begin);
                output += preserve_jsx_boundaries ? token : shorten_js_string(token);
                return true;
            }
        }
        error = std::string("unterminated JavaScript ") +
                (quote == '`' ? "template literal" : "string literal");
        output.clear();
        return false;
    };

    static const std::unordered_set<std::string> control_keywords = {
        "if", "while", "for", "with", "switch", "catch"
    };
    static const std::unordered_set<std::string> expression_prefix_keywords = {
        "return", "throw", "case", "delete", "void", "typeof", "new",
        "in", "instanceof", "yield", "await", "else", "do"
    };
    static const std::unordered_set<std::string> boolean_expression_prefixes = {
        "(", "[", "=", "!", "?", ":", "&", "|", "+", "-", "*", "%",
        "<", ">", "/", "return", "throw", "case", "delete", "void",
        "typeof", "in", "instanceof", "yield", "await"
    };

    auto next_nontrivia = [&](std::size_t position) {
        while (position < input.size()) {
            if (ws(input[position])) {
                ++position;
            } else if (position + 1 < input.size() && input[position] == '/' &&
                       input[position + 1] == '/') {
                position += 2;
                while (position < input.size() && input[position] != '\n' &&
                       input[position] != '\r') ++position;
            } else if (position + 1 < input.size() && input[position] == '/' &&
                       input[position + 1] == '*') {
                const auto close = input.find("*/", position + 2);
                if (close == std::string::npos) return input.size();
                position = close + 2;
            } else {
                break;
            }
        }
        return position;
    };

    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i];

        if (ws(c)) {
            if (c == '\n' || c == '\r') pending_newline = true;
            else pending_space = true;
            ++i;
            continue;
        }

        // ASCII identifier/keyword token. Non-ASCII identifier bytes are kept
        // byte-for-byte by the generic path below.
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            const std::size_t begin = i++;
            while (i < input.size()) {
                const unsigned char u = static_cast<unsigned char>(input[i]);
                if (!std::isalnum(u) && input[i] != '_' && input[i] != '$') break;
                ++i;
            }
            const std::string word = input.substr(begin, i - begin);
            const bool boolean_literal = word == "true" || word == "false";
            const std::size_t following = boolean_literal ? next_nontrivia(i) : input.size();
            const char following_char = following < input.size() ? input[following] : '\0';
            const bool binds_more_tightly =
                following_char == '.' || following_char == '[' ||
                following_char == '(' || following_char == '`' ||
                (following_char == '?' && following + 1 < input.size() &&
                 input[following + 1] == '.');
            const bool boolean_expression =
                boolean_literal && !preserve_jsx_boundaries &&
                !binds_more_tightly && last_token != "new" &&
                (last_token.empty() || boolean_expression_prefixes.count(last_token) != 0);
            emit_pending(boolean_expression ? '!' : word.front());
            if (boolean_expression) output += word == "true" ? "!0" : "!1";
            else {
                output += word;
                record_token(JsTokenKind::Identifier, begin, i);
            }

            const bool was_pending_control_paren = pending_control_paren;
            pending_control_paren = control_keywords.count(word) != 0 ||
                                    (was_pending_control_paren && word == "await");
            if (word == "async") {
                pending_async_expression =
                    !(last_token.empty() || last_token == ";" || last_token == "}block" ||
                      last_token == "export" || last_token == "default");
            }
            if (word == "function") {
                pending_function_brace = true;
                pending_function_expression = last_token == "async"
                    ? pending_async_expression
                    : !(last_token.empty() || last_token == ";" || last_token == "}block" ||
                        last_token == "export" || last_token == "default");
            }
            if (word == "class") {
                pending_class_brace = true;
                pending_class_expression =
                    !(last_token.empty() || last_token == ";" || last_token == "}block" ||
                      last_token == "export" || last_token == "default");
            }
            can_start_regex = boolean_expression
                ? false
                : pending_control_paren || expression_prefix_keywords.count(word) != 0;
            last_token = boolean_expression ? "value" : word;
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t begin = i++;
            bool exponent = false;
            while (i < input.size()) {
                const unsigned char u = static_cast<unsigned char>(input[i]);
                const char q = input[i];
                if (std::isalnum(u) || q == '.' || q == '_') {
                    exponent = q == 'e' || q == 'E';
                    ++i;
                    continue;
                }
                if ((q == '+' || q == '-') && exponent) {
                    exponent = false;
                    ++i;
                    continue;
                }
                break;
            }
            const std::string raw_number = input.substr(begin, i - begin);
            const std::string number = preserve_jsx_boundaries
                ? raw_number : shorten_js_integer(raw_number);
            emit_pending(number.front());
            output += number;
            record_token(JsTokenKind::Number, begin, i);
            pending_control_paren = false;
            can_start_regex = false;
            last_token = "value";
            continue;
        }

        if (c == '`') {
            emit_pending(c);
            if (!copy_template_literal(input, i, output, error, record_token)) return false;
            pending_control_paren = false;
            can_start_regex = false;
            last_token = "value";
            continue;
        }

        if (c == '\'' || c == '"') {
            const std::size_t begin = i;
            if (!copy_quoted(i, c)) return false;
            record_token(JsTokenKind::String, begin, i);
            pending_control_paren = false;
            can_start_regex = false;
            last_token = "value";
            continue;
        }

        if (c == '(') {
            emit_pending(c);
            output.push_back(c);
            record_token(JsTokenKind::Punctuator, i, i + 1);
            control_parens.push_back(pending_control_paren);
            pending_control_paren = false;
            can_start_regex = true;
            last_token = "(";
            ++i;
            continue;
        }

        if (c == ')') {
            emit_pending(c);
            output.push_back(c);
            record_token(JsTokenKind::Punctuator, i, i + 1);
            const bool was_control = !control_parens.empty() && control_parens.back();
            if (!control_parens.empty()) control_parens.pop_back();
            pending_control_paren = false;
            // A statement can begin with a regex after if/while/for/etc.; a
            // normal function-call parenthesis instead ends an expression.
            can_start_regex = was_control;
            last_token = was_control ? ")control" : ")";
            ++i;
            continue;
        }

        // Comments are recognized before regex because // and /* cannot begin
        // a JavaScript regex literal.
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n' && input[i] != '\r') ++i;
            pending_newline = true;
            continue;
        }

        if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            const bool preserve = i + 2 < input.size() && input[i + 2] == '!';
            const auto close = input.find("*/", i + 2);
            if (close == std::string::npos) {
                error = "unterminated JavaScript block comment";
                output.clear();
                return false;
            }
            const bool had_newline = js_line_terminator_in(input, i + 2, close);
            if (preserve) {
                emit_pending('/');
                output.append(input, i, close + 2 - i);
            } else if (had_newline) {
                pending_newline = true;
            } else {
                pending_space = true;
            }
            i = close + 2;
            continue;
        }

        if (c == '/' && can_start_regex && i + 1 < input.size() &&
            (output.empty() || output.back() != '<')) {
            const std::size_t begin = i;
            emit_pending(c);
            output.push_back(input[i++]);
            bool escaped = false;
            bool in_class = false;
            bool closed = false;
            while (i < input.size()) {
                const char q = input[i++];
                output.push_back(q);
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (q == '\\') {
                    escaped = true;
                    continue;
                }
                if (q == '[') in_class = true;
                else if (q == ']') in_class = false;
                else if (q == '/' && !in_class) {
                    closed = true;
                    break;
                } else if (q == '\n' || q == '\r') {
                    break;
                }
            }
            if (!closed) {
                error = "unterminated JavaScript regular expression";
                output.clear();
                return false;
            }
            while (i < input.size() &&
                   std::isalpha(static_cast<unsigned char>(input[i])))
                output.push_back(input[i++]);
            record_token(JsTokenKind::Regex, begin, i);
            pending_control_paren = false;
            can_start_regex = false;
            last_token = "value";
            pending_space = true;
            regex_boundary = true;
            continue;
        }

        // Track whether braces belong to executable blocks or object
        // expressions. Slash after a block may begin a regex statement; slash
        // after an object literal is division.
        if (c == '{') {
            emit_pending(c);
            bool is_block = false;
            if (pending_class_brace || pending_function_brace ||
                last_token.empty() || last_token == ";" || last_token == "}block" ||
                last_token == ")" || last_token == ")control" ||
                last_token == "else" || last_token == "do" ||
                last_token == "try" || last_token == "catch" || last_token == "finally" ||
                last_token == "class") {
                is_block = true;
            } else if (last_token == ":" && (block_braces.empty() || block_braces.back())) {
                // A labelled statement (`label: { ... }`) closes like a block,
                // unlike an object property or ternary whose value is `{...}`.
                // Confirm that the current statement fragment before ':' is
                // only an identifier; this keeps `cond ? x : {}` expression-like.
                std::size_t colon = output.empty() ? 0 : output.size() - 1;
                std::size_t end = colon;
                while (end > 0 && ws(output[end - 1])) --end;
                std::size_t start = end;
                while (start > 0) {
                    const unsigned char ch = static_cast<unsigned char>(output[start - 1]);
                    if (!(std::isalnum(ch) || output[start - 1] == '_' || output[start - 1] == '$' || ch >= 0x80)) break;
                    --start;
                }
                bool identifier = end > start;
                // Labels may directly follow a control header: `if(x) label:{}`.
                // Object/ternary colons do not have a bare identifier in this
                // statement position.
                std::size_t before=start;
                while(before>0 && ws(output[before-1])) --before;
                const bool statement_position =
                    before==0 || output[before-1]==';' || output[before-1]=='{' ||
                    output[before-1]=='}' || output[before-1]==')';
                if (identifier && statement_position) is_block = true;
            }
            // For class expressions the body is syntactically a block, but
            // the closing brace yields an expression value, so slash afterward
            // must be interpreted like division rather than a regex statement.
            const bool expression_body =
                (pending_class_brace && pending_class_expression) ||
                (pending_function_brace && pending_function_expression);
            block_braces.push_back(expression_body ? false : is_block);
            output.push_back(c);
            record_token(JsTokenKind::Punctuator, i, i + 1);
            pending_class_brace = false;
            pending_class_expression = false;
            pending_function_brace = false;
            pending_function_expression = false;
            pending_async_expression = false;
            pending_control_paren = false;
            can_start_regex = true;
            last_token = "{";
            ++i;
            continue;
        }

        if (c == '}') {
            emit_pending(c);
            // A statement terminator is implied before a closing brace. Keep
            // semicolons which form an empty control/labelled statement; all
            // other immediately preceding semicolons are redundant.
            if (!preserve_jsx_boundaries && !output.empty() && output.back() == ';' &&
                before_semicolon_token != ")control" &&
                before_semicolon_token != ":" &&
                before_semicolon_token != "{" &&
                before_semicolon_token != ";" &&
                before_semicolon_token != "else") {
                output.pop_back();
            }
            output.push_back(c);
            record_token(JsTokenKind::Punctuator, i, i + 1);
            const bool was_block = block_braces.empty() ? true : block_braces.back();
            if (!block_braces.empty()) block_braces.pop_back();
            pending_control_paren = false;
            can_start_regex = was_block;
            last_token = was_block ? "}block" : "}object";
            ++i;
            continue;
        }

        emit_pending(c);
        output.push_back(c);
        record_token(JsTokenKind::Punctuator, i, i + 1);
        pending_control_paren = false;

        if (c == ';') before_semicolon_token = last_token;

        if (static_cast<unsigned char>(c) >= 0x80 || word_char(c) ||
            c == ']' || c == '.' || c == '\'' || c == '"' || c == '`') {
            can_start_regex = false;
        } else if (c == ';' || c == ',' || c == ':' || c == '[' ||
                   c == '=' || c == '!' || c == '?' || c == '&' ||
                   c == '|' || c == '+' || c == '-' || c == '*' || c == '%' ||
                   c == '<' || c == '>' || c == '/') {
            // A bare slash arriving here is division; the token following a
            // division operator begins an expression and may itself be regex.
            can_start_regex = true;
        }
        last_token.assign(1, c);
        ++i;
    }

    while (!output.empty() && ws(output.back())) output.pop_back();
    if (!preserve_jsx_boundaries && !output.empty() && output.back() == ';' &&
        before_semicolon_token != ")control" &&
        before_semicolon_token != ":" &&
        before_semicolon_token != "{" &&
        before_semicolon_token != ";" &&
        before_semicolon_token != "else") {
        output.pop_back();
    }
    error.clear();
    return true;
}

bool javascript(const std::string& input, std::string& output, std::string& error) {
    return minify_javascript(input, output, error, false);
}

bool javascript(const std::string& input, std::string& output, std::string& error,
                const Options& options) {
    return minify_javascript(input, output, error, false,
                             options.optimization != OptimizationLevel::Conservative);
}

static bool minify_xml_like(const std::string& input, std::string& output,
                            std::string& error, bool svg_mode) {
    output.clear();
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        if (input.compare(i, 4, "<!--") == 0) {
            const auto end = input.find("-->", i + 4);
            if (end == std::string::npos) {
                error = "unterminated XML comment";
                output.clear();
                return false;
            }
            i = end + 3;
            continue;
        }
        if (input.compare(i, 9, "<![CDATA[") == 0) {
            const auto end = input.find("]]>", i + 9);
            if (end == std::string::npos) {
                error = "unterminated CDATA section";
                output.clear();
                return false;
            }
            output.append(input, i, end + 3 - i);
            i = end + 3;
            continue;
        }
        if (input.compare(i, 2, "<?") == 0) {
            const auto end = input.find("?>", i + 2);
            if (end == std::string::npos) {
                error = "unterminated XML processing instruction";
                output.clear();
                return false;
            }
            // Processing-instruction data is application-defined; whitespace
            // inside it may be significant, so preserve the complete PI.
            output.append(input, i, end + 2 - i);
            i = end + 2;
            continue;
        }
        if (input[i] == '<') {
            std::size_t j = i + 1;
            bool quoted = false;
            char quote = 0;
            for (; j < input.size(); ++j) {
                char c = input[j];
                if (quoted) {
                    if (c == quote) quoted = false;
                } else if (c == '\'' || c == '"') {
                    quoted = true; quote = c;
                } else if (c == '>') { ++j; break; }
            }
            if (quoted || j > input.size() || j == 0 || input[j - 1] != '>') {
                error = "unterminated XML tag";
                output.clear();
                return false;
            }

            // Collapse insignificant whitespace inside markup, preserving quotes.
            bool ws_pending = false, in_quote = false;
            char q = 0;
            for (std::size_t k = i; k < j; ++k) {
                char c = input[k];
                if (in_quote) {
                    output.push_back(c);
                    if (c == q) in_quote = false;
                } else if (c == '\'' || c == '"') {
                    if (ws_pending && !output.empty() && output.back() != '<' && output.back() != ' ')
                        output.push_back(' ');
                    ws_pending = false; in_quote = true; q = c; output.push_back(c);
                } else if (ws(c)) {
                    ws_pending = true;
                } else {
                    const bool after_tag_prefix = !output.empty() &&
                        (output.back() == '<' ||
                         (output.back() == '/' && output.size() >= 2 && output[output.size() - 2] == '<'));
                    if (ws_pending && !output.empty() &&
                        (after_tag_prefix || (output.back() != '/' && c != '>' && c != '/')))
                        output.push_back(' ');
                    ws_pending = false;
                    output.push_back(c);
                }
            }
            i = j;
            continue;
        }

        // XML/SVG whitespace is text content unless a schema/consumer says
        // otherwise. Preserve it verbatim; even whitespace between tags may be
        // visible in mixed content or SVG <text>/<tspan>.
        if (ws(input[i])) {
            std::size_t j = i;
            while (j < input.size() && ws(input[j])) ++j;
            output.append(input, i, j - i);
            i = j;
            continue;
        }
        output.push_back(input[i++]);
    }

    (void)svg_mode;
    error.clear();
    return true;
}

bool xml(const std::string& input, std::string& output, std::string& error) {
    return minify_xml_like(input, output, error, false);
}

bool svg(const std::string& input, std::string& output, std::string& error) {
    return minify_xml_like(input, output, error, true);
}

static bool looks_like_jsx_start(const std::string& input, std::size_t i) {
    if (i + 1 >= input.size() || input[i] != '<') return false;
    const unsigned char n = static_cast<unsigned char>(input[i + 1]);
    return std::isalpha(n) || input[i + 1] == '>' || input[i + 1] == '/';
}

static bool looks_like_jsx_root_start(const std::string& input, std::size_t i) {
    if (!looks_like_jsx_start(input, i)) return false;
    // A closing tag can only belong to a root already being copied. Treating
    // it as a fresh root makes ordinary standalone JSX expressions appear
    // unterminated when their opening tag followed an ASI boundary.
    if (input[i + 1] == '/') return false;
    if (input[i + 1] == '>') return true;

    std::size_t p = i;
    bool crossed_line = false;
    while (p > 0 && ws(input[p - 1])) {
        crossed_line = crossed_line || input[p - 1] == '\n' ||
                       input[p - 1] == '\r';
        --p;
    }
    if (p == 0) return true;
    if (crossed_line && (input[p - 1] == '}' || input[p - 1] == ')' ||
                         input[p - 1] == '\'' || input[p - 1] == '"')) return true;
    if (crossed_line) {
        const auto line_start_pos = input.rfind('\n', p - 1);
        std::size_t line_start =
            line_start_pos == std::string::npos ? 0 : line_start_pos + 1;
        while (line_start < p && ws(input[line_start])) ++line_start;
        if (line_start + 1 < p && input[line_start] == '/' &&
            input[line_start + 1] == '/') return true;
    }

    const char prev = input[p - 1];
    if (prev == '=' || prev == '(' || prev == '[' || prev == '{' || prev == ',' ||
        prev == ':' || prev == ';' || prev == '?' || prev == '!' || prev == '&' ||
        prev == '|' || prev == '+' || prev == '-' || prev == '*' || prev == '%' ||
        prev == '~' || prev == '^' || prev == '>') return true;

    // JSX can directly follow expression-introducing keywords. Ordinary compact
    // comparisons such as a<b and generic-looking calls such as foo<Bar>(x)
    // must remain JavaScript instead of being guessed as markup.
    std::size_t end = p;
    while (p > 0) {
        const unsigned char c = static_cast<unsigned char>(input[p - 1]);
        if (!(std::isalnum(c) || input[p - 1] == '_' || input[p - 1] == '$')) break;
        --p;
    }
    const std::string word = input.substr(p, end - p);
    return word == "return" || word == "yield" || word == "await" ||
           word == "default" ||
           word == "case" || word == "throw";
}

static bool find_jsx_expression_end(const std::string& input, std::size_t start,
                                    std::size_t limit, std::size_t& end,
                                    std::string& error);
static bool looks_like_tsx_generic_arrow(const std::string& input,
                                         std::size_t start,
                                         std::size_t limit) {
    if (start >= limit || input[start] != '<') return false;
    std::size_t i = start + 1;
    std::size_t angle = 1;
    bool trailing_comma = false, saw_extends = false;
    bool quoted = false, escaped = false;
    char quote = 0;
    for (; i < limit; ++i) {
        const char c = input[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quoted = false;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { quoted = true; quote = c; continue; }
        if (c == '<') { ++angle; continue; }
        if (c == '>') {
            if (i > start && input[i-1] == '=') continue;
            if (--angle == 0) break;
            continue;
        }
        if (angle == 1 && c == ',') trailing_comma = true;
    }
    if (i >= limit || angle != 0) return false;

    const std::string head = input.substr(start + 1, i - start - 1);
    if (head.find("extends") != std::string::npos) saw_extends = true;
    if (!trailing_comma && !saw_extends) return false; // `<T>` is JSX-ambiguous in TSX.

    std::size_t p = i + 1;
    while (p < limit && ws(input[p])) ++p;
    if (p >= limit || input[p] != '(') return false;
    int paren = 0;
    bool q = false, esc = false; char qc = 0;
    for (; p < limit; ++p) {
        const char c = input[p];
        if (q) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == qc) q = false;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { q = true; qc = c; continue; }
        if (c == '(') ++paren;
        else if (c == ')' && --paren == 0) { ++p; break; }
    }
    if (paren != 0) return false;
    while (p < limit && ws(input[p])) ++p;
    // Optional return type before the arrow.
    if (p < limit && input[p] == ':') {
        ++p; int a=0,b=0,c=0;
        for (; p + 1 < limit; ++p) {
            const char x=input[p];
            if(x=='<')++a; else if(x=='>'&&a)--a;
            else if(x=='[')++b; else if(x==']'&&b)--b;
            else if(x=='{')++c; else if(x=='}'&&c)--c;
            if(!a&&!b&&!c&&input[p]=='='&&input[p+1]=='>') break;
        }
    }
    while (p < limit && ws(input[p])) ++p;
    return p + 1 < limit && input[p] == '=' && input[p+1] == '>';
}


static bool find_nested_jsx_end(const std::string& input, std::size_t start,
                                std::size_t limit, std::size_t& end,
                                std::string& error) {
    std::size_t p = start;
    std::size_t depth = 0;
    bool started = false;
    while (p < limit) {
        if (input[p] == '<' && looks_like_jsx_start(input, p)) {
            const bool closing = p + 1 < limit && input[p + 1] == '/';
            std::size_t j = p + 1;
            bool quoted = false, escaped = false;
            char quote = 0;
            std::size_t tag_angles = 0;
            while (j < limit) {
                const char c = input[j];
                if (quoted) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == quote) quoted = false;
                    ++j; continue;
                }
                if (c == '\'' || c == '"') { quoted = true; quote = c; ++j; continue; }
                if (c == '/' && j + 1 < limit && input[j + 1] == '/') {
                    j += 2;
                    while (j < limit && input[j] != '\n' && input[j] != '\r') ++j;
                    continue;
                }
                if (c == '/' && j + 1 < limit && input[j + 1] == '*') {
                    const auto close = input.find("*/", j + 2);
                    if (close == std::string::npos || close >= limit) {
                        error = "unterminated comment in nested JSX tag";
                        return false;
                    }
                    j = close + 2;
                    continue;
                }
                if (c == '{') {
                    std::size_t q = 0;
                    if (!find_jsx_expression_end(input, j + 1, limit, q, error)) return false;
                    j = q; continue;
                }
                if (c == '<') { ++tag_angles; ++j; continue; }
                if (c == '>') {
                    // `=>` may occur inside a generic JSX type argument, e.g.
                    // `<Comp<(x:number)=>string> ... />`; the arrow's `>` is
                    // not a generic closer.
                    if (tag_angles && j > p && input[j-1] == '=') { ++j; continue; }
                    if (tag_angles) { --tag_angles; ++j; continue; }
                    ++j; break;
                }
                ++j;
            }
            if (quoted || j > limit || j == 0 || input[j - 1] != '>') { error = "unterminated nested JSX tag"; return false; }
            std::size_t k = j >= 2 ? j - 2 : 0;
            while (k > p && ws(input[k])) --k;
            const bool self_closing = input[k] == '/';
            if (!started) { started = true; depth = self_closing ? 0 : 1; }
            else if (closing) { if (depth) --depth; }
            else if (!self_closing) ++depth;
            p = j;
            if (started && depth == 0) { end = p; return true; }
            continue;
        }
        if (input[p] == '{') {
            std::size_t q = 0;
            if (!find_jsx_expression_end(input, p + 1, limit, q, error)) return false;
            p = q; continue;
        }
        ++p;
    }
    error = "unterminated nested JSX element";
    return false;
}

static bool find_jsx_expression_end(const std::string& input, std::size_t start,
                                    std::size_t limit, std::size_t& end,
                                    std::string& error) {
    std::size_t braces = 1;
    bool can_start_regex = true;
    for (std::size_t i = start; i < limit;) {
        const char c = input[i];

        // Track JavaScript keywords that put the scanner back into an
        // expression-prefix position. This matters for nested JSX after
        // `return`, `yield`, `await`, etc. inside a JSX expression block.
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            const std::size_t begin = i++;
            while (i < limit) {
                const unsigned char u = static_cast<unsigned char>(input[i]);
                if (!std::isalnum(u) && input[i] != '_' && input[i] != '$') break;
                ++i;
            }
            const std::string word = input.substr(begin, i - begin);
            static const std::unordered_set<std::string> prefix_words = {
                "return","throw","case","delete","void","typeof","new",
                "in","instanceof","yield","await","else","do"
            };
            can_start_regex = prefix_words.count(word) != 0;
            continue;
        }

        if (c == '`') {
            // Consume the template literal as a unit so nested ${...}
            // expressions and regular-expression literals inside them (which
            // may contain backticks, braces or `//`-lookalikes) cannot corrupt
            // the enclosing brace scan.
            std::string sink;
            if (!copy_template_literal(input, i, sink, error)) return false;
            can_start_regex = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            const char quote = c;
            ++i;
            bool escaped = false;
            bool closed = false;
            while (i < limit) {
                const char q = input[i++];
                if (escaped) escaped = false;
                else if (q == '\\') escaped = true;
                else if (q == quote) { closed = true; break; }
            }
            if (!closed) { error = "unterminated string in JSX expression"; return false; }
            can_start_regex = false;
            continue;
        }

        if (c == '<' && can_start_regex && looks_like_jsx_start(input, i)) {
            if (looks_like_tsx_generic_arrow(input, i, limit)) {
                // TypeScript generic arrow syntax is JavaScript-expression
                // context for Minify++; do not reinterpret `<T,>` as markup.
                ++i;
                can_start_regex = true;
                continue;
            }
            std::size_t jsx_end = 0;
            if (!find_nested_jsx_end(input, i, limit, jsx_end, error)) return false;
            i = jsx_end;
            can_start_regex = false;
            continue;
        }

        if (c == '/' && i + 1 < limit && input[i + 1] == '/') {
            i += 2;
            while (i < limit && input[i] != '\n' && input[i] != '\r') ++i;
            can_start_regex = true;
            continue;
        }
        if (c == '/' && i + 1 < limit && input[i + 1] == '*') {
            const auto close = input.find("*/", i + 2);
            if (close == std::string::npos || close >= limit) {
                error = "unterminated JavaScript block comment in JSX expression";
                return false;
            }
            i = close + 2;
            continue;
        }
        if (c == '/' && can_start_regex) {
            ++i;
            bool escaped = false, in_class = false, closed = false;
            while (i < limit) {
                const char q = input[i++];
                if (escaped) { escaped = false; continue; }
                if (q == '\\') { escaped = true; continue; }
                if (q == '[') in_class = true;
                else if (q == ']') in_class = false;
                else if (q == '/' && !in_class) { closed = true; break; }
                else if (q == '\n' || q == '\r') break;
            }
            if (!closed) { error = "unterminated JavaScript regular expression in JSX expression"; return false; }
            while (i < limit && std::isalpha(static_cast<unsigned char>(input[i]))) ++i;
            can_start_regex = false;
            continue;
        }

        if (c == '{') { ++braces; can_start_regex = true; ++i; continue; }
        if (c == '}') {
            if (--braces == 0) { end = i + 1; return true; }
            can_start_regex = false;
            ++i;
            continue;
        }
        if (ws(c)) { ++i; continue; }

        if (word_char(c) || c == ')' || c == ']' || c == '.') can_start_regex = false;
        else if (c == ';' || c == ',' || c == ':' || c == '(' || c == '[' ||
                 c == '=' || c == '!' || c == '?' || c == '&' || c == '|' ||
                 c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                 c == '<' || c == '>') can_start_regex = true;
        ++i;
    }
    error = "unterminated JSX expression";
    return false;
}

bool jsx(const std::string& input, std::string& output, std::string& error) {
    output.clear();
    output.reserve(input.size());

    std::size_t i = 0;
    std::size_t js_start = 0;
    auto flush_js = [&](std::size_t end) -> bool {
        if (end <= js_start) return true;
        std::string part, e;
        if (!minify_javascript(input.substr(js_start, end - js_start), part, e, true)) {
            error = e;
            return false;
        }
        output += part;
        return true;
    };

    while (i < input.size()) {
        // JSX-looking bytes inside JavaScript comments are not markup roots.
        // Leave the comment itself for the JavaScript minifier in flush_js(),
        // but advance this outer root finder past its contents.
        if (input[i] == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n' && input[i] != '\r') ++i;
            continue;
        }
        if (input[i] == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            const auto close = input.find("*/", i + 2);
            if (close == std::string::npos) {
                error = "unterminated JavaScript block comment";
                output.clear();
                return false;
            }
            i = close + 2;
            continue;
        }

        // Skip JavaScript regex literals before looking for JSX roots. Without
        // this, `<`/`>` inside a regex character class (for example
        // `/[{}<>]/`) can be mistaken for a JSX fragment.
        if (input[i] == '/' && i + 1 < input.size() &&
            input[i + 1] != '/' && input[i + 1] != '*') {
            std::size_t p = i;
            while (p > js_start && ws(input[p - 1])) --p;
            bool regex_here = p == js_start;
            if (!regex_here && p > js_start) {
                const char prev = input[p - 1];
                regex_here = prev == '=' || prev == '(' || prev == '[' ||
                             prev == '{' || prev == ',' || prev == ':' ||
                             prev == ';' || prev == '?' || prev == '!' ||
                             prev == '&' || prev == '|' || prev == '+' ||
                             prev == '-' || prev == '*' || prev == '%' ||
                             prev == '~' || prev == '^' || prev == '>';
                if (!regex_here &&
                    (std::isalpha(static_cast<unsigned char>(prev)) ||
                     prev == '_' || prev == '$')) {
                    std::size_t end = p;
                    while (p > js_start) {
                        const unsigned char c =
                            static_cast<unsigned char>(input[p - 1]);
                        if (!(std::isalnum(c) || input[p - 1] == '_' ||
                              input[p - 1] == '$')) break;
                        --p;
                    }
                    const std::string word = input.substr(p, end - p);
                    regex_here = word == "return" || word == "throw" ||
                                 word == "case" || word == "yield" ||
                                 word == "await";
                }
            }
            if (regex_here) {
                ++i;
                bool escaped = false, in_class = false;
                while (i < input.size()) {
                    const char c = input[i++];
                    if (escaped) { escaped = false; continue; }
                    if (c == '\\') { escaped = true; continue; }
                    if (c == '[') in_class = true;
                    else if (c == ']') in_class = false;
                    else if (c == '/' && !in_class) break;
                    else if (c == '\n' || c == '\r') break;
                }
                while (i < input.size() &&
                       std::isalpha(static_cast<unsigned char>(input[i]))) ++i;
                continue;
            }
        }

        // Respect quoted JS before deciding that '<' begins JSX.
        if (input[i] == '\'' || input[i] == '"' || input[i] == '`') {
            char q = input[i++];
            bool esc = false;
            while (i < input.size()) {
                char c = input[i++];
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == q) break;
            }
            continue;
        }
        if (input[i] == '<' && looks_like_tsx_generic_arrow(input, i, input.size())) {
            // Recursive JSX minification also sees the JavaScript/TSX expression
            // itself. Do not mistake a valid `<T,>(...) =>` generic arrow for
            // the root of a JSX region.
            ++i;
            continue;
        }
        if (!looks_like_jsx_root_start(input, i)) { ++i; continue; }

        bool root_follows_line = false;
        for (std::size_t k = i; k > js_start && ws(input[k - 1]); --k)
            root_follows_line = root_follows_line ||
                                input[k - 1] == '\n' || input[k - 1] == '\r';
        if (!flush_js(i)) return false;
        if (root_follows_line) output.push_back('\n');

        // Copy one JSX region conservatively. Markup/text are preserved except
        // formatting whitespace inside tags; {...} expressions are recursively
        // minified with the JS scanner.
        std::size_t depth = 0;
        std::size_t p = i;
        bool started = false;
        while (p < input.size()) {
            if (input[p] == '<' && looks_like_jsx_start(input, p)) {
                bool closing = p + 1 < input.size() && input[p + 1] == '/';
                bool fragment_close = p + 2 < input.size() && input[p + 1] == '/' && input[p + 2] == '>';
                std::size_t j = p + 1;
                bool quoted = false, escaped = false;
                char q = 0;
                std::size_t attribute_braces = 0;
                std::size_t tag_angles = 0;
                for (; j < input.size(); ++j) {
                    const char c = input[j];
                    if (quoted) {
                        if (escaped) escaped = false;
                        else if (c == '\\') escaped = true;
                        else if (c == q) quoted = false;
                        continue;
                    }
                    if (c == '\'' || c == '"' || (attribute_braces && c == '`')) {
                        quoted = true; q = c; continue;
                    }
                    if (c == '/' && j + 1 < input.size() && input[j + 1] == '/') {
                        ++j;
                        while (j + 1 < input.size() &&
                               input[j + 1] != '\n' && input[j + 1] != '\r') ++j;
                        continue;
                    }
                    if (c == '/' && j + 1 < input.size() && input[j + 1] == '*') {
                        const auto close = input.find("*/", j + 2);
                        if (close == std::string::npos) {
                            error = "unterminated comment in JSX tag";
                            output.clear();
                            return false;
                        }
                        j = close + 1;
                        continue;
                    }
                    if (c == '{') { ++attribute_braces; continue; }
                    if (c == '}' && attribute_braces) { --attribute_braces; continue; }
                    if (attribute_braces == 0 && c == '<') { ++tag_angles; continue; }
                    if (c == '>' && attribute_braces == 0) {
                        if (tag_angles && j > p && input[j-1] == '=') continue;
                        if (tag_angles) { --tag_angles; continue; }
                        ++j; break;
                    }
                }
                if (quoted || j > input.size() || input[j - 1] != '>') {
                    error = "unterminated JSX tag"; output.clear(); return false;
                }
                const bool self_closing = j >= 2 && input[j - 2] == '/';
                // Preserve JSX tag spelling, but minify JavaScript expressions
                // inside attribute braces.
                {
                    bool attr_quote = false;
                    bool attr_escaped = false;
                    char attr_q = 0;
                    for (std::size_t k = p; k < j;) {
                        const char tc = input[k];
                        if (attr_quote) {
                            output.push_back(tc);
                            if (attr_escaped) attr_escaped = false;
                            else if (tc == '\\') attr_escaped = true;
                            else if (tc == attr_q) attr_quote = false;
                            ++k;
                            continue;
                        }
                        if (tc == '\'' || tc == '"') {
                            attr_quote = true; attr_q = tc; output.push_back(tc); ++k; continue;
                        }
                        if (tc == '{') {
                            std::size_t qpos = 0;
                            std::string scan_error;
                            if (!find_jsx_expression_end(input, k + 1, j, qpos, scan_error)) {
                                error = scan_error; output.clear(); return false;
                            }
                            std::string expr, e;
                            if (!jsx(input.substr(k + 1, qpos - k - 2), expr, e)) {
                                error = e; output.clear(); return false;
                            }
                            output.push_back('{'); output += expr; output.push_back('}');
                            k = qpos;
                            continue;
                        }
                        output.push_back(tc);
                        ++k;
                    }
                }
                if (!started) {
                    started = true;
                    depth = self_closing ? 0 : 1;
                } else if (closing || fragment_close) {
                    if (depth) --depth;
                } else if (!self_closing) {
                    ++depth;
                }
                p = j;
                if (started && depth == 0) break;
                continue;
            }
            if (input[p] == '{') {
                std::size_t j = 0;
                std::string scan_error;
                if (!find_jsx_expression_end(input, p + 1, input.size(), j, scan_error)) {
                    error = scan_error; output.clear(); return false;
                }
                std::string expr, e;
                if (!jsx(input.substr(p + 1, j - p - 2), expr, e)) {
                    error = e; output.clear(); return false;
                }
                output.push_back('{'); output += expr; output.push_back('}');
                p = j;
                continue;
            }
            output.push_back(input[p++]); // preserve JSX text exactly
        }
        if (!started || depth != 0) {
            error = "unterminated JSX element";
            output.clear();
            return false;
        }
        i = p;
        js_start = i;
        // The JavaScript minifier receives the suffix after this JSX region as
        // a separate fragment. Preserve a line terminator from the boundary so
        // ASI still separates a JSX expression from a following declaration,
        // export, or expression statement.
        std::size_t boundary = p;
        bool had_line_terminator = false;
        while (boundary < input.size()) {
            if (ws(input[boundary])) {
                had_line_terminator = had_line_terminator ||
                                      input[boundary] == '\n' ||
                                      input[boundary] == '\r';
                ++boundary;
                continue;
            }
            if (boundary + 1 < input.size() && input[boundary] == '/' &&
                input[boundary + 1] == '/') {
                boundary += 2;
                while (boundary < input.size() &&
                       input[boundary] != '\n' && input[boundary] != '\r') ++boundary;
                continue;
            }
            if (boundary + 1 < input.size() && input[boundary] == '/' &&
                input[boundary + 1] == '*') {
                const auto close = input.find("*/", boundary + 2);
                if (close == std::string::npos) break;
                for (std::size_t k = boundary; k < close + 2; ++k)
                    had_line_terminator = had_line_terminator ||
                                          input[k] == '\n' || input[k] == '\r';
                boundary = close + 2;
                continue;
            }
            break;
        }
        if (had_line_terminator) output.push_back('\n');
    }

    if (!flush_js(input.size())) return false;
    error.clear();
    return true;
}

bool jsx(const std::string& input, std::string& output, std::string& error,
         const Options& options) {
    (void)options;
    return jsx(input, output, error);
}

bool format_for_extension(const std::string& extension, Format& format) {
    std::string ext = lower(extension);
    if (!ext.empty() && ext.front() != '.') ext.insert(ext.begin(), '.');
    if (ext == ".html" || ext == ".htm") { format = Format::Html; return true; }
    if (ext == ".css") { format = Format::Css; return true; }
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") { format = Format::JavaScript; return true; }
    if (ext == ".jsx") { format = Format::Jsx; return true; }
    if (ext == ".json") { format = Format::Json; return true; }
    if (ext == ".xml") { format = Format::Xml; return true; }
    if (ext == ".svg") { format = Format::Svg; return true; }
    return false;
}

bool run(Format format, const std::string& input, std::string& output, std::string& error) {
    return run(format, input, output, error, Options{});
}

bool run(Format format, const std::string& input, std::string& output,
         std::string& error, const Options& options) {
    switch (format) {
        case Format::Html: return html(input, output, error);
        case Format::Css: return css(input, output, error);
        case Format::JavaScript: return javascript(input, output, error, options);
        case Format::Jsx: return jsx(input, output, error, options);
        case Format::Json: return json(input, output, error);
        case Format::Xml: return xml(input, output, error);
        case Format::Svg: return svg(input, output, error);
    }
    error = "unknown minification format";
    output.clear();
    return false;
}

} // namespace minify
