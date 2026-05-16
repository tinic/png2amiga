const G=`#version 100
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`,B=`#version 100
precision mediump float;
varying vec2 v_uv;

uniform sampler2D u_src;
uniform vec2 u_srcSize;        // source pixel grid (e.g. 320x213)
uniform vec2 u_outSize;        // output canvas size in physical pixels
uniform float u_hardScan;
uniform float u_hardPix;
uniform float u_maskDark;
uniform float u_maskLight;
uniform float u_warpX;
uniform float u_warpY;
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
vec3 toLinear(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(2.2));
}
vec3 toSrgb(vec3 c) {
  return pow(max(c, vec3(0.0)), vec3(1.0 / 2.4));
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

// Geometric residual warp — what's left of the tube's deflection
// distortion after the chassis pincushion-correction circuit kicks
// in. Lottes form: only off-axis points bow, so center-axis lines
// (the central crosshair) stay straight — matches what a real, well-
// adjusted 1084S looks like under a service-manual cross-hatch test
// pattern. kx > ky reflects the 90° tube's larger horizontal
// deflection angle (≈38.7° H vs ≈31° V on 4:3); horizontal bow is
// always ≥ vertical bow on a real 1084S. See the warpX/warpY uniform
// assignments for the numeric derivation.
//
// Inverse mapping: warp() takes the output uv and returns the source
// uv to sample. Positive kx, ky push source-corner samples outside
// [0,1] → output corners go to the bezel; the visible image bulges
// at the screen-edge midpoints (a horizontal line near the top of the
// source appears highest in the middle, lower at its ends). That's
// the "outward bow" residual a real 1084S exhibits — slight over-
// correction of the deflection pincushion. Flipping the signs would
// invert it into the under-corrected (inward bow) regime.
vec2 warp(vec2 uv) {
  uv = uv * 2.0 - 1.0;
  uv *= vec2(1.0 + (uv.y * uv.y) * u_warpX,
             1.0 + (uv.x * uv.x) * u_warpY);
  return uv * 0.5 + 0.5;
}

void main() {
  vec2 uv = warp(v_uv);
  // Discard pixels that warped outside the source image — keeps the
  // black "bezel" around a curved-tube look.
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
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
`;function z(o,c,e){const r=o.createShader(c);if(!r)throw new Error("CRT shader create failed");if(o.shaderSource(r,e),o.compileShader(r),!o.getShaderParameter(r,o.COMPILE_STATUS)){const l=o.getShaderInfoLog(r);throw o.deleteShader(r),new Error(`CRT shader compile error: ${l??"(no log)"}`)}return r}function H(o,c,e){const r=o.createProgram();if(!r)throw new Error("CRT program create failed");if(o.attachShader(r,c),o.attachShader(r,e),o.linkProgram(r),!o.getProgramParameter(r,o.LINK_STATUS)){const l=o.getProgramInfoLog(r);throw o.deleteProgram(r),new Error(`CRT program link error: ${l??"(no log)"}`)}return r}function I(o){const c=o.getContext("webgl",{premultipliedAlpha:!1,alpha:!1,antialias:!1,preserveDrawingBuffer:!1});if(!c)throw new Error("WebGL not available");const e=c,r=z(e,e.VERTEX_SHADER,G),l=z(e,e.FRAGMENT_SHADER,B),t=H(e,r,l),v=e.createBuffer();e.bindBuffer(e.ARRAY_BUFFER,v),e.bufferData(e.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),e.STATIC_DRAW);const m=e.createTexture();e.bindTexture(e.TEXTURE_2D,m),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MIN_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MAG_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_S,e.CLAMP_TO_EDGE),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_T,e.CLAMP_TO_EDGE);const a={src:e.getUniformLocation(t,"u_src"),srcSize:e.getUniformLocation(t,"u_srcSize"),outSize:e.getUniformLocation(t,"u_outSize"),hardScan:e.getUniformLocation(t,"u_hardScan"),hardPix:e.getUniformLocation(t,"u_hardPix"),maskDark:e.getUniformLocation(t,"u_maskDark"),maskLight:e.getUniformLocation(t,"u_maskLight"),warpX:e.getUniformLocation(t,"u_warpX"),warpY:e.getUniformLocation(t,"u_warpY"),bloom:e.getUniformLocation(t,"u_bloom"),brightness:e.getUniformLocation(t,"u_brightness"),palMode:e.getUniformLocation(t,"u_palMode"),maskPeriod:e.getUniformLocation(t,"u_maskPeriod"),interlaceFlicker:e.getUniformLocation(t,"u_interlaceFlicker"),phosphorPersist:e.getUniformLocation(t,"u_phosphorPersist"),time:e.getUniformLocation(t,"u_time")},b=e.getAttribLocation(t,"a_pos");let _=0,x=3,f=0;const k=.4;let u=0,y=0,g=0,w=0,n=0;const L=performance.now()/1e3;function U(i){_=i?1:0}function A(i){x=Math.max(3,Number.isFinite(i)?i:3)}let S=!1;function C(i){S=i;const s=i?1:0;s!==f&&(f=s,s&&n===0&&u>0&&T(),!s&&n!==0&&P())}function T(){const i=()=>{u>0&&R(),n=requestAnimationFrame(i)};n=requestAnimationFrame(i)}function P(){n!==0&&cancelAnimationFrame(n),n=0}function R(){const i=u>=480,s=S,d=s?-1:-8,h=i?-5:-3,p=s?.1:.18;let E=s?1:1.2;f>.5&&(E*=2/(1+k)),e.viewport(0,0,g,w),e.clearColor(0,0,0,1),e.clear(e.COLOR_BUFFER_BIT),e.useProgram(t),e.activeTexture(e.TEXTURE0),e.bindTexture(e.TEXTURE_2D,m),e.uniform1i(a.src,0),e.uniform2f(a.srcSize,u,y),e.uniform2f(a.outSize,g,w),e.uniform1f(a.hardScan,d),e.uniform1f(a.hardPix,h),e.uniform1f(a.maskDark,.5),e.uniform1f(a.maskLight,1.5),e.uniform1f(a.warpX,.03),e.uniform1f(a.warpY,.022),e.uniform1f(a.bloom,p),e.uniform1f(a.brightness,E),e.uniform1f(a.palMode,_),e.uniform1f(a.maskPeriod,x),e.uniform1f(a.interlaceFlicker,f),e.uniform1f(a.phosphorPersist,k),e.uniform1f(a.time,performance.now()/1e3-L),e.bindBuffer(e.ARRAY_BUFFER,v),e.enableVertexAttribArray(b),e.vertexAttribPointer(b,2,e.FLOAT,!1,0,0),e.drawArrays(e.TRIANGLE_STRIP,0,4)}function F(i,s,d,h,p){o.width!==h&&(o.width=h),o.height!==p&&(o.height=p),e.bindTexture(e.TEXTURE_2D,m),e.pixelStorei(e.UNPACK_FLIP_Y_WEBGL,!0),e.texImage2D(e.TEXTURE_2D,0,e.RGBA,s,d,0,e.RGBA,e.UNSIGNED_BYTE,i),u=s,y=d,g=h,w=p,R(),f>.5&&n===0&&T()}function D(){P(),e.deleteProgram(t),e.deleteShader(r),e.deleteShader(l),e.deleteBuffer(v),e.deleteTexture(m)}return{render:F,setPalMode:U,setMaskPeriod:A,setInterlaceMode:C,dispose:D}}export{I as createCrtRenderer};
