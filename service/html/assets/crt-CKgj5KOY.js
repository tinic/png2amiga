function e(e,t,n){let r=e.createShader(t);if(!r)throw Error(`CRT shader create failed`);if(e.shaderSource(r,n),e.compileShader(r),!e.getShaderParameter(r,e.COMPILE_STATUS)){let t=e.getShaderInfoLog(r);throw e.deleteShader(r),Error(`CRT shader compile error: ${t??`(no log)`}`)}return r}function t(e,t,n){let r=e.createProgram();if(!r)throw Error(`CRT program create failed`);if(e.attachShader(r,t),e.attachShader(r,n),e.linkProgram(r),!e.getProgramParameter(r,e.LINK_STATUS)){let t=e.getProgramInfoLog(r);throw e.deleteProgram(r),Error(`CRT program link error: ${t??`(no log)`}`)}return r}function n(n){let r=n.getContext(`webgl`,{premultipliedAlpha:!1,alpha:!1,antialias:!1,preserveDrawingBuffer:!1});if(!r)throw Error(`WebGL not available`);let i=r,a=e(i,i.VERTEX_SHADER,`#version 100
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`),o=e(i,i.FRAGMENT_SHADER,`#version 100
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
`),s=t(i,a,o),c=i.createBuffer();i.bindBuffer(i.ARRAY_BUFFER,c),i.bufferData(i.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),i.STATIC_DRAW);let l=i.createTexture();i.bindTexture(i.TEXTURE_2D,l),i.texParameteri(i.TEXTURE_2D,i.TEXTURE_MIN_FILTER,i.NEAREST),i.texParameteri(i.TEXTURE_2D,i.TEXTURE_MAG_FILTER,i.NEAREST),i.texParameteri(i.TEXTURE_2D,i.TEXTURE_WRAP_S,i.CLAMP_TO_EDGE),i.texParameteri(i.TEXTURE_2D,i.TEXTURE_WRAP_T,i.CLAMP_TO_EDGE);let u={src:i.getUniformLocation(s,`u_src`),srcSize:i.getUniformLocation(s,`u_srcSize`),outSize:i.getUniformLocation(s,`u_outSize`),hardScan:i.getUniformLocation(s,`u_hardScan`),hardPix:i.getUniformLocation(s,`u_hardPix`),maskDark:i.getUniformLocation(s,`u_maskDark`),maskLight:i.getUniformLocation(s,`u_maskLight`),bloom:i.getUniformLocation(s,`u_bloom`),brightness:i.getUniformLocation(s,`u_brightness`),palMode:i.getUniformLocation(s,`u_palMode`),maskPeriod:i.getUniformLocation(s,`u_maskPeriod`),interlaceFlicker:i.getUniformLocation(s,`u_interlaceFlicker`),phosphorPersist:i.getUniformLocation(s,`u_phosphorPersist`),time:i.getUniformLocation(s,`u_time`)},d=i.getAttribLocation(s,`a_pos`),f=0,p=3,m=0,h=0,g=0,_=0,v=0,y=0,b=performance.now()/1e3;function x(e){f=+!!e}function S(e){p=Math.max(3,Number.isFinite(e)?e:3)}let C=!1;function w(e){C=e;let t=+!!e;t!==m&&(m=t,t&&y===0&&h>0&&T(),!t&&y!==0&&E())}function T(){let e=()=>{h>0&&D(),y=requestAnimationFrame(e)};y=requestAnimationFrame(e)}function E(){y!==0&&cancelAnimationFrame(y),y=0}function D(){let e=h>=480,t=C,n=t?-1:-8,r=e?-5:-3,a=t?.1:.18,o=t?1:1.2;m>.5&&(o*=2/1.4),i.viewport(0,0,_,v),i.clearColor(0,0,0,1),i.clear(i.COLOR_BUFFER_BIT),i.useProgram(s),i.activeTexture(i.TEXTURE0),i.bindTexture(i.TEXTURE_2D,l),i.uniform1i(u.src,0),i.uniform2f(u.srcSize,h,g),i.uniform2f(u.outSize,_,v),i.uniform1f(u.hardScan,n),i.uniform1f(u.hardPix,r),i.uniform1f(u.maskDark,.5),i.uniform1f(u.maskLight,1.5),i.uniform1f(u.bloom,a),i.uniform1f(u.brightness,o),i.uniform1f(u.palMode,f),i.uniform1f(u.maskPeriod,p),i.uniform1f(u.interlaceFlicker,m),i.uniform1f(u.phosphorPersist,.4),i.uniform1f(u.time,performance.now()/1e3-b),i.bindBuffer(i.ARRAY_BUFFER,c),i.enableVertexAttribArray(d),i.vertexAttribPointer(d,2,i.FLOAT,!1,0,0),i.drawArrays(i.TRIANGLE_STRIP,0,4)}function O(e,t,r,a,o){n.width!==a&&(n.width=a),n.height!==o&&(n.height=o),i.bindTexture(i.TEXTURE_2D,l),i.pixelStorei(i.UNPACK_FLIP_Y_WEBGL,!0),i.texImage2D(i.TEXTURE_2D,0,i.RGBA,t,r,0,i.RGBA,i.UNSIGNED_BYTE,e),h=t,g=r,_=a,v=o,D(),m>.5&&y===0&&T()}function k(){E(),i.deleteProgram(s),i.deleteShader(a),i.deleteShader(o),i.deleteBuffer(c),i.deleteTexture(l)}return{render:O,setPalMode:x,setMaskPeriod:S,setInterlaceMode:w,dispose:k}}export{n as createCrtRenderer};