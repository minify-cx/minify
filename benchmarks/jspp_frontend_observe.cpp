#include <jspp/frontend.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
const char* status_name(jspp::syntax::ParseStatus status) {
    switch (status) {
    case jspp::syntax::ParseStatus::Success: return "success";
    case jspp::syntax::ParseStatus::SyntaxError: return "syntax-error";
    case jspp::syntax::ParseStatus::Unsupported: return "unsupported";
    }
    return "unknown";
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: jspp-frontend-observe FILE...\n";
        return 2;
    }
    for (int argument = 1; argument < argc; ++argument) {
        std::ifstream input(argv[argument], std::ios::binary);
        if (!input) {
            std::cerr << "cannot open " << argv[argument] << '\n';
            return 1;
        }
        std::string source{std::istreambuf_iterator<char>(input), {}};
        jspp::syntax::ParseResult result;
        const auto status = jspp::syntax::parse(std::move(source), result);
        std::cout << argv[argument] << '\t' << status_name(status) << '\t'
                  << result.tree.lexical.tokens.size() << '\t'
                  << result.tree.nodes.size() << '\n';
    }
}
