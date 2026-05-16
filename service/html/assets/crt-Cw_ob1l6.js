const k=`#version 100
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`,U=`#version 100
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

// Three-line composite: weighted sum of three vertically-adjacent
// horizontally-filtered rows, each multiplied by its scanline weight.
vec3 tri(vec2 pos) {
  vec3 a = horz3(pos, -1.0);
  vec3 b = horz3(pos,  0.0);
  vec3 c = horz3(pos,  1.0);
  return a * scan(pos, -1.0) + b * scan(pos, 0.0) + c * scan(pos, 1.0);
}

// Bloom: 5×5-ish wider Gaussian over 5 rows, used to add halation around
// bright phosphors (the diffuse glow real CRTs exhibit).
vec3 bloom(vec2 pos) {
  vec3 a = horz5(pos, -2.0);
  vec3 b = horz5(pos, -1.0);
  vec3 c = horz5(pos,  0.0);
  vec3 d = horz5(pos,  1.0);
  vec3 e = horz5(pos,  2.0);
  return a * bloomScan(pos, -2.0)
       + b * bloomScan(pos, -1.0)
       + c * bloomScan(pos,  0.0)
       + d * bloomScan(pos,  1.0)
       + e * bloomScan(pos,  2.0);
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

// Subtle barrel warp. 1084S CRTs are fairly flat compared to a 70s TV
// so the warp factors are small. Set u_warpX / u_warpY to 0 to disable.
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
`;function b(o,s,e){const r=o.createShader(s);if(!r)throw new Error("CRT shader create failed");if(o.shaderSource(r,e),o.compileShader(r),!o.getShaderParameter(r,o.COMPILE_STATUS)){const i=o.getShaderInfoLog(r);throw o.deleteShader(r),new Error(`CRT shader compile error: ${i??"(no log)"}`)}return r}function L(o,s,e){const r=o.createProgram();if(!r)throw new Error("CRT program create failed");if(o.attachShader(r,s),o.attachShader(r,e),o.linkProgram(r),!o.getProgramParameter(r,o.LINK_STATUS)){const i=o.getProgramInfoLog(r);throw o.deleteProgram(r),new Error(`CRT program link error: ${i??"(no log)"}`)}return r}function A(o){const s=o.getContext("webgl",{premultipliedAlpha:!1,alpha:!1,antialias:!1,preserveDrawingBuffer:!1});if(!s)throw new Error("WebGL not available");const e=s,r=b(e,e.VERTEX_SHADER,k),i=b(e,e.FRAGMENT_SHADER,U),a=L(e,r,i),u=e.createBuffer();e.bindBuffer(e.ARRAY_BUFFER,u),e.bufferData(e.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),e.STATIC_DRAW);const n=e.createTexture();e.bindTexture(e.TEXTURE_2D,n),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MIN_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MAG_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_S,e.CLAMP_TO_EDGE),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_T,e.CLAMP_TO_EDGE);const t={src:e.getUniformLocation(a,"u_src"),srcSize:e.getUniformLocation(a,"u_srcSize"),outSize:e.getUniformLocation(a,"u_outSize"),hardScan:e.getUniformLocation(a,"u_hardScan"),hardPix:e.getUniformLocation(a,"u_hardPix"),maskDark:e.getUniformLocation(a,"u_maskDark"),maskLight:e.getUniformLocation(a,"u_maskLight"),warpX:e.getUniformLocation(a,"u_warpX"),warpY:e.getUniformLocation(a,"u_warpY"),bloom:e.getUniformLocation(a,"u_bloom"),brightness:e.getUniformLocation(a,"u_brightness"),palMode:e.getUniformLocation(a,"u_palMode"),maskPeriod:e.getUniformLocation(a,"u_maskPeriod")},d=e.getAttribLocation(a,"a_pos");let v=0,g=3;function w(c){v=c?1:0}function _(c){g=Math.max(3,Number.isFinite(c)?c:3)}function x(c,p,h,l,f){o.width!==l&&(o.width=l),o.height!==f&&(o.height=f),e.viewport(0,0,l,f),e.clearColor(0,0,0,1),e.clear(e.COLOR_BUFFER_BIT),e.bindTexture(e.TEXTURE_2D,n),e.pixelStorei(e.UNPACK_FLIP_Y_WEBGL,!0),e.texImage2D(e.TEXTURE_2D,0,e.RGBA,p,h,0,e.RGBA,e.UNSIGNED_BYTE,c);const S=p>=480,m=h>=280,P=m?-1:-8,R=S?-5:-3,y=m?.1:.18,E=m?1:1.1;e.useProgram(a),e.activeTexture(e.TEXTURE0),e.bindTexture(e.TEXTURE_2D,n),e.uniform1i(t.src,0),e.uniform2f(t.srcSize,p,h),e.uniform2f(t.outSize,l,f),e.uniform1f(t.hardScan,P),e.uniform1f(t.hardPix,R),e.uniform1f(t.maskDark,.5),e.uniform1f(t.maskLight,1.5),e.uniform1f(t.warpX,1/96),e.uniform1f(t.warpY,1/72),e.uniform1f(t.bloom,y),e.uniform1f(t.brightness,E),e.uniform1f(t.palMode,v),e.uniform1f(t.maskPeriod,g),e.bindBuffer(e.ARRAY_BUFFER,u),e.enableVertexAttribArray(d),e.vertexAttribPointer(d,2,e.FLOAT,!1,0,0),e.drawArrays(e.TRIANGLE_STRIP,0,4)}function T(){e.deleteProgram(a),e.deleteShader(r),e.deleteShader(i),e.deleteBuffer(u),e.deleteTexture(n)}return{render:x,setPalMode:w,setMaskPeriod:_,dispose:T}}export{A as createCrtRenderer};
