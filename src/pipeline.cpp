#include "pipeline.hpp"

#include "color_space.hpp"

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

amiga::Chipset resolve_chipset(std::optional<amiga::Chipset> requested,
                               amiga::Mode mode) {
    auto params = amiga::get_mode_params(mode);
    if (params.bitplane_depth > 6) return amiga::Chipset::aga;
    if (requested.has_value()) return *requested;
    return amiga::Chipset::ocs;
}

amiga::Chipset resolve_chipset(std::string_view requested, amiga::Mode mode) {
    std::optional<amiga::Chipset> req;
    if (requested == "aga") req = amiga::Chipset::aga;
    else if (requested == "ocs") req = amiga::Chipset::ocs;
    return resolve_chipset(req, mode);
}

void PipelineResult::finalize_psnr(const Image& src, float total_error) {
    quant_error = total_error;
    psnr = color_space::compute_psnr_blurred(
        src.pixels(), rendered.pixels(),
        src.width(), src.height());
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
