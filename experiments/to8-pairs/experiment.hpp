#pragma once
// Experimental coupled palette fitting. No production encoder changes.
#include "api.hpp"
#include "dither_tuning.hpp"
#include "palette.hpp"
#include "png_io.hpp"
#include "scale.hpp"
#include "ssimulacra2.hpp"
#include "thomson.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

namespace to8_pair_experiment {
using namespace png2amiga;
struct Frame {
    std::vector<Color3f> palette;
    std::vector<std::uint8_t> indices;
    Image rendered;
    std::vector<std::uint8_t> raw;
};
struct Proposal {
    double predicted{};
    std::vector<Color3f> palette;
    std::size_t a{}, b{};
};
inline Color3f space(Color3f c, bool lab) {
    if (!lab) return c;
    auto l = color_space::linear_to_oklab(c);
    return {l.L, l.a, l.b};
}
inline Color3f unspace(Color3f c, bool lab) {
    return lab ? color_space::oklab_to_linear({c.r, c.g, c.b}) : c;
}
inline double dot(Color3f a, Color3f b) {
    return static_cast<double>(a.r)*static_cast<double>(b.r) + static_cast<double>(a.g)*static_cast<double>(b.g) + static_cast<double>(a.b)*static_cast<double>(b.b);
}
inline thomson::PaletteEntry to8_entry(Color3f c) {
    auto channel = [](float v) {
        return palette::thomson_channel_index(static_cast<int>(std::lround(
            std::clamp(color_space::linear_to_srgb(v), 0.0f, 1.0f)*255.0f)));
    };
    return {channel(c.r), channel(c.g), channel(c.b)};
}
inline Color3f snap(Color3f c, bool to8) {
    if (!to8) return palette::quantize_to_ocs(c);
    auto e = to8_entry(c);
    return color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(e.r,e.g,e.b));
}
inline std::vector<int> key(std::span<const Color3f> p, bool to8) {
    std::vector<int> k;
    for (auto c : p) {
        if (to8) { auto e=to8_entry(c); k.push_back((e.r<<8)|(e.g<<4)|e.b); }
        else k.push_back(palette::linear_to_ocs(c));
    }
    return k;
}
inline std::vector<Proposal> proposals(const Image& source, const Frame& f, bool to8,
                                       bool paired, bool lab) {
    const auto w=source.width(), h=source.height(), n=w*h, k=f.palette.size();
    std::vector<double> gram(k*k,0);
    std::vector<Color3f> rhs(k), colors;
    for (auto c:f.palette) colors.push_back(space(c,lab));
    // Blurred indicator fields retain how palette entries mix spatially.
    // Off-diagonal terms couple entries that occur together in the filter.
    for (std::size_t p=0;p<n;++p) {
        const int x=static_cast<int>(p%w), y=static_cast<int>(p/w);
        std::array<float,32> weights{};
        Color3f target{}, rendered{};
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) {
            int xx=x+dx, yy=y+dy;
            if (xx<0 || yy<0 || xx>=static_cast<int>(w) || yy>=static_cast<int>(h)) continue;
            auto q=static_cast<std::size_t>(yy)*w+static_cast<std::size_t>(xx);
            float a=static_cast<float>((dx==0?2:1)*(dy==0?2:1))/16.0f;
            weights[f.indices[q]]+=a;
            target+=space(source.pixels()[q],lab)*a;
            rendered+=colors[f.indices[q]]*a;
        }
        auto residual=target-rendered;
        for (std::size_t a=0;a<k;++a) if (weights[a]>0) {
            rhs[a]+=residual*weights[a];
            for (std::size_t b=0;b<k;++b) if (weights[b]>0)
                gram[a*k+b]+=static_cast<double>(weights[a])*static_cast<double>(weights[b]);
        }
    }
    std::vector<Proposal> out;
    const auto original_key=key(f.palette,to8);
    const auto unique_before=std::set<int>(original_key.begin(),original_key.end()).size();
    std::set<std::vector<int>> seen;
    for (std::size_t a=to8?0u:1u;a<k;++a) {
        for (std::size_t b=paired?a+1:a;b<(paired?k:a+1);++b) {
            double aa=gram[a*k+a], bb=gram[b*k+b], ab=paired?gram[a*k+b]:0;
            if (aa<1e-5 || bb<1e-5 || (paired && ab<0.01*std::sqrt(aa*bb))) continue;
            double ridge=0.001*(aa+bb);
            double det=(aa+ridge)*(bb+ridge)-ab*ab;
            if (det<1e-8) continue;
            auto da=paired?(rhs[a]*static_cast<float>(bb+ridge)-rhs[b]*static_cast<float>(ab))*static_cast<float>(1/det)
                          :rhs[a]*static_cast<float>(1/(aa+ridge));
            auto db=paired?(rhs[b]*static_cast<float>(aa+ridge)-rhs[a]*static_cast<float>(ab))*static_cast<float>(1/det)
                          :Color3f{};
            for (float step:{0.5f,1.0f,1.5f}) {
                auto pal=f.palette;
                pal[a]=snap(unspace(colors[a]+da*step,lab),to8);
                if (paired) pal[b]=snap(unspace(colors[b]+db*step,lab),to8);
                auto ka=key(pal,to8);
                if (ka==original_key || !seen.insert(ka).second ||
                    std::set<int>(ka.begin(),ka.end()).size()<unique_before) continue;
                if (paired && (ka[a]==original_key[a] || ka[b]==original_key[b])) continue;
                auto d1=space(pal[a],lab)-colors[a];
                auto d2=paired?space(pal[b],lab)-colors[b]:Color3f{};
                double e=aa*dot(d1,d1)+bb*dot(d2,d2)+2*ab*dot(d1,d2)-2*dot(rhs[a],d1)-2*dot(rhs[b],d2);
                if (e<0) out.push_back({e,std::move(pal),a,b});
            }
        }
    }
    std::sort(out.begin(),out.end(),[](const Proposal& a,const Proposal& b){return a.predicted<b.predicted;});
    return out;
}
inline Frame render(const Image& source, const std::vector<Color3f>& pal, bool to8,
                    const dither::Settings& dith, bool cells) {
    Frame f;
    if (to8) {
        std::vector<thomson::PaletteEntry> entries;
        for (auto c:pal) entries.push_back(to8_entry(c));
        auto r=thomson::encode(source,amiga::Mode::thomson_to8_320x16,dith,{},&entries,cells);
        if (!r) throw std::runtime_error(r.error().message);
        f.rendered=std::move(r->rendered);
        f.raw=r->page_a;
        f.raw.insert(f.raw.end(),r->page_b.begin(),r->page_b.end());
        for (auto e:r->palette) f.palette.push_back(color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(e.r,e.g,e.b)));
        f.indices.resize(source.width()*source.height());
        for (std::size_t cell=0;cell<8000;++cell) {
            auto attr=r->page_a[cell], bits=r->page_b[cell];
            auto bg=(attr&7)|(((~attr)>>4)&8), fg=((attr>>3)&7)|(((~attr)>>3)&8);
            for (std::size_t x=0;x<8;++x) f.indices[cell*8+x]=static_cast<std::uint8_t>((bits&(0x80>>x))?fg:bg);
        }
    } else {
        f.palette=pal;
        f.indices=dither::apply(source,pal,dith).indices;
        f.rendered=Image(source.width(),source.height());
        for (std::size_t p=0;p<f.indices.size();++p) f.rendered.pixels()[p]=pal[f.indices[p]];
    }
    return f;
}
inline int run(std::string mode, const std::string& input, const std::string& output, const std::string& method, bool cells) {
    const bool to8=mode=="to8";
    const int depth=mode=="lores16"?4:5;
    if (!to8 && mode!="lores16" && mode!="lores32") return 2;
    auto loaded=png_io::load(input);
    if (!loaded) throw std::runtime_error(loaded.error().message);
    auto scaled=scale::resample(*loaded,320,to8?200:256);
    if (!scaled) throw std::runtime_error(scaled.error().message);
    auto source=std::move(*scaled);
    // Same stretched, Lanczos-resampled source for every arm of the test.
    auto amode=to8?amiga::Mode::thomson_to8_320x16:amiga::Mode::lores;
    auto defaults=dither_tuning::defaults_for({amode,depth,false,false,false,amiga::Chipset::ocs,*dither::parse_method_or_null(method)});
    dither::Settings dith;
    dith.method=*dither::parse_method_or_null(method);
    dith.strength=defaults.strength;
    dith.error_clamp=defaults.error_clamp;
    api::Options opts;
    opts.mode=to8?"thomson-to8-320x16":"lores";
    opts.depth=depth;
    opts.dither=method;
    opts.dither_strength=dith.strength;
    opts.error_clamp=dith.error_clamp;
    opts.refine_iterations=8;
    opts.cell_refine=cells;
    auto baseline=api::encode_state_image(source,opts);
    if (!baseline.ok()) throw std::runtime_error(baseline.error_msg);
    auto seed=render(source,baseline.state.palette,to8,dith,cells);
    ssimulacra2::PrecomputedSource ref;
    ref.prepare(source.pixels(),source.width(),source.height());
    const float base_score=ssimulacra2::compute(ref,baseline.state.rendered.pixels());
    const float seed_score=ssimulacra2::compute(ref,seed.rendered.pixels());
    if (std::abs(base_score-seed_score)>0.001f) throw std::runtime_error("Baseline replay differs");
    std::filesystem::create_directories(output);
    auto save=[&](std::string name,const Frame& f) {
        auto p=std::filesystem::path(output)/name;
        auto r=png_io::save(p.string()+".png",f.rendered);
        if (!r) throw std::runtime_error(r.error().message);
        nlohmann::json j;
        j["palette_codes"]=key(f.palette,to8);
        j["width"]=source.width(); j["height"]=source.height();
        std::ofstream(p.string()+".json")<<j.dump(2)<<'\n';
        std::ofstream bytes(p.string()+".idx",std::ios::binary);
        bytes.write(reinterpret_cast<const char*>(f.indices.data()),static_cast<std::streamsize>(f.indices.size()));
        if (!f.raw.empty()) {
            std::ofstream raw(p.string()+".bin",std::ios::binary);
            raw.write(reinterpret_cast<const char*>(f.raw.data()),static_cast<std::streamsize>(f.raw.size()));
        }
    };
    save("before",seed);
    (void)png_io::save((std::filesystem::path(output)/"source.png").string(),source);
    nlohmann::json report={{"mode",mode},{"dither",method},{"cell_refine",cells},{"input",input},{"baseline",base_score},{"dither_strength",dith.strength}};
    for (bool paired:{false,true}) {
        auto frame=seed;
        float best=base_score;
        int evaluations=0,accepted=0;
        auto start=std::chrono::steady_clock::now();
        for (int pass=0;pass<2;++pass) {
            std::vector<Proposal> todo;
            std::set<std::vector<int>> seen;
            for (bool lab:{false,true}) {
                auto ps=proposals(source,frame,to8,paired,lab);
                int added=0;
                for (auto& p:ps) if (seen.insert(key(p.palette,to8)).second) {
                    todo.push_back(std::move(p));
                    if (++added==4) break;
                }
            }
            auto winner=frame;
            float score=best;
            for (auto& p:todo) {
                auto candidate=render(source,p.palette,to8,dith,cells);
                ++evaluations;
                float s=ssimulacra2::compute(ref,candidate.rendered.pixels());
                if (std::isfinite(s) && s>score+0.001f) {
                    winner=std::move(candidate); score=s;
                }
            }
            if (score<=best) break;
            best=score; frame=std::move(winner); ++accepted;
        }
        auto label=paired?"paired":"single";
        save(label,frame);
        report[label]={{"s2",best},{"gain",best-base_score},{"evaluations",evaluations},{"accepted_rounds",accepted},
                       {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()}};
        std::println("{} {}: {:.3f} -> {:.3f} ({:+.3f}), {} evaluations",mode,label,base_score,best,best-base_score,evaluations);
    }
    std::ofstream(std::filesystem::path(output)/"results.json")<<report.dump(2)<<'\n';
    return 0;
}
inline int stage(const std::string& input, const std::string& directory, const std::string& method) {
    auto loaded=png_io::load(input); if (!loaded) return 1;
    auto source=scale::resample(*loaded,320,200); if (!source) return 1;
    auto dir=std::filesystem::path(directory);
    dither::Settings dith;dith.method=*dither::parse_method_or_null(method);
    auto defaults=dither_tuning::defaults_for({amiga::Mode::thomson_to8_320x16,5,false,false,false,amiga::Chipset::ocs,dith.method});
    dith.strength=defaults.strength;dith.error_clamp=defaults.error_clamp;
    ssimulacra2::PrecomputedSource ref;ref.prepare(source->pixels(),320,200);
    Frame winner;float best=-1e30f;float baseline=0;
    nlohmann::json report;
    auto start=std::chrono::steady_clock::now();
    for (const char* name : {"before","single","paired"}) {
        nlohmann::json stored;std::ifstream(dir/(std::string(name)+".json"))>>stored;
        std::vector<Color3f> colors;
        for (int code : stored["palette_codes"].get<std::vector<int>>())
            colors.push_back(color_space::srgb_hex_to_linear(palette::thomson_rgb_hex(code>>8,(code>>4)&15,code&15)));
        auto frame=render(*source,colors,true,dith,true);
        float score=ssimulacra2::compute(ref,frame.rendered.pixels());
        report[name]=score;
        if (std::string_view(name)=="before") baseline=score;
        if (score>best) {best=score;winner=std::move(frame);report["winner"]=name;}
    }
    report["gain"]=best-baseline;
    report["seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    (void)png_io::save((dir/"staged.png").string(),winner.rendered);
    std::ofstream(dir/"staged.json")<<report.dump(2);
    std::ofstream(dir/"staged-palette.json")<<nlohmann::json{{"palette_codes",key(winner.palette,true)}}.dump(2);
    std::ofstream raw(dir/"staged.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(winner.raw.data()),static_cast<std::streamsize>(winner.raw.size()));
    std::println("{} {} staged: {:+.3f}",input,method,best-baseline);
    return 0;
}
} // namespace to8_pair_experiment
