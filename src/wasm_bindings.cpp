#include "api.hpp"

#include <emscripten/bind.h>
#include <emscripten/threading.h>
#include <emscripten/val.h>

#include <atomic>
#include <utility>

using namespace emscripten;
using namespace png2amiga::api;

// File-scope progress slot. emscripten::val handles are thread-bound,
// so when a pthread worker calls the encoder's on_progress lambda we
// can't invoke the captured JS callback directly. Instead the worker
// async-dispatches main_thread_progress_tick() onto the WASM main
// runtime thread (which processes the dispatch queue while it's
// blocked on pthread_join), and main reads the cb out of this slot
// and calls it. WASM is single-threaded at the JS layer so one slot
// is enough — only one encode runs at a time.
namespace {
val g_progress_cb = val::null();
std::atomic<bool> g_progress_active{false};
}

extern "C" void EMSCRIPTEN_KEEPALIVE
main_thread_progress_tick(float p) {
    if (g_progress_active.load(std::memory_order_acquire) &&
        !g_progress_cb.isNull()) {
        // Stage label is dropped on the proxy path — only the float
        // is cheap to marshal across the dispatch queue. UI bars use
        // the value alone so the label loss is invisible.
        g_progress_cb(p, std::string("encoding"));
    }
}

// Convert JS options object to C++ Options
Options parse_js_options(val js_opts) {
    Options opts;
    if (js_opts.hasOwnProperty("mode"))
        opts.mode = js_opts["mode"].as<std::string>();
    if (js_opts.hasOwnProperty("chipset"))
        opts.chipset = js_opts["chipset"].as<std::string>();
    if (js_opts.hasOwnProperty("depth"))
        opts.depth = js_opts["depth"].as<int>();
    if (js_opts.hasOwnProperty("interlace"))
        opts.interlace = js_opts["interlace"].as<bool>();
    if (js_opts.hasOwnProperty("dither"))
        opts.dither = js_opts["dither"].as<std::string>();
    if (js_opts.hasOwnProperty("ditherStrength"))
        opts.dither_strength = js_opts["ditherStrength"].as<float>();
    if (js_opts.hasOwnProperty("errorClamp"))
        opts.error_clamp = js_opts["errorClamp"].as<float>();
    if (js_opts.hasOwnProperty("hamBeam"))
        opts.ham_beam = js_opts["hamBeam"].as<int>();
    if (js_opts.hasOwnProperty("hamTriple"))
        opts.ham_triple = js_opts["hamTriple"].as<int>();
    if (js_opts.hasOwnProperty("hamFast"))
        opts.ham_fast = js_opts["hamFast"].as<bool>();
    if (js_opts.hasOwnProperty("quantizer"))
        opts.quantizer = js_opts["quantizer"].as<std::string>();
    if (js_opts.hasOwnProperty("refineIterations"))
        opts.refine_iterations = js_opts["refineIterations"].as<int>();
    if (js_opts.hasOwnProperty("copper"))
        opts.copper = js_opts["copper"].as<bool>();
    if (js_opts.hasOwnProperty("gamma"))
        opts.gamma = js_opts["gamma"].as<float>();
    if (js_opts.hasOwnProperty("brightness"))
        opts.brightness = js_opts["brightness"].as<float>();
    if (js_opts.hasOwnProperty("contrast"))
        opts.contrast = js_opts["contrast"].as<float>();
    if (js_opts.hasOwnProperty("saturation"))
        opts.saturation = js_opts["saturation"].as<float>();
    if (js_opts.hasOwnProperty("hueShift"))
        opts.hue_shift = js_opts["hueShift"].as<float>();
    if (js_opts.hasOwnProperty("sharpen"))
        opts.sharpen = js_opts["sharpen"].as<float>();
    if (js_opts.hasOwnProperty("blackPoint"))
        opts.black_point = js_opts["blackPoint"].as<float>();
    if (js_opts.hasOwnProperty("whitePoint"))
        opts.white_point = js_opts["whitePoint"].as<float>();
    if (js_opts.hasOwnProperty("matchRange"))
        opts.match_range = js_opts["matchRange"].as<bool>();
    if (js_opts.hasOwnProperty("width"))
        opts.width = js_opts["width"].as<int>();
    if (js_opts.hasOwnProperty("height"))
        opts.height = js_opts["height"].as<int>();
    if (js_opts.hasOwnProperty("paletteFile"))
        opts.palette_file = js_opts["paletteFile"].as<std::string>();
    if (js_opts.hasOwnProperty("paletteData")) {
        auto arr = js_opts["paletteData"];
        auto len = arr["length"].as<std::size_t>();
        opts.palette_data.resize(len);
        val view = val(typed_memory_view(len, opts.palette_data.data()));
        view.call<void>("set", arr);
    }
    if (js_opts.hasOwnProperty("symbolName"))
        opts.symbol_name = js_opts["symbolName"].as<std::string>();
    if (js_opts.hasOwnProperty("copperChanges"))
        opts.copper_changes = js_opts["copperChanges"].as<int>();
    if (js_opts.hasOwnProperty("lockColor0"))
        opts.lock_color0 = js_opts["lockColor0"].as<bool>();
    if (js_opts.hasOwnProperty("paletteDiversity"))
        opts.palette_diversity = js_opts["paletteDiversity"].as<int>();
    if (js_opts.hasOwnProperty("cropX"))
        opts.crop_x = js_opts["cropX"].as<int>();
    if (js_opts.hasOwnProperty("cropY"))
        opts.crop_y = js_opts["cropY"].as<int>();
    if (js_opts.hasOwnProperty("cropW"))
        opts.crop_w = js_opts["cropW"].as<int>();
    if (js_opts.hasOwnProperty("cropH"))
        opts.crop_h = js_opts["cropH"].as<int>();
    if (js_opts.hasOwnProperty("cropAuto"))
        opts.crop_auto = js_opts["cropAuto"].as<bool>();
    if (js_opts.hasOwnProperty("alphaThreshold"))
        opts.alpha_threshold = js_opts["alphaThreshold"].as<float>();
    if (js_opts.hasOwnProperty("alphaDither"))
        opts.alpha_dither = js_opts["alphaDither"].as<std::string>();
    if (js_opts.hasOwnProperty("alphaDitherStrength"))
        opts.alpha_dither_strength = js_opts["alphaDitherStrength"].as<float>();
    if (js_opts.hasOwnProperty("maskInvert"))
        opts.mask_invert = js_opts["maskInvert"].as<bool>();
    if (js_opts.hasOwnProperty("nativePar"))
        opts.native_par = js_opts["nativePar"].as<bool>();
    if (js_opts.hasOwnProperty("cgaTextMetric"))
        opts.cga_text_metric = js_opts["cgaTextMetric"].as<std::string>();
    if (js_opts.hasOwnProperty("c64Palette"))
        opts.c64_palette = js_opts["c64Palette"].as<std::string>();
    if (js_opts.hasOwnProperty("c64Metric"))
        opts.c64_metric = js_opts["c64Metric"].as<std::string>();
    if (js_opts.hasOwnProperty("c64PetsciiGraphicsOnly"))
        opts.c64_petscii_graphics_only =
            js_opts["c64PetsciiGraphicsOnly"].as<bool>();
    if (js_opts.hasOwnProperty("tileBudget"))
        opts.tile_budget = static_cast<std::size_t>(
            js_opts["tileBudget"].as<int>());
    if (js_opts.hasOwnProperty("tileReserve"))
        opts.tile_reserve = static_cast<std::size_t>(
            js_opts["tileReserve"].as<int>());
    if (js_opts.hasOwnProperty("dualPlayfield"))
        opts.dual_playfield = js_opts["dualPlayfield"].as<bool>();
    if (js_opts.hasOwnProperty("scap"))
        opts.scap = js_opts["scap"].as<bool>();
    if (js_opts.hasOwnProperty("scapDebug"))
        opts.strips_debug = js_opts["scapDebug"].as<bool>();
    if (js_opts.hasOwnProperty("best"))
        opts.best = js_opts["best"].as<bool>();
    if (js_opts.hasOwnProperty("bestMetric"))
        opts.best_metric = js_opts["bestMetric"].as<std::string>();
    if (js_opts.hasOwnProperty("onProgress")) {
        // The encoder may call this from worker threads (parallel_for
        // bodies, HAM beam search, best_sweep trials). emscripten
        // ::val handles are bound to the thread that captured them, so
        // pthread-worker calls would crash on the captured cb. Main-
        // runtime-thread calls invoke the cb directly; worker calls
        // async-dispatch main_thread_progress_tick() onto the main
        // runtime thread (which processes the dispatch queue while
        // blocked on pthread_join). The cb is stashed in a file-scope
        // slot since the dispatched function takes a primitive float
        // — emscripten can't marshal a captured val across threads.
        val cb = js_opts["onProgress"];
        if (cb.typeOf().as<std::string>() == "function") {
            g_progress_cb = cb;
            g_progress_active.store(true, std::memory_order_release);
            opts.on_progress = [cb](float p, std::string_view stage) {
                if (emscripten_is_main_runtime_thread()) {
                    cb(p, std::string(stage));
                } else {
                    emscripten_async_run_in_main_runtime_thread(
                        EM_FUNC_SIG_VF,
                        reinterpret_cast<void*>(&main_thread_progress_tick),
                        p);
                }
            };
        }
    }
    return opts;
}

// Helper: copy a vector<uint8_t> into a JS Uint8Array
val make_uint8_array(const std::vector<std::uint8_t>& data) {
    val arr = val::global("Uint8Array").new_(data.size());
    // Use typed_memory_view for efficient bulk copy
    val view = val(typed_memory_view(data.size(), data.data()));
    arr.call<void>("set", view);
    return arr;
}

// JS API: convert(Uint8Array, options) -> { data: Uint8Array(PNG), width, height, error }
val js_convert(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: convertRGBA(Uint8Array, options) -> { rgba: Uint8Array, width, height, error }
val js_convert_rgba(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_rgba(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("depth", result.depth);
    obj.set("colors", result.colors);
    obj.set("copperChanges", result.copperChanges);
    obj.set("changesPerLine", result.changesPerLine);
    obj.set("maxMovesPerLine", result.maxMovesPerLine);
    obj.set("aga", result.aga);
    obj.set("totalColors", result.totalColors);
    obj.set("planeBytes", result.planeBytes);
    obj.set("copperBytes", result.copperBytes);
    obj.set("diskBytes",  result.diskBytes);
    obj.set("chipBytes",  result.chipBytes);
    obj.set("quantError", result.quantError);
    obj.set("psnr", result.psnr);
    obj.set("s2", result.s2);
    obj.set("hasTransparency", result.hasTransparency);
    obj.set("genesisUniqueTiles", result.genesisUniqueTiles);
    obj.set("genesisTotalCells",  result.genesisTotalCells);
    obj.set("tileDataBytes",      result.tileDataBytes);
    if (!result.c64CharsetData.empty()) {
        obj.set("c64CharsetData",
                make_uint8_array(result.c64CharsetData));
        obj.set("c64Mc1",     result.c64Mc1);
        obj.set("c64Mc2",     result.c64Mc2);
        obj.set("c64BgColor", result.c64BgColor);
    }
    if (!result.genesisTileBytes.empty()) {
        obj.set("genesisTileBytes",
                make_uint8_array(result.genesisTileBytes));
        obj.set("genesisTilemapBytes",
                make_uint8_array(result.genesisTilemapBytes));
        obj.set("genesisPaletteBytes",
                make_uint8_array(result.genesisPaletteBytes));
    }
    if (!result.snesTileBytes.empty()) {
        obj.set("snesTileBytes",
                make_uint8_array(result.snesTileBytes));
        obj.set("snesTilemapBytes",
                make_uint8_array(result.snesTilemapBytes));
        if (!result.snesPaletteBytes.empty())
            obj.set("snesPaletteBytes",
                    make_uint8_array(result.snesPaletteBytes));
    }
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("rgba", make_uint8_array(result.data));

    return obj;
}

// JS API: convertIFF(Uint8Array, options) -> { data: Uint8Array(IFF), width, height, error }
val js_convert_iff(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_iff(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: convertHeader(Uint8Array, options, symbolName) -> { data: Uint8Array(text), width, height, error }
val js_convert_header(val input_array, val js_opts, std::string symbol_name) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    if (!symbol_name.empty())
        opts.symbol_name = symbol_name;
    auto result = convert_cheader(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty()) {
        std::string text(result.data.begin(), result.data.end());
        obj.set("header", text);
        obj.set("data", make_uint8_array(result.data));
    }

    return obj;
}

// JS API: convertRaw(Uint8Array, options) -> { data: Uint8Array, width, height, error }
val js_convert_raw(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_raw(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: convertPRG(Uint8Array, options) -> { data: Uint8Array(.prg), width, height, error }
// c64 modes only — Koala/hires/FLI/AFLI/PETSCII displayer .prg.
val js_convert_prg(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_prg(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);
    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));
    return obj;
}

// JS API: convertKoa(Uint8Array, options) -> { data: Uint8Array(.koa), error }
val js_convert_koa(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);
    auto opts = parse_js_options(js_opts);
    auto result = convert_koa(input.data(), input.size(), opts);
    val obj = val::object();
    obj.set("error", result.error);
    if (!result.data.empty()) obj.set("data", make_uint8_array(result.data));
    return obj;
}

// JS API: convertHir(Uint8Array, options) -> { data: Uint8Array(.hir), error }
val js_convert_hir(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);
    auto opts = parse_js_options(js_opts);
    auto result = convert_hir(input.data(), input.size(), opts);
    val obj = val::object();
    obj.set("error", result.error);
    if (!result.data.empty()) obj.set("data", make_uint8_array(result.data));
    return obj;
}

// JS API: convertDegas(Uint8Array, options) -> { data: Uint8Array, width, height, error }
val js_convert_degas(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_degas(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: convertViewer(Uint8Array, options) -> { data: Uint8Array(text), header: string, width, height, error }
val js_convert_viewer(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_viewer(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("error", result.error);

    if (!result.data.empty()) {
        std::string text(result.data.begin(), result.data.end());
        obj.set("header", text);
        obj.set("data", make_uint8_array(result.data));
    }

    return obj;
}

// JS API: convertMask(Uint8Array, options) -> { data: Uint8Array(PNG), width, height, error }
val js_convert_mask(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_mask(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("hasTransparency", result.hasTransparency);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: convertMaskRaw(Uint8Array, options) -> { data: Uint8Array, width, height, error }
val js_convert_mask_raw(val input_array, val js_opts) {
    auto length = input_array["length"].as<std::size_t>();
    std::vector<std::uint8_t> input(length);
    val view = val(typed_memory_view(length, input.data()));
    view.call<void>("set", input_array);

    auto opts = parse_js_options(js_opts);
    auto result = convert_mask_raw(input.data(), input.size(), opts);

    val obj = val::object();
    obj.set("width", result.width);
    obj.set("height", result.height);
    obj.set("hasTransparency", result.hasTransparency);
    obj.set("error", result.error);

    if (!result.data.empty())
        obj.set("data", make_uint8_array(result.data));

    return obj;
}

// JS API: ditherDefaults(options) -> { strength, errorClamp }
val js_dither_defaults(val js_opts) {
    auto opts = parse_js_options(js_opts);
    auto d = dither_defaults_for(opts);
    val obj = val::object();
    obj.set("strength", d.strength);
    obj.set("errorClamp", d.error_clamp);
    return obj;
}

EMSCRIPTEN_BINDINGS(png2amiga) {
    function("convert", &js_convert);
    function("convertRGBA", &js_convert_rgba);
    function("convertIFF", &js_convert_iff);
    function("convertHeader", &js_convert_header);
    function("convertViewer", &js_convert_viewer);
    function("convertDegas", &js_convert_degas);
    function("convertRaw", &js_convert_raw);
    function("convertPRG", &js_convert_prg);
    function("convertKoa", &js_convert_koa);
    function("convertHir", &js_convert_hir);
    function("convertMask", &js_convert_mask);
    function("convertMaskRaw", &js_convert_mask_raw);
    function("ditherDefaults", &js_dither_defaults);
}
