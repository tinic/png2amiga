// CRT preview renderer for png2amiga.
//
// Simulates a Commodore 1084S RGB monitor: 14" slot-mask CRT, 0.42mm dot
// pitch, RGB analog input (NO composite NTSC artifacting — that's the
// whole point of an RGB monitor).
//
// The fragment shader is adapted from Tim Lottes' public-domain CRT
// shader (libretro/glsl-shaders/crt-lottes.glsl), with parameter values
// tuned for an 80s pro-grade Amiga monitor rather than an arcade tube:
//
//   * Slot mask phosphor pattern with diagonal stagger (pos.x += pos.y*3
//     creates the staggered RGB triads characteristic of slot masks vs.
//     pure aperture-grille verticals).
//   * Soft scanlines (hardScan = -8): real Gaussian beam profile, not the
//     binary every-other-line darkening that looks like a cheap filter.
//   * Moderate horizontal sharpness (hardPix = -3): 1084S has higher
//     bandwidth than a TV but isn't infinitely sharp.
//   * Subtle bloom on bright pixels (halation that real phosphors exhibit).
//   * No geometric warp — pixel positions are preserved so the preview
//     stays usable as a reference (curvature removed per user request).
//   * Matched 2.2 decode/encode gamma so flat colors round-trip to the
//     source values exactly; an asymmetric 2.2/2.4 pair shifts every
//     color to c^0.917 (brighter + desaturated).
//
// Single-pass fragment shader. No bloom downsample chain; the bloom is
// approximated via a wider 5-tap horizontal & 5-line vertical Gaussian
// added at low intensity to the main filtered color.
//
// Caller flow:
//   const crt = createCrtRenderer(canvas)
//   crt.render(rgbaUint8, srcW, srcH, dstW, dstH)
//   crt.dispose()  (when toggling off / unmount)

const VERT = `#version 100
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`

// Lottes mask mode 1 (slot mask, 6-px period with vertical stagger).
// hardScan = -8.0  → soft Gaussian scanlines (visible but not crunchy)
// hardPix  = -3.0  → soft horizontal — 1084S RGB bandwidth isn't infinite
// maskDark = 0.5, maskLight = 1.5  → 50%/150% modulation either side of 1.0
//                                    so total brightness is preserved
// shape    = 2.0   → Gaussian (could be 1.5 for triangular fall-off)
// bloomStrength = 0.18      → subtle halation, not glow-around-everything
const FRAG = `#version 100
precision mediump float;
varying vec2 v_uv;

uniform sampler2D u_src;
uniform vec2 u_srcSize;        // source pixel grid (e.g. 320x213)
uniform vec2 u_outSize;        // output canvas size in physical pixels
uniform float u_hardScan;
uniform float u_hardPix;
uniform float u_maskDark;
uniform float u_maskLight;
uniform float u_bloom;
uniform float u_brightness;    // post-mask brightness boost
uniform float u_maskPeriod;    // device pixels per RGB triad (≈0.42mm × DPR × 96/25.4)

// 1084S RGB profile = 0. C64 PAL composite profile = 1. Enables chroma
// low-pass (U/V horizontal blur + 1-H delay-line vertical averaging),
// chromatic aberration, and a softer mask suited to a TV's bigger phosphor
// triads — the look of a C64 plugged into a PAL TV via composite.
// Ported from png2c64's crt.js (commits e408658 / d70a705 / 56aaf89 /
// c27d846).
uniform float u_palMode;
// Interlace flicker. Field rate is hardcoded to 60 Hz (NTSC field rate
// = 30 Hz pair rate, matches typical 60 Hz monitors so each simulated
// field lands on exactly one display frame). Active = 1.0 means the
// row currently belongs to the "live" field — gain 1.0. Inactive rows
// fade to u_phosphorPersist (0.0–1.0). u_time is monotonic seconds.
uniform float u_interlaceFlicker;
uniform float u_phosphorPersist;
uniform float u_time;

// sRGB ↔ linear (gamma 2.2 approximation, ample for the simulation).
// Decode and encode exponents must match — an asymmetric pair (e.g.
// encode at 1/2.4) maps every flat color to c^(2.2/2.4), a visible
// brightness lift + desaturation relative to the source.
vec3 toLinear(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(2.2));
}
vec3 toSrgb(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
}

// Gaussian weight. scale < 0; the more negative, the tighter.
float gaus(float pos, float scale) {
  return exp2(scale * pos * pos);
}

// Fetch source texel at integer offset (off in source pixels).
// Operates on a per-source-texel grid: pos is in source-texel units,
// floor()ed so fetch always lands on the canonical center.
vec3 fetch(vec2 pos, vec2 off) {
  pos = (floor(pos * u_srcSize + off) + vec2(0.5)) / u_srcSize;
  if (any(lessThan(pos, vec2(0.0))) || any(greaterThan(pos, vec2(1.0)))) {
    return vec3(0.0);
  }
  return toLinear(texture2D(u_src, pos).rgb);
}

// Distance to nearest texel center (range [-0.5, 0.5]).
vec2 dist(vec2 pos) {
  pos = pos * u_srcSize;
  return -((pos - floor(pos)) - vec2(0.5));
}

// Horizontal 3-tap reconstruction at vertical scanline offset off.
vec3 horz3(vec2 pos, float off) {
  vec3 b = fetch(pos, vec2(-1.0, off));
  vec3 c = fetch(pos, vec2( 0.0, off));
  vec3 d = fetch(pos, vec2( 1.0, off));
  float dst = dist(pos).x;
  // Gaussian weights centered on dst with hardPix sharpness
  float wb = gaus(dst - 1.0, u_hardPix);
  float wc = gaus(dst        , u_hardPix);
  float wd = gaus(dst + 1.0, u_hardPix);
  return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

// Wider 5-tap variant for bloom contribution.
vec3 horz5(vec2 pos, float off) {
  vec3 a = fetch(pos, vec2(-2.0, off));
  vec3 b = fetch(pos, vec2(-1.0, off));
  vec3 c = fetch(pos, vec2( 0.0, off));
  vec3 d = fetch(pos, vec2( 1.0, off));
  vec3 e = fetch(pos, vec2( 2.0, off));
  float dst = dist(pos).x;
  // hardPix * 0.4 → wider Gaussian for bloom
  float bloomP = u_hardPix * 0.4;
  float wa = gaus(dst - 2.0, bloomP);
  float wb = gaus(dst - 1.0, bloomP);
  float wc = gaus(dst        , bloomP);
  float wd = gaus(dst + 1.0, bloomP);
  float we = gaus(dst + 2.0, bloomP);
  return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa + wb + wc + wd + we);
}

// Vertical scanline weight for the row at vertical offset off (in source rows).
float scan(vec2 pos, float off) {
  float dst = dist(pos).y;
  return gaus(dst + off, u_hardScan);
}

// Wider scanline weight for the bloom contribution.
float bloomScan(vec2 pos, float off) {
  float dst = dist(pos).y;
  return gaus(dst + off, u_hardScan * 0.25);
}

// Per-source-row interlace gain. The Amiga lace frame is already
// stored as both fields interleaved (even rows = field 0, odd rows =
// field 1), so we just have to read the row's parity and compare it
// to which field is currently "live." Live row → gain 1.0; off-field
// row → fades to u_phosphorPersist. The 60 Hz field rate (vs 25/30 Hz
// pair rate) is what makes the flicker visible — well below human
// flicker fusion (~50 Hz) on high-contrast horizontal edges, which is
// exactly the artifact a real 1084S exhibits in interlace mode.
float interlaceRowGain(vec2 pos, float off) {
  if (u_interlaceFlicker < 0.5) return 1.0;
  float row = floor(pos.y * u_srcSize.y + off);
  float rowParity = mod(row, 2.0);
  // 60 Hz field rate hardcoded — matches typical 60 Hz monitors so each
  // simulated field gets exactly one display frame.
  float currentField = mod(floor(u_time * 60.0), 2.0);
  return (abs(rowParity - currentField) < 0.5) ? 1.0 : u_phosphorPersist;
}

// Three-line composite: weighted sum of three vertically-adjacent
// horizontally-filtered rows, each multiplied by its scanline weight.
// Normalized by the peak-on-scanline weight sum so both progressive
// (sharp scanlines, hardScan=-8 → sum ≈ 1.008) and interlace (soft,
// hardScan=-1 → sum = 2.0) peak at the source luminance. Without this
// normalization interlace emits ~2× the light per fragment before the
// mask even applies, and the mode looks dramatically brighter than
// progressive. Modulation depth (the visible scanlines) is preserved
// because the dark-valley fragments still get their off-peak weight
// sums, which are smaller than the peak we divide by.
vec3 tri(vec2 pos) {
  vec3 a = horz3(pos, -1.0);
  vec3 b = horz3(pos,  0.0);
  vec3 c = horz3(pos,  1.0);
  float ga = interlaceRowGain(pos, -1.0);
  float gb = interlaceRowGain(pos,  0.0);
  float gc = interlaceRowGain(pos,  1.0);
  float peakSum = 2.0 * exp2(u_hardScan) + 1.0;
  return (a * ga * scan(pos, -1.0)
        + b * gb * scan(pos, 0.0)
        + c * gc * scan(pos, 1.0)) / peakSum;
}

// Bloom: 5×5-ish wider Gaussian over 5 rows, used to add halation around
// bright phosphors (the diffuse glow real CRTs exhibit). Same peak-sum
// normalization as tri() — bloomScan is hardScan*0.25 so its weight
// sum varies even more across modes (1.51 progressive vs 3.68 inter-
// lace at scale -8 vs -1), which would compound the brightness skew.
vec3 bloom(vec2 pos) {
  vec3 a = horz5(pos, -2.0);
  vec3 b = horz5(pos, -1.0);
  vec3 c = horz5(pos,  0.0);
  vec3 d = horz5(pos,  1.0);
  vec3 e = horz5(pos,  2.0);
  float ga = interlaceRowGain(pos, -2.0);
  float gb = interlaceRowGain(pos, -1.0);
  float gc = interlaceRowGain(pos,  0.0);
  float gd = interlaceRowGain(pos,  1.0);
  float ge = interlaceRowGain(pos,  2.0);
  float bs = u_hardScan * 0.25;
  // exp2(s*4) + exp2(s*1) + 1 + exp2(s*1) + exp2(s*4)
  float peakSum = 2.0 * exp2(bs * 4.0) + 2.0 * exp2(bs) + 1.0;
  return (a * ga * bloomScan(pos, -2.0)
        + b * gb * bloomScan(pos, -1.0)
        + c * gc * bloomScan(pos,  0.0)
        + d * gd * bloomScan(pos,  1.0)
        + e * ge * bloomScan(pos,  2.0)) / peakSum;
}

// PAL chroma low-pass + 1-H delay-line averaging. Samples a 5-tap
// horizontal × 2-row vertical neighborhood, converts each to YUV, and
// returns the *averaged U/V* with Y from the center tap. Used to
// replace the C64 frame's chroma while keeping luma sharp — same model
// as png2c64's CPU pass, in 1 fragment.
vec3 palChroma(vec2 pos) {
  vec2 ts = 1.0 / u_srcSize;
  // 5-tap horizontal weights, 2-row vertical (current + next).
  float wH[5];
  wH[0] = 0.10; wH[1] = 0.20; wH[2] = 0.40; wH[3] = 0.20; wH[4] = 0.10;
  vec3 yuvCenter = vec3(0.0);
  float uSum = 0.0, vSum = 0.0;
  for (int dx = -2; dx <= 2; ++dx) {
    for (int dy = 0; dy <= 1; ++dy) {
      vec2 p = pos + vec2(float(dx), float(dy)) * ts;
      vec3 rgb = toLinear(texture2D(u_src, clamp(p, vec2(0.0), vec2(1.0))).rgb);
      float Y =  0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
      float U = -0.147 * rgb.r - 0.289 * rgb.g + 0.436 * rgb.b;
      float V =  0.615 * rgb.r - 0.515 * rgb.g - 0.100 * rgb.b;
      float w = wH[dx + 2] * 0.5;  // 0.5 for vertical 2-row average
      uSum += U * w;
      vSum += V * w;
      if (dx == 0 && dy == 0) yuvCenter = vec3(Y, U, V);
    }
  }
  // Reconstruct RGB: keep center Y, replace U/V with the smoothed
  // values (the PAL viewer's eye sees luma at full bandwidth and chroma
  // smeared horizontally + vertically across line pairs).
  vec3 yuv = vec3(yuvCenter.x, uSum, vSum);
  return vec3(
    yuv.x + 1.140 * yuv.z,
    yuv.x - 0.395 * yuv.y - 0.581 * yuv.z,
    yuv.x + 2.032 * yuv.y
  );
}

// Slot-mask phosphor pattern. Cosine-modulated rather than hard-edged
// because real 0.42mm pitch on common displays falls below the 3-stripe
// hard-mask Nyquist (≤4 device px per triad → each stripe sub-pixel,
// aliases into chroma noise). The three channels are 120° out of phase
// so their sum is constant — total luminance is preserved exactly,
// unlike the old 50/50 ramp that needed u_brightness=1.25 compensation.
//
// The diagonal stagger (pos.x += pos.y * period/2) gives the slot-mask
// look — every second row shifts by half a triad, distinguishing it
// from a pure aperture grille's vertical stripes. u_maskPeriod is in
// device pixels and is driven by the caller from
// (0.42mm × 96/25.4 × DPR), floored at 3 device px so even at extreme
// low-DPI the cosine stays at-or-above Nyquist.
vec3 mask(vec2 pos) {
  pos.x += pos.y * (u_maskPeriod * 0.5);
  float phase = pos.x * 6.2831853 / u_maskPeriod;
  // 120° offsets: R at 0, G at 2π/3, B at 4π/3. The 0.5+0.5*cos maps
  // each channel into [0, 1] before lerping into [maskDark, maskLight].
  vec3 raw = vec3(
    0.5 + 0.5 * cos(phase),
    0.5 + 0.5 * cos(phase - 2.0943951),
    0.5 + 0.5 * cos(phase - 4.1887902)
  );
  return mix(vec3(u_maskDark), vec3(u_maskLight), raw);
}

void main() {
  vec2 uv = v_uv;
  vec3 col = tri(uv);
  // Add bloom on top — only the parts brighter than a threshold halate.
  col += u_bloom * bloom(uv);

  // C64 PAL composite path: replace col's chroma with the smoothed
  // PAL-blurred chroma and add a horizontal R/B shift to model imperfect
  // beam convergence. The slot mask + scanlines below stay the same —
  // a CRT's mask + beam profile work the same regardless of input
  // signal type.
  if (u_palMode > 0.5) {
    vec3 palCol = palChroma(uv);
    // Keep luma from the existing tri() result (sharp) and chroma from
    // palCol (smoothed).
    float Y =  0.299 * col.r + 0.587 * col.g + 0.114 * col.b;
    float U = -0.147 * palCol.r - 0.289 * palCol.g + 0.436 * palCol.b;
    float V =  0.615 * palCol.r - 0.515 * palCol.g - 0.100 * palCol.b;
    col = vec3(Y + 1.140 * V,
               Y - 0.395 * U - 0.581 * V,
               Y + 2.032 * U);
    // Chromatic aberration: shift R sample left, B sample right by ~1
    // source pixel. Cheap proxy for poor convergence — gives the
    // characteristic red/blue fringe on white edges.
    vec2 ts = 1.0 / u_srcSize;
    vec3 lcol = tri(uv + vec2(-ts.x, 0.0));
    vec3 rcol = tri(uv + vec2( ts.x, 0.0));
    col.r = mix(col.r, lcol.r, 0.5);
    col.b = mix(col.b, rcol.b, 0.5);
  }

  col *= mask(gl_FragCoord.xy);
  col *= u_brightness;
  gl_FragColor = vec4(toSrgb(col), 1.0);
}
`

export interface CrtRenderer {
  render: (rgba: Uint8Array, srcW: number, srcH: number, dstW: number, dstH: number) => void
  // Toggle PAL composite simulation (chroma blur + delay-line averaging +
  // chromatic aberration). On for c64 modes (TV via composite), off for
  // amiga modes (1084S RGB monitor). Default: off.
  setPalMode: (enabled: boolean) => void
  // Set the slot-mask period in *device pixels per RGB triad*. Caller
  // computes this from the display's pixel density and 0.42mm (1084S
  // dot pitch). Default: enough for the cosine to render smoothly
  // (≥3 device px, Nyquist limit). Re-call when DPR changes.
  setMaskPeriod: (periodDevicePx: number) => void
  // Tell the renderer the source is in an Amiga interlace mode
  // (-lace suffix). Drives the visual tuning (softer scanlines, less
  // bloom, equalized brightness) AND enables the 60 Hz field-flicker
  // animation. While flicker is on the renderer runs its own
  // requestAnimationFrame loop and re-renders the cached texture each
  // frame with updated u_time, so caller doesn't drive the animation.
  setInterlaceMode: (enabled: boolean) => void
  dispose: () => void
}

function compile(gl: WebGLRenderingContext, type: number, src: string): WebGLShader {
  const sh = gl.createShader(type)
  if (!sh) throw new Error('CRT shader create failed')
  gl.shaderSource(sh, src)
  gl.compileShader(sh)
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(sh)
    gl.deleteShader(sh)
    throw new Error(`CRT shader compile error: ${log ?? '(no log)'}`)
  }
  return sh
}

function link(gl: WebGLRenderingContext, vs: WebGLShader, fs: WebGLShader): WebGLProgram {
  const p = gl.createProgram()
  // Modern lib.dom.d.ts types createProgram() as non-null, but at runtime it
  // returns null on lost context / out-of-memory. Keep the guard.
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (!p) throw new Error('CRT program create failed')
  gl.attachShader(p, vs)
  gl.attachShader(p, fs)
  gl.linkProgram(p)
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(p)
    gl.deleteProgram(p)
    throw new Error(`CRT program link error: ${log ?? '(no log)'}`)
  }
  return p
}

export function createCrtRenderer(canvas: HTMLCanvasElement): CrtRenderer {
  const ctx = canvas.getContext('webgl', {
    premultipliedAlpha: false,
    alpha: false,
    antialias: false,
    preserveDrawingBuffer: false,
  })
  if (!ctx) throw new Error('WebGL not available')
  // Capture into a non-nullable local so TS keeps the narrowing inside
  // the closures we return (render / dispose).
  const gl: WebGLRenderingContext = ctx

  const vs = compile(gl, gl.VERTEX_SHADER, VERT)
  const fs = compile(gl, gl.FRAGMENT_SHADER, FRAG)
  const program = link(gl, vs, fs)

  const buf = gl.createBuffer()
  gl.bindBuffer(gl.ARRAY_BUFFER, buf)
  gl.bufferData(gl.ARRAY_BUFFER,
    new Float32Array([-1, -1,  1, -1, -1,  1,  1,  1]), gl.STATIC_DRAW)

  const tex = gl.createTexture()
  gl.bindTexture(gl.TEXTURE_2D, tex)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)

  const u = {
    src:        gl.getUniformLocation(program, 'u_src'),
    srcSize:    gl.getUniformLocation(program, 'u_srcSize'),
    outSize:    gl.getUniformLocation(program, 'u_outSize'),
    hardScan:   gl.getUniformLocation(program, 'u_hardScan'),
    hardPix:    gl.getUniformLocation(program, 'u_hardPix'),
    maskDark:   gl.getUniformLocation(program, 'u_maskDark'),
    maskLight:  gl.getUniformLocation(program, 'u_maskLight'),
    bloom:      gl.getUniformLocation(program, 'u_bloom'),
    brightness: gl.getUniformLocation(program, 'u_brightness'),
    palMode:    gl.getUniformLocation(program, 'u_palMode'),
    maskPeriod: gl.getUniformLocation(program, 'u_maskPeriod'),
    interlaceFlicker: gl.getUniformLocation(program, 'u_interlaceFlicker'),
    phosphorPersist:  gl.getUniformLocation(program, 'u_phosphorPersist'),
    time:             gl.getUniformLocation(program, 'u_time'),
  }
  const aPos = gl.getAttribLocation(program, 'a_pos')

  let palMode = 0
  // Sensible default — typical CSS-96-dpi display at DPR=1 gives
  // 0.42mm × 3.78 px/mm ≈ 1.6 device px, clamped to the 3-px Nyquist
  // floor below. Caller should setMaskPeriod with the live DPR-driven
  // value before each render.
  let maskPeriod = 3
  let interlaceFlicker = 0
  // Off-field row brightness. 0 = full flicker (off rows go black,
  // headache-inducing); 1 = no flicker. 0.4 keeps the flicker visible
  // on horizontal edges (which is the artifact we're modeling) while
  // staying watchable on uniform fields.
  const phosphorPersist = 0.4
  // Cached params from the last full render(), so the RAF tick can
  // re-render with the new u_time without re-uploading the texture or
  // recomputing per-mode parameters.
  let lastSrcW = 0, lastSrcH = 0, lastDstW = 0, lastDstH = 0
  let rafHandle = 0
  // Wall-clock seconds at renderer creation — subtracted from
  // performance.now() to keep u_time in a small, well-conditioned float
  // (avoids precision loss after the page has been open for hours).
  const epoch = performance.now() / 1000

  function setPalMode(enabled: boolean): void {
    palMode = enabled ? 1 : 0
  }

  function setMaskPeriod(periodDevicePx: number): void {
    // Floor at 3 — the cosine mask needs ≥3 samples per period to stay
    // above Nyquist (sum-of-cosines aliases below that and the channels
    // shift into a single muddy color). Negative / non-finite inputs
    // would also crash the shader, so reject those.
    maskPeriod = Math.max(3, Number.isFinite(periodDevicePx) ? periodDevicePx : 3)
  }

  // Sticky "the source mode is interlace" flag. Drives BOTH the
  // flicker animation AND the visual tuning (softer scanlines, less
  // bloom, equalized brightness). Caller is the source of truth —
  // the prior source-height heuristic (h ≥ 280) failed on short
  // interlace content like a 320×238 logo.
  let isInterlaceMode = false

  function setInterlaceMode(enabled: boolean): void {
    isInterlaceMode = enabled
    const want = enabled ? 1 : 0
    if (want === interlaceFlicker) return
    interlaceFlicker = want
    if (want && rafHandle === 0 && lastSrcW > 0) startTickLoop()
    if (!want && rafHandle !== 0) stopTickLoop()
  }

  function startTickLoop(): void {
    const tick = (): void => {
      // Re-render with the cached source — the texture is still bound
      // from the last render() call, so just push the new u_time and
      // redraw. ~10 µs of GPU per frame.
      if (lastSrcW > 0) drawCached()
      rafHandle = requestAnimationFrame(tick)
    }
    rafHandle = requestAnimationFrame(tick)
  }

  function stopTickLoop(): void {
    if (rafHandle !== 0) cancelAnimationFrame(rafHandle)
    rafHandle = 0
  }

  function drawCached(): void {
    // Per-mode shader parameter tuning. Three regimes:
    //
    //   * lores progressive (320×~200, square-ish): full-strength
    //     scanlines and moderate horizontal sharpness. The reference
    //     1084S look.
    //   * hires (640+ wide): horizontal Gaussian tightened so 640-wide
    //     pixel-art content stays crisp. The 1084S has higher RGB
    //     bandwidth than its TV-grade peers, and at hires resolutions
    //     the 3-tap reconstruction would otherwise visibly soften
    //     1-pixel-wide vertical features.
    //   * interlace: scanlines flattened toward zero because both
    //     fields fill the tube and the dark gap between source rows
    //     that would normally read as a scanline is filled in by the
    //     alternate field on real hardware. Driven by the caller-
    //     supplied mode flag, not source height — short interlace
    //     content (e.g. 320×238 logo) needs the same tuning.
    const isHires     = lastSrcW >= 480
    const isInterlace = isInterlaceMode
    const hardScan = isInterlace ? -1 : -8
    const hardPix  = isHires     ? -5 : -3
    // Bloom contributes more in interlace / hires (where the per-row
    // beam is thinner relative to the visible structure).
    const bloom    = isInterlace ? 0.1 : 0.18
    // Brightness compensation. tri()'s peak-sum normalization makes
    // both modes peak at the source luminance, but progressive still
    // has dark valleys between scanlines — average over y is ≈ 0.75
    // of source vs interlace's ≈ 1.0 (interlace's softer Gaussian has
    // negligible valleys). Boost progressive ~25 % so the on-screen
    // average matches across modes; otherwise switching to interlace
    // looks markedly brighter. Bloom also adds a small DC component
    // (slightly more in progressive after normalization), which
    // contributes ~5 % of the equalization on its own.
    let brightness = isInterlace ? 1 : 1.2
    // Interlace flicker dims the time-average per row to (1+p)/2.
    // Compensate so the static brightness matches the no-flicker case
    // — only the temporal variation differs, not the mean level.
    if (interlaceFlicker > 0.5) {
      brightness *= 2 / (1 + phosphorPersist)
    }

    gl.viewport(0, 0, lastDstW, lastDstH)
    gl.clearColor(0, 0, 0, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)

    gl.useProgram(program)
    gl.activeTexture(gl.TEXTURE0)
    gl.bindTexture(gl.TEXTURE_2D, tex)
    gl.uniform1i(u.src, 0)
    gl.uniform2f(u.srcSize, lastSrcW, lastSrcH)
    gl.uniform2f(u.outSize, lastDstW, lastDstH)
    gl.uniform1f(u.hardScan,   hardScan)
    gl.uniform1f(u.hardPix,    hardPix)
    gl.uniform1f(u.maskDark,   0.5)
    gl.uniform1f(u.maskLight,  1.5)
    gl.uniform1f(u.bloom,      bloom)
    gl.uniform1f(u.brightness, brightness)
    gl.uniform1f(u.palMode,    palMode)
    gl.uniform1f(u.maskPeriod, maskPeriod)
    gl.uniform1f(u.interlaceFlicker, interlaceFlicker)
    gl.uniform1f(u.phosphorPersist,  phosphorPersist)
    gl.uniform1f(u.time,             performance.now() / 1000 - epoch)

    gl.bindBuffer(gl.ARRAY_BUFFER, buf)
    gl.enableVertexAttribArray(aPos)
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
  }

  function render(rgba: Uint8Array, srcW: number, srcH: number, dstW: number, dstH: number): void {
    if (canvas.width !== dstW)  canvas.width  = dstW
    if (canvas.height !== dstH) canvas.height = dstH

    gl.bindTexture(gl.TEXTURE_2D, tex)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, srcW, srcH, 0,
                  gl.RGBA, gl.UNSIGNED_BYTE, rgba)

    lastSrcW = srcW; lastSrcH = srcH; lastDstW = dstW; lastDstH = dstH
    drawCached()

    // If flicker should be running but the loop isn't (e.g. caller
    // toggled flicker on before the first render), kick it off now.
    if (interlaceFlicker > 0.5 && rafHandle === 0) startTickLoop()
  }

  function dispose(): void {
    stopTickLoop()
    gl.deleteProgram(program)
    gl.deleteShader(vs)
    gl.deleteShader(fs)
    gl.deleteBuffer(buf)
    gl.deleteTexture(tex)
  }

  return { render, setPalMode, setMaskPeriod, setInterlaceMode, dispose }
}
