#ifndef MINIFYPP_MINIFY_H
#define MINIFYPP_MINIFY_H

#include <string>

namespace minify {

inline constexpr int format_version = 1;

enum class Format { Html, Css, JavaScript, Jsx, Json, Xml, Svg };
enum class OptimizationLevel { Conservative, Structured, Aggressive };

struct Options {
    OptimizationLevel optimization = OptimizationLevel::Conservative;
    bool structured_jsx_expressions = false;
};

bool html(const std::string& input, std::string& output, std::string& error);
bool css(const std::string& input, std::string& output, std::string& error);
bool javascript(const std::string& input, std::string& output, std::string& error);
bool javascript(const std::string& input, std::string& output, std::string& error,
                const Options& options);
bool jsx(const std::string& input, std::string& output, std::string& error);
bool jsx(const std::string& input, std::string& output, std::string& error,
         const Options& options);
bool json(const std::string& input, std::string& output, std::string& error);
bool xml(const std::string& input, std::string& output, std::string& error);
bool svg(const std::string& input, std::string& output, std::string& error);

bool format_for_extension(const std::string& extension, Format& format);
bool run(Format format, const std::string& input, std::string& output, std::string& error);
bool run(Format format, const std::string& input, std::string& output,
         std::string& error, const Options& options);

} // namespace minify

#endif
