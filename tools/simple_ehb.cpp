// Simplest-possible-honest EHB encoder.
//
//   Wu quantize → 32 base colours.
//   Twin-aware k-means refine (joint base + half-brite pairing).
//   Build 64-effective palette (32 base + sRGB-halved twins).
//   FS error diffusion, serpentine, in OKLab perceptual nearest.
//   Output as PNG (preview, no copper, no SCAP, no --best gymnastics).
//
// Build:
//   clang++ -std=c++20 -O2 -I/Users/turo/png2amiga/third_party \
//     /tmp/simple_ehb.cpp -o /tmp/simple_ehb
// Run:
//   /tmp/simple_ehb input.png output.png

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

// ---------- color space ----------
struct OKLab { float L, a, b; };
struct C3 { float r, g, b; };

static inline float srgb_to_linear(float s) {
    return s <= 0.04045f ? s / 12.92f
         : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
static inline float linear_to_srgb(float l) {
    return l <= 0.0031308f ? l * 12.92f
         : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

static inline OKLab linear_to_oklab(C3 c) {
    float L = 0.4122214708f*c.r + 0.5363325363f*c.g + 0.0514459929f*c.b;
    float M = 0.2119034982f*c.r + 0.6806995451f*c.g + 0.1073969566f*c.b;
    float S = 0.0883024619f*c.r + 0.2817188376f*c.g + 0.6299787005f*c.b;
    L = std::cbrt(L); M = std::cbrt(M); S = std::cbrt(S);
    return {
        0.2104542553f*L + 0.7936177850f*M - 0.0040720468f*S,
        1.9779984951f*L - 2.4285922050f*M + 0.4505937099f*S,
        0.0259040371f*L + 0.7827717662f*M - 0.8086757660f*S,
    };
}

// ---------- Wu quantizer (port of icafe4j WuQuant.java) ----------
namespace wu {
constexpr int Q = 33;
struct Box { int r0=0, r1=0, g0=0, g1=0, b0=0, b1=0, vol=0; };
using L3 = std::vector<std::vector<std::vector<long long>>>;
using F3 = std::vector<std::vector<std::vector<float>>>;
inline L3 zL() { return L3(Q, std::vector<std::vector<long long>>(Q, std::vector<long long>(Q, 0))); }
inline F3 zF() { return F3(Q, std::vector<std::vector<float>>(Q, std::vector<float>(Q, 0.0f))); }
class Wu {
public:
    Wu(const std::vector<std::uint32_t>& argb, int n)
        : pixels(argb), N(static_cast<int>(argb.size())), K(n),
          wt(zL()), mr(zL()), mg(zL()), mb(zL()), m2(zF()) {}
    int quantize(std::vector<std::uint32_t>& out) {
        Hist3d(); M3d();
        std::vector<Box> boxes(256);
        std::vector<float> v(256, 0);
        boxes[0] = {0, Q-1, 0, Q-1, 0, Q-1, 0};
        int next = 0, ks = K;
        for (int i = 1; i < ks; ++i) {
            if (Cut(boxes[next], boxes[i])) {
                v[next] = boxes[next].vol > 1 ? Var(boxes[next]) : 0.0f;
                v[i]    = boxes[i].vol > 1    ? Var(boxes[i])    : 0.0f;
            } else { v[next] = 0.0f; --i; }
            next = 0;
            float mx = v[0];
            for (int j = 1; j <= i; ++j) if (v[j] > mx) { mx = v[j]; next = j; }
            if (mx <= 0.0f) { ks = i + 1; break; }
        }
        out.assign(K, 0);
        for (int i = 0; i < K; ++i) {
            long long w = Vol(boxes[i], wt);
            if (w > 0) {
                int r = (int)(Vol(boxes[i], mr) / w);
                int g = (int)(Vol(boxes[i], mg) / w);
                int b = (int)(Vol(boxes[i], mb) / w);
                out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        return K;
    }
private:
    void Hist3d() {
        std::array<int,256> sq{};
        for (int i = 0; i < 256; ++i) sq[i] = i*i;
        for (int i = 0; i < N; ++i) {
            std::uint32_t p = pixels[i];
            int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            int ri = (r >> 3)+1, gi = (g >> 3)+1, bi = (b >> 3)+1;
            wt[ri][gi][bi]++; mr[ri][gi][bi]+=r; mg[ri][gi][bi]+=g; mb[ri][gi][bi]+=b;
            m2[ri][gi][bi] += (float)(sq[r]+sq[g]+sq[b]);
        }
    }
    void M3d() {
        std::array<long long,Q> a1{},a2{},a3{},a4{}; std::array<float,Q> a5{};
        for (int i = 1; i < Q; ++i) {
            std::fill(a1.begin(),a1.end(),0); std::fill(a2.begin(),a2.end(),0);
            std::fill(a3.begin(),a3.end(),0); std::fill(a4.begin(),a4.end(),0);
            std::fill(a5.begin(),a5.end(),0);
            for (int j = 1; j < Q; ++j) {
                long long sw=0,sr=0,sg=0,sb=0; float s2=0;
                for (int k = 1; k < Q; ++k) {
                    sw+=wt[i][j][k]; sr+=mr[i][j][k]; sg+=mg[i][j][k]; sb+=mb[i][j][k]; s2+=m2[i][j][k];
                    a1[k]+=sw; a2[k]+=sr; a3[k]+=sg; a4[k]+=sb; a5[k]+=s2;
                    wt[i][j][k]=wt[i-1][j][k]+a1[k]; mr[i][j][k]=mr[i-1][j][k]+a2[k];
                    mg[i][j][k]=mg[i-1][j][k]+a3[k]; mb[i][j][k]=mb[i-1][j][k]+a4[k];
                    m2[i][j][k]=m2[i-1][j][k]+a5[k];
                }
            }
        }
    }
    long long Vol(const Box& b, const L3& m) const {
        return m[b.r1][b.g1][b.b1] - m[b.r1][b.g1][b.b0] - m[b.r1][b.g0][b.b1] + m[b.r1][b.g0][b.b0]
             - m[b.r0][b.g1][b.b1] + m[b.r0][b.g1][b.b0] + m[b.r0][b.g0][b.b1] - m[b.r0][b.g0][b.b0];
    }
    long long Bot(const Box& b, int axis, const L3& m) const {
        switch (axis) {
        case 2: return -m[b.r0][b.g1][b.b1]+m[b.r0][b.g1][b.b0]+m[b.r0][b.g0][b.b1]-m[b.r0][b.g0][b.b0];
        case 1: return -m[b.r1][b.g0][b.b1]+m[b.r1][b.g0][b.b0]+m[b.r0][b.g0][b.b1]-m[b.r0][b.g0][b.b0];
        case 0: return -m[b.r1][b.g1][b.b0]+m[b.r1][b.g0][b.b0]+m[b.r0][b.g1][b.b0]-m[b.r0][b.g0][b.b0];
        }
        return 0;
    }
    long long Top(const Box& b, int axis, int p, const L3& m) const {
        switch (axis) {
        case 2: return m[p][b.g1][b.b1]-m[p][b.g1][b.b0]-m[p][b.g0][b.b1]+m[p][b.g0][b.b0];
        case 1: return m[b.r1][p][b.b1]-m[b.r1][p][b.b0]-m[b.r0][p][b.b1]+m[b.r0][p][b.b0];
        case 0: return m[b.r1][b.g1][p]-m[b.r1][b.g0][p]-m[b.r0][b.g1][p]+m[b.r0][b.g0][p];
        }
        return 0;
    }
    float Var(const Box& b) const {
        float fr=(float)Vol(b,mr), fg=(float)Vol(b,mg), fb=(float)Vol(b,mb);
        float f4=m2[b.r1][b.g1][b.b1]-m2[b.r1][b.g1][b.b0]-m2[b.r1][b.g0][b.b1]+m2[b.r1][b.g0][b.b0]
                -m2[b.r0][b.g1][b.b1]+m2[b.r0][b.g1][b.b0]+m2[b.r0][b.g0][b.b1]-m2[b.r0][b.g0][b.b0];
        long long w = Vol(b, wt);
        if (w == 0) return 0;
        return f4 - (fr*fr + fg*fg + fb*fb) / (float)w;
    }
    float Maximize(const Box& b, int axis, int lo, int hi, int& cut,
                   long long wr, long long wg, long long wb, long long ww) const {
        long long br = Bot(b,axis,mr), bg = Bot(b,axis,mg), bb = Bot(b,axis,mb), bw = Bot(b,axis,wt);
        float mx = 0; cut = -1;
        for (int i = lo; i < hi; ++i) {
            long long hr = br + Top(b,axis,i,mr);
            long long hg = bg + Top(b,axis,i,mg);
            long long hb = bb + Top(b,axis,i,mb);
            long long hw = bw + Top(b,axis,i,wt);
            if (hw == 0) continue;
            float t = (float)(hr*hr+hg*hg+hb*hb) / (float)hw;
            long long ohr=wr-hr, ohg=wg-hg, ohb=wb-hb, ohw=ww-hw;
            if (ohw == 0) continue;
            t += (float)(ohr*ohr+ohg*ohg+ohb*ohb) / (float)ohw;
            if (t > mx) { mx = t; cut = i; }
        }
        return mx;
    }
    bool Cut(Box& a, Box& bb) {
        int rc=-1,gc=-1,bc=-1;
        long long wr=Vol(a,mr), wg=Vol(a,mg), wb=Vol(a,mb), ww=Vol(a,wt);
        float vr=Maximize(a,2,a.r0+1,a.r1,rc,wr,wg,wb,ww);
        float vg=Maximize(a,1,a.g0+1,a.g1,gc,wr,wg,wb,ww);
        float vbx=Maximize(a,0,a.b0+1,a.b1,bc,wr,wg,wb,ww);
        int axis;
        if (vr>=vg && vr>=vbx) { axis=2; if (rc<0) return false; }
        else axis = (vg>=vr && vg>=vbx) ? 1 : 0;
        bb.r1=a.r1; bb.g1=a.g1; bb.b1=a.b1;
        switch (axis) {
        case 2: bb.r0=a.r1=rc; bb.g0=a.g0; bb.b0=a.b0; break;
        case 1: bb.g0=a.g1=gc; bb.r0=a.r0; bb.b0=a.b0; break;
        case 0: bb.b0=a.b1=bc; bb.r0=a.r0; bb.g0=a.g0; break;
        }
        a.vol  = (a.r1-a.r0)*(a.g1-a.g0)*(a.b1-a.b0);
        bb.vol = (bb.r1-bb.r0)*(bb.g1-bb.g0)*(bb.b1-bb.b0);
        return true;
    }
    std::vector<std::uint32_t> pixels;
    int N, K;
    L3 wt, mr, mg, mb;
    F3 m2;
};
}  // namespace wu

// ---------- twin-aware refinement (sRGB-DAC space) ----------
static void halve_sRGB(C3& dst, C3 src) {
    dst.r = src.r * 0.5f;
    dst.g = src.g * 0.5f;
    dst.b = src.b * 0.5f;
}

static void refine_pair_aware(std::vector<C3>& base32_lin,
                               const std::vector<C3>& pixels_lin,
                               int max_iters = 8) {
    std::vector<C3> pix_s(pixels_lin.size());
    for (std::size_t i = 0; i < pixels_lin.size(); ++i) {
        pix_s[i] = {std::clamp(linear_to_srgb(pixels_lin[i].r), 0.0f, 1.0f),
                    std::clamp(linear_to_srgb(pixels_lin[i].g), 0.0f, 1.0f),
                    std::clamp(linear_to_srgb(pixels_lin[i].b), 0.0f, 1.0f)};
    }
    for (int it = 0; it < max_iters; ++it) {
        std::array<C3, 64> full;
        std::array<OKLab, 64> oklab64;
        for (int k = 0; k < 32; ++k) {
            full[k] = base32_lin[k];
            C3 hb_s; halve_sRGB(hb_s, {std::clamp(linear_to_srgb(base32_lin[k].r),0.f,1.f),
                                       std::clamp(linear_to_srgb(base32_lin[k].g),0.f,1.f),
                                       std::clamp(linear_to_srgb(base32_lin[k].b),0.f,1.f)});
            full[32 + k] = {srgb_to_linear(hb_s.r), srgb_to_linear(hb_s.g), srgb_to_linear(hb_s.b)};
        }
        for (int k = 0; k < 64; ++k) oklab64[k] = linear_to_oklab(full[k]);
        std::array<C3, 32> sumB{}, sumH{};
        std::array<int, 32> cntB{}, cntH{};
        for (std::size_t i = 0; i < pixels_lin.size(); ++i) {
            OKLab t = linear_to_oklab(pixels_lin[i]);
            int best = 0; float bestd = std::numeric_limits<float>::infinity();
            for (int k = 0; k < 64; ++k) {
                float dL=t.L-oklab64[k].L, da=t.a-oklab64[k].a, db=t.b-oklab64[k].b;
                float d=dL*dL+da*da+db*db;
                if (d < bestd) { bestd = d; best = k; }
            }
            const C3& s = pix_s[i];
            if (best < 32) { sumB[best].r+=s.r; sumB[best].g+=s.g; sumB[best].b+=s.b; cntB[best]++; }
            else { int j=best-32; sumH[j].r+=s.r; sumH[j].g+=s.g; sumH[j].b+=s.b; cntH[j]++; }
        }
        for (int k = 0; k < 32; ++k) {
            int den = 4*cntB[k] + cntH[k];
            if (den == 0) continue;
            float fd = (float)den;
            C3 nbs{(4.0f*sumB[k].r + 2.0f*sumH[k].r)/fd,
                   (4.0f*sumB[k].g + 2.0f*sumH[k].g)/fd,
                   (4.0f*sumB[k].b + 2.0f*sumH[k].b)/fd};
            nbs.r = std::clamp(nbs.r, 0.0f, 1.0f);
            nbs.g = std::clamp(nbs.g, 0.0f, 1.0f);
            nbs.b = std::clamp(nbs.b, 0.0f, 1.0f);
            base32_lin[k] = {srgb_to_linear(nbs.r), srgb_to_linear(nbs.g), srgb_to_linear(nbs.b)};
        }
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s in.png out.png\n", argv[0]); return 1; }
    int W, H, ch;
    unsigned char* data = stbi_load(argv[1], &W, &H, &ch, 4);
    if (!data) return 1;
    std::vector<std::uint32_t> argb(W*H);
    std::vector<C3> pix_lin(W*H);
    for (int i = 0; i < W*H; ++i) {
        unsigned char r=data[4*i+0], g=data[4*i+1], b=data[4*i+2], a=data[4*i+3];
        argb[i] = (std::uint32_t(a)<<24)|(std::uint32_t(r)<<16)|(std::uint32_t(g)<<8)|b;
        pix_lin[i] = {srgb_to_linear(r/255.0f), srgb_to_linear(g/255.0f), srgb_to_linear(b/255.0f)};
    }
    stbi_image_free(data);

    // 1. Wu → 32 base.
    wu::Wu q(argb, 32);
    std::vector<std::uint32_t> wu_pal;
    q.quantize(wu_pal);
    std::vector<C3> base32(32);
    for (int i = 0; i < 32; ++i) {
        std::uint32_t c = wu_pal[i];
        base32[i] = {srgb_to_linear(((c>>16)&0xFF)/255.0f),
                     srgb_to_linear(((c>>8)&0xFF)/255.0f),
                     srgb_to_linear((c&0xFF)/255.0f)};
    }

    // 2. Twin-aware refine.
    refine_pair_aware(base32, pix_lin);

    // 2b. Snap base palette to OCS 12-bit nibbles (per-channel rounding).
    // ham_convert's CMAP is 4-bit-per-channel nibbles too — so this is
    // the Amiga-displayable comparison.
    for (auto& c : base32) {
        auto snap = [](float lin) {
            float s = std::clamp(linear_to_srgb(lin), 0.0f, 1.0f);
            int nib = std::clamp((int)std::round(s * 15.0f), 0, 15);
            int v8 = (nib << 4) | nib;
            return srgb_to_linear(v8 / 255.0f);
        };
        c.r = snap(c.r); c.g = snap(c.g); c.b = snap(c.b);
    }

    // 3. Build 64-effective.
    std::vector<C3> full64(64);
    for (int k = 0; k < 32; ++k) {
        full64[k] = base32[k];
        C3 s{std::clamp(linear_to_srgb(base32[k].r),0.f,1.f),
             std::clamp(linear_to_srgb(base32[k].g),0.f,1.f),
             std::clamp(linear_to_srgb(base32[k].b),0.f,1.f)};
        s.r *= 0.5f; s.g *= 0.5f; s.b *= 0.5f;
        full64[32+k] = {srgb_to_linear(s.r), srgb_to_linear(s.g), srgb_to_linear(s.b)};
    }
    std::vector<OKLab> pal_lab(64);
    for (int k = 0; k < 64; ++k) pal_lab[k] = linear_to_oklab(full64[k]);

    // 4. FS dither, serpentine, OKLab nearest.
    std::vector<OKLab> err_buf(W*H, OKLab{0,0,0});
    std::vector<std::uint8_t> idx(W*H);
    for (int y = 0; y < H; ++y) {
        bool rev = (y & 1);
        for (int step = 0; step < W; ++step) {
            int x = rev ? (W - 1 - step) : step;
            OKLab src = linear_to_oklab(pix_lin[y*W+x]);
            OKLab tgt = src;
            auto& e = err_buf[y*W+x];
            tgt.L += std::clamp(e.L, -0.35f, 0.35f);
            tgt.a += std::clamp(e.a, -0.35f, 0.35f);
            tgt.b += std::clamp(e.b, -0.35f, 0.35f);
            int best = 0; float bestd = std::numeric_limits<float>::infinity();
            for (int k = 0; k < 64; ++k) {
                float dL=tgt.L-pal_lab[k].L, da=tgt.a-pal_lab[k].a, db=tgt.b-pal_lab[k].b;
                float d=dL*dL+da*da+db*db;
                if (d < bestd) { bestd = d; best = k; }
            }
            idx[y*W+x] = static_cast<std::uint8_t>(best);
            OKLab qe{(tgt.L - pal_lab[best].L) * 0.85f,
                     (tgt.a - pal_lab[best].a) * 0.85f,
                     (tgt.b - pal_lab[best].b) * 0.85f};
            // FS kernel: (+1,0)=7/16, (-1,+1)=3/16, (0,+1)=5/16, (+1,+1)=1/16
            // serpentine flips x sign
            auto add = [&](int nx, int ny, float w) {
                if (nx<0||nx>=W||ny<0||ny>=H) return;
                err_buf[ny*W+nx].L += qe.L*w;
                err_buf[ny*W+nx].a += qe.a*w;
                err_buf[ny*W+nx].b += qe.b*w;
            };
            int dx = rev ? -1 : 1;
            add(x+dx,    y,   7.0f/16.0f);
            add(x-dx,   y+1,  3.0f/16.0f);
            add(x,      y+1,  5.0f/16.0f);
            add(x+dx,   y+1,  1.0f/16.0f);
        }
    }

    // 5. Render to RGB output.
    std::vector<unsigned char> out(W*H*3);
    for (int i = 0; i < W*H; ++i) {
        const C3& c = full64[idx[i]];
        out[3*i+0] = (unsigned char)std::clamp(linear_to_srgb(c.r)*255.0f + 0.5f, 0.0f, 255.0f);
        out[3*i+1] = (unsigned char)std::clamp(linear_to_srgb(c.g)*255.0f + 0.5f, 0.0f, 255.0f);
        out[3*i+2] = (unsigned char)std::clamp(linear_to_srgb(c.b)*255.0f + 0.5f, 0.0f, 255.0f);
    }
    stbi_write_png(argv[2], W, H, 3, out.data(), W*3);
    std::fprintf(stderr, "wrote %s (%dx%d)\n", argv[2], W, H);
    return 0;
}
