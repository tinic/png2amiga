const E=`#version 100
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}
`,S=`#version 100
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
// floor()ed so fetch always lands on the canonical centre.
vec3 fetch(vec2 pos, vec2 off) {
  pos = (floor(pos * u_srcSize + off) + vec2(0.5)) / u_srcSize;
  if (any(lessThan(pos, vec2(0.0))) || any(greaterThan(pos, vec2(1.0)))) {
    return vec3(0.0);
  }
  return toLinear(texture2D(u_src, pos).rgb);
}

// Distance to nearest texel centre (range [-0.5, 0.5]).
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
  // Gaussian weights centred on dst with hardPix sharpness
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

// Slot-mask phosphor pattern. Each output pixel sits on one of three
// phosphor stripes (R, G, or B). The "+ pos.y * 3.0" creates the
// signature diagonal stagger that distinguishes slot masks from pure
// vertical aperture grilles. Period of 6 output pixels for the RGB
// triad gives ~0.42mm dot pitch when the canvas is rendered at the
// 1084S display size on a typical modern monitor.
vec3 mask(vec2 pos) {
  pos.x += pos.y * 3.0;
  vec3 m = vec3(u_maskDark);
  pos.x = fract(pos.x / 6.0);
  if (pos.x < 0.333)      m.r = u_maskLight;
  else if (pos.x < 0.666) m.g = u_maskLight;
  else                    m.b = u_maskLight;
  return m;
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
  col *= mask(gl_FragCoord.xy);
  col *= u_brightness;
  gl_FragColor = vec4(toSrgb(col), 1.0);
}
`;function m(o,e,s){const t=o.createShader(e);if(o.shaderSource(t,s),o.compileShader(t),!o.getShaderParameter(t,o.COMPILE_STATUS)){const r=o.getShaderInfoLog(t);throw o.deleteShader(t),new Error(`CRT shader compile error: ${r}`)}return t}function R(o,e,s){const t=o.createProgram();if(o.attachShader(t,e),o.attachShader(t,s),o.linkProgram(t),!o.getProgramParameter(t,o.LINK_STATUS)){const r=o.getProgramInfoLog(t);throw o.deleteProgram(t),new Error(`CRT program link error: ${r}`)}return t}function P(o){const e=o.getContext("webgl",{premultipliedAlpha:!1,alpha:!1,antialias:!1,preserveDrawingBuffer:!1});if(!e)throw new Error("WebGL not available");const s=m(e,e.VERTEX_SHADER,E),t=m(e,e.FRAGMENT_SHADER,S),r=R(e,s,t),f=e.createBuffer();e.bindBuffer(e.ARRAY_BUFFER,f),e.bufferData(e.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),e.STATIC_DRAW);const i=e.createTexture();e.bindTexture(e.TEXTURE_2D,i),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MIN_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_MAG_FILTER,e.NEAREST),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_S,e.CLAMP_TO_EDGE),e.texParameteri(e.TEXTURE_2D,e.TEXTURE_WRAP_T,e.CLAMP_TO_EDGE);const a={src:e.getUniformLocation(r,"u_src"),srcSize:e.getUniformLocation(r,"u_srcSize"),outSize:e.getUniformLocation(r,"u_outSize"),hardScan:e.getUniformLocation(r,"u_hardScan"),hardPix:e.getUniformLocation(r,"u_hardPix"),maskDark:e.getUniformLocation(r,"u_maskDark"),maskLight:e.getUniformLocation(r,"u_maskLight"),warpX:e.getUniformLocation(r,"u_warpX"),warpY:e.getUniformLocation(r,"u_warpY"),bloom:e.getUniformLocation(r,"u_bloom"),brightness:e.getUniformLocation(r,"u_brightness")},h=e.getAttribLocation(r,"a_pos");function d(_,u,l,c,n){o.width!==c&&(o.width=c),o.height!==n&&(o.height=n),e.viewport(0,0,c,n),e.clearColor(0,0,0,1),e.clear(e.COLOR_BUFFER_BIT),e.bindTexture(e.TEXTURE_2D,i),e.pixelStorei(e.UNPACK_FLIP_Y_WEBGL,!0),e.texImage2D(e.TEXTURE_2D,0,e.RGBA,u,l,0,e.RGBA,e.UNSIGNED_BYTE,_);const g=u>=480,p=l>=280,b=p?-1:-8,w=g?-5:-3,T=p?.1:.18,x=p?1.1:1.25;e.useProgram(r),e.activeTexture(e.TEXTURE0),e.bindTexture(e.TEXTURE_2D,i),e.uniform1i(a.src,0),e.uniform2f(a.srcSize,u,l),e.uniform2f(a.outSize,c,n),e.uniform1f(a.hardScan,b),e.uniform1f(a.hardPix,w),e.uniform1f(a.maskDark,.5),e.uniform1f(a.maskLight,1.5),e.uniform1f(a.warpX,1/96),e.uniform1f(a.warpY,1/72),e.uniform1f(a.bloom,T),e.uniform1f(a.brightness,x),e.bindBuffer(e.ARRAY_BUFFER,f),e.enableVertexAttribArray(h),e.vertexAttribPointer(h,2,e.FLOAT,!1,0,0),e.drawArrays(e.TRIANGLE_STRIP,0,4)}function v(){e.deleteProgram(r),e.deleteShader(s),e.deleteShader(t),e.deleteBuffer(f),e.deleteTexture(i)}return{render:d,dispose:v}}export{P as createCrtRenderer};
