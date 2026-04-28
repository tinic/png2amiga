#include "pipeline.hpp"

#include <cctype>

namespace png2amiga::pipeline {

std::string derive_symbol_name(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos) path = path.substr(slash + 1);
    auto backslash = path.rfind('\\');
    if (backslash != std::string_view::npos) path = path.substr(backslash + 1);
    auto dot = path.rfind('.');
    if (dot != std::string_view::npos) path = path.substr(0, dot);

    std::string result;
    result.reserve(path.size());
    for (auto c : path) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) return "image";
    if (std::isdigit(static_cast<unsigned char>(result[0])))
        result.insert(result.begin(), '_');
    return result;
}

cheader::CHeaderOptions make_ch_opts(const ChOptsBase& base) {
    cheader::CHeaderOptions ch;
    ch.symbol_name = base.symbol_override.empty()
        ? derive_symbol_name(base.output_path)
        : std::string(base.symbol_override);
    ch.hires = base.hires;
    ch.interlace = base.interlace;
    ch.aga = base.aga;
    ch.fade_in = base.fade_in;
    ch.dpf = base.dpf;
    ch.interleaved = base.interleaved;
    return ch;
}

}  // namespace png2amiga::pipeline
