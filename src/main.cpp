#include "ir.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

namespace {

void usage(std::ostream& out) {
    out << "Usage: mdrframe [--emit=ir|deps] [--p6-narrow-writes] [-o output] input.mdr\n";
}

int run(int argc, char** argv) {
    std::filesystem::path input;
    std::filesystem::path output;
    bool dependenciesOnly = false;
    mdr::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "--emit=ir") dependenciesOnly = false;
        else if (arg == "--emit=deps") dependenciesOnly = true;
        else if (arg == "--p6-narrow-writes") options.narrowWrites = true;
        else if (arg == "-o" && i + 1 < argc) output = argv[++i];
        else if (!arg.starts_with('-') && input.empty()) input = argv[i];
        else throw std::runtime_error("unexpected argument '" + std::string(arg) + "'");
    }
    if (input.empty()) {
        usage(std::cerr);
        return 2;
    }
    if (!input.has_extension() && !std::filesystem::exists(input)) input += ".mdr";
    if (!output.empty() && std::filesystem::exists(output) && std::filesystem::equivalent(input, output))
        throw std::runtime_error("output must differ from the input file");

    std::ifstream source(input, std::ios::binary);
    if (!source) throw std::runtime_error("cannot open " + input.string());
    const std::string text{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};

    mdr::ModuleIR module;
    try {
        module = mdr::lower(mdr::parse(text), options);
    } catch (const mdr::Error& error) {
        std::cerr << input.string() << ':' << error.location.line << ':' << error.location.column
                  << ": error: " << error.what() << '\n';
        return 1;
    }
    if (output.empty()) {
        mdr::print(std::cout, module, dependenciesOnly);
        if (!std::cout) throw std::runtime_error("cannot write output");
    } else {
        std::ofstream destination(output, std::ios::binary);
        if (!destination) throw std::runtime_error("cannot open " + output.string());
        mdr::print(destination, module, dependenciesOnly);
        destination.close();
        if (!destination) throw std::runtime_error("cannot write " + output.string());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "mdrframe: " << error.what() << '\n';
        return 1;
    }
}

