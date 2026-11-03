#include "tokuder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

extern "C" {
extern const uint8_t spear_std_t99_tokuder_data[];
extern const uint8_t spear_std_t99_tokuder_end[];
}

namespace spear::std_t99::tokuder {
namespace {

// ---------------------------------------------------------------- FEC (tokuder_neurodec/fec.py)
constexpr const char* kH[12] = {
    "100000010001011100100100", "010000010001010001001110", "001000010001001000011101",
    "000100010001000101111000", "000010010000011000111010", "000001010000010101100011",
    "000000110000001101010110", "000000001001011001101001", "000000000101010101010101",
    "000000000011001100110011", "000000000000111100001111", "000000000000000011111111",
};
constexpr int kInfo[12] = {7, 11, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23};

struct Codebook {
    std::array<std::array<uint8_t, 24>, 4096> words{};
    std::array<std::array<float, 24>, 4096> sign{};
    Codebook() {
        int pivots[12], np = 0;
        for (int c = 0; c < 24; ++c) if (std::find(std::begin(kInfo), std::end(kInfo), c) == std::end(kInfo)) pivots[np++] = c;
        for (int k = 0; k < 4096; ++k) {
            auto& w = words[static_cast<std::size_t>(k)];
            for (int i = 0; i < 12; ++i) w[static_cast<std::size_t>(kInfo[i])] = static_cast<uint8_t>((k >> (11 - i)) & 1);
            for (int r = 0; r < 12; ++r) {   // H is in RREF with the identity on the pivot columns
                int s = 0;
                for (int i = 0; i < 12; ++i) s ^= (kH[r][kInfo[i]] - '0') & w[static_cast<std::size_t>(kInfo[i])];
                w[static_cast<std::size_t>(pivots[r])] = static_cast<uint8_t>(s);
            }
            for (int i = 0; i < 24; ++i) sign[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = w[static_cast<std::size_t>(i)] ? 1.f : -1.f;
        }
    }
};
const Codebook& codebook() { static const Codebook c; return c; }

// ---------------------------------------------------------------- numerics
inline float gelu(float x) { return 0.5f * x * (1.f + std::erf(x * 0.70710678118654752f)); }
inline float sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

// Matrix-vector products, y (+)= W x, W row-major [out][in]. AVX2/FMA is chosen at run time
// (Windows has no ifunc for target_clones); the file is built with -ffast-math so both loops vectorize.
template <bool add>
inline void matvec_generic(const float* w, const float* x, float* y, int out, int in) {
    for (int o = 0; o < out; ++o) {
        const float* row = w + static_cast<std::size_t>(o) * static_cast<std::size_t>(in);
        float s = 0.f;
        for (int i = 0; i < in; ++i) s += row[i] * x[i];
        y[o] = add ? y[o] + s : s;
    }
}
template <bool add>
__attribute__((target("avx2,fma"))) void matvec_avx2(const float* w, const float* x, float* y, int out, int in) {
    for (int o = 0; o < out; ++o) {
        const float* row = w + static_cast<std::size_t>(o) * static_cast<std::size_t>(in);
        float s = 0.f;
        for (int i = 0; i < in; ++i) s += row[i] * x[i];
        y[o] = add ? y[o] + s : s;
    }
}
const bool kAvx2 = [] { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"); }();
void linear(const float* w, const float* b, const float* x, float* y, int out, int in) {
    if (kAvx2) matvec_avx2<false>(w, x, y, out, in); else matvec_generic<false>(w, x, y, out, in);
    if (b) for (int o = 0; o < out; ++o) y[o] += b[o];
}
void accumulate(const float* w, const float* x, float* y, int out, int in) {
    if (kAvx2) matvec_avx2<true>(w, x, y, out, in); else matvec_generic<true>(w, x, y, out, in);
}
void layer_norm(const float* x, const float* g, const float* b, float* y, int n) {
    double mean = 0;
    for (int i = 0; i < n; ++i) mean += x[i];
    mean /= n;
    double var = 0;
    for (int i = 0; i < n; ++i) { const double d = x[i] - mean; var += d * d; }
    var /= n;
    const float inv = static_cast<float>(1.0 / std::sqrt(var + 1e-6));
    for (int i = 0; i < n; ++i) y[i] = (x[i] - static_cast<float>(mean)) * inv * g[i] + b[i];
}

// History of the last `taps - 1` vectors before the current one (zeros before the stream starts)
class History {
    int width_ = 0, taps_ = 0;
    std::vector<float> data_;   // [taps - 1][width], oldest first
public:
    History() = default;
    History(int width, int taps) : width_(width), taps_(taps), data_(static_cast<std::size_t>(width) * static_cast<std::size_t>(taps - 1)) {}
    const float* at(int k) const { return data_.data() + static_cast<std::size_t>(k) * static_cast<std::size_t>(width_); }   // k = 0: t - (taps - 1)
    void push(const float* x) {
        if (taps_ <= 1) return;
        std::memmove(data_.data(), data_.data() + width_, sizeof(float) * static_cast<std::size_t>(width_) * static_cast<std::size_t>(taps_ - 2));
        std::memcpy(data_.data() + static_cast<std::size_t>(width_) * static_cast<std::size_t>(taps_ - 2), x, sizeof(float) * static_cast<std::size_t>(width_));
    }
    void clear() { std::fill(data_.begin(), data_.end(), 0.f); }
};

// Causal Conv1d [out][in][k] stored per tap as [k][out][in]
struct CausalConv {
    int out = 0, in = 0, k = 0;
    std::vector<float> taps;
    const float* bias = nullptr;
    History hist;
    CausalConv() = default;
    CausalConv(const Weights& w, const std::string& name, int o, int i, int kk) : out(o), in(i), k(kk), taps(static_cast<std::size_t>(o * i * kk)), hist(i, kk) {
        const float* src = w.get(name + ".weight", {o, i, kk});
        bias = w.get(name + ".bias", {o});
        for (int t = 0; t < k; ++t)
            for (int a = 0; a < o; ++a)
                for (int b = 0; b < i; ++b) taps[(static_cast<std::size_t>(t) * o + a) * i + b] = src[(static_cast<std::size_t>(a) * i + b) * k + t];
    }
    void step(const float* x, float* y) {
        std::copy(bias, bias + out, y);
        for (int t = 0; t < k - 1; ++t) accumulate(taps.data() + static_cast<std::size_t>(t) * out * in, hist.at(t), y, out, in);
        accumulate(taps.data() + static_cast<std::size_t>(k - 1) * out * in, x, y, out, in);
        hist.push(x);
    }
};

// istft.SpectralBlock with a causal depthwise kernel 7
struct Block {
    int width = 0, hidden = 0;
    const float *tw, *tb, *ng, *nb, *ew, *eb, *pw, *pb, *scale;
    History hist;
    std::vector<float> t, n, e, p;
    Block(const Weights& w, const std::string& name, int width_, int expansion) : width(width_), hidden(width_ * expansion), hist(width_, 7),
        t(static_cast<std::size_t>(width_)), n(static_cast<std::size_t>(width_)), e(static_cast<std::size_t>(hidden)), p(static_cast<std::size_t>(width_)) {
        tw = w.get(name + ".temporal.weight", {width, 1, 7});
        tb = w.get(name + ".temporal.bias", {width});
        ng = w.get(name + ".norm.weight", {width});
        nb = w.get(name + ".norm.bias", {width});
        ew = w.get(name + ".expand.weight", {hidden, width});
        eb = w.get(name + ".expand.bias", {hidden});
        pw = w.get(name + ".project.weight", {width, hidden});
        pb = w.get(name + ".project.bias", {width});
        scale = w.get(name + ".scale", {width});
    }
    void step(const float* x, float* y) {
        for (int c = 0; c < width; ++c) {
            float s = tb[c];
            for (int k = 0; k < 6; ++k) s += tw[c * 7 + k] * hist.at(k)[c];
            t[static_cast<std::size_t>(c)] = s + tw[c * 7 + 6] * x[c];
        }
        hist.push(x);
        layer_norm(t.data(), ng, nb, n.data(), width);
        linear(ew, eb, n.data(), e.data(), hidden, width);
        for (auto& v : e) v = gelu(v);
        linear(pw, pb, e.data(), p.data(), width, hidden);
        for (int c = 0; c < width; ++c) y[c] = x[c] + p[static_cast<std::size_t>(c)] * scale[c];
    }
    void clear() { hist.clear(); }
};

// Radix-2 complex FFT, n = 512
class Fft {
    static constexpr int n = 512;
    std::array<std::complex<float>, n / 2> twiddle_{};
    std::array<int, n> reverse_{};
public:
    Fft() {
        for (int i = 0; i < n / 2; ++i) twiddle_[static_cast<std::size_t>(i)] = std::polar(1.f, static_cast<float>(-2 * std::numbers::pi * i / n));
        for (int i = 0, j = 0; i < n; ++i) {
            reverse_[static_cast<std::size_t>(i)] = j;
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
        }
    }
    void run(std::complex<float>* x, bool inverse) const {
        for (int i = 0; i < n; ++i) if (i < reverse_[static_cast<std::size_t>(i)]) std::swap(x[i], x[reverse_[static_cast<std::size_t>(i)]]);
        for (int len = 2; len <= n; len <<= 1) {
            const int stride = n / len;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < len / 2; ++k) {
                    auto w = twiddle_[static_cast<std::size_t>(k * stride)];
                    if (inverse) w = std::conj(w);
                    const auto u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                }
        }
    }
};
const Fft& fft() { static const Fft f; return f; }

constexpr int kNfft = 512, kHop = 80, kBins = 257, kPadLeft = kNfft - kHop;   // causal excitation STFT
constexpr int kFrontWidth = 256, kMel = 80, kHead = 7, kCond = 56, kWidth = 192, kExc = 3 * kBins, kBands = 5, kHarmonics = 40;

const std::array<float, kNfft>& window() {
    static const auto w = [] {
        std::array<float, kNfft> a{};
        for (int i = 0; i < kNfft; ++i) a[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / kNfft));   // periodic Hann
        return a;
    }();
    return w;
}
} // namespace

int soft_decode(const int8_t* llr, uint8_t* info) {
    const auto& cb = codebook();
    int corrected = 0;
    for (int word = 0; word < 2; ++word) {
        float x[24];
        for (int i = 0; i < 24; ++i) x[i] = llr[3 * i + word];
        int best = 0;
        float best_score = -1e30f;
        for (int k = 0; k < 4096; ++k) {
            const auto& s = cb.sign[static_cast<std::size_t>(k)];
            float score = 0;
            for (int i = 0; i < 24; ++i) score += x[i] * s[static_cast<std::size_t>(i)];
            if (score > best_score) { best_score = score; best = k; }
        }
        const auto& w = cb.words[static_cast<std::size_t>(best)];
        for (int i = 0; i < 24; ++i) corrected += w[static_cast<std::size_t>(i)] != (x[i] > 0);
        for (int i = 0; i < 12; ++i) info[12 * word + i] = w[static_cast<std::size_t>(kInfo[i])];
    }
    for (int i = 0; i < 24; ++i) info[24 + i] = llr[3 * i + 2] > 0;
    return corrected;
}

// ---------------------------------------------------------------- weights
Weights::Weights(const uint8_t* blob, std::size_t size) {
    auto need = [&](std::size_t at, std::size_t n) { if (at + n > size) throw std::runtime_error("TOKUDER model is truncated"); };
    need(0, 8);
    if (std::memcmp(blob, "TKD1", 4) != 0) throw std::runtime_error("TOKUDER model has an unknown format");
    uint32_t count;
    std::memcpy(&count, blob + 4, 4);
    std::size_t at = 8;
    struct Raw { std::string name; std::vector<int> shape; uint64_t offset; };
    std::vector<Raw> raws;
    uint64_t total = 0;
    for (uint32_t e = 0; e < count; ++e) {
        need(at, 2);
        uint16_t len;
        std::memcpy(&len, blob + at, 2);
        at += 2;
        need(at, len + 1u);
        Raw r;
        r.name.assign(reinterpret_cast<const char*>(blob + at), len);
        at += len;
        const int ndim = blob[at++];
        need(at, static_cast<std::size_t>(ndim) * 4 + 8);
        uint64_t n = 1;
        for (int d = 0; d < ndim; ++d) {
            int32_t v;
            std::memcpy(&v, blob + at, 4);
            at += 4;
            r.shape.push_back(v);
            n *= static_cast<uint64_t>(v);
        }
        std::memcpy(&r.offset, blob + at, 8);
        at += 8;
        total = std::max(total, r.offset + n);
        raws.push_back(std::move(r));
    }
    need(at, total * 4);
    storage_.resize(total);
    std::memcpy(storage_.data(), blob + at, total * 4);
    for (auto& r : raws) entries_[r.name] = {r.shape, storage_.data() + r.offset};
}

const float* Weights::get(const std::string& name, std::initializer_list<int> shape) const {
    const auto it = entries_.find(name);
    if (it == entries_.end() || !std::equal(it->second.shape.begin(), it->second.shape.end(), shape.begin(), shape.end()))
        throw std::runtime_error("TOKUDER model: missing or unexpected tensor " + name);
    return it->second.data;
}

const Weights& model() {
    static const Weights w(spear_std_t99_tokuder_data, static_cast<std::size_t>(spear_std_t99_tokuder_end - spear_std_t99_tokuder_data));
    return w;
}

// ---------------------------------------------------------------- decoder
struct Decoder::Impl {
    // frontend
    const float *read0w, *read0b, *read2w, *read2b, *burst, *fng, *fnb, *auxw, *auxb, *f0w, *f0b;
    std::vector<Block> fblocks;
    // bridge, upsampler, generator
    CausalConv bridge, input;
    const float *upw, *upb, *excw, *excb, *ing, *inb, *ong, *onb, *specw, *specb;
    std::vector<Block> gblocks;
    // state
    std::vector<float> h, last_h, feat, head, mel, cond, cond1, cond2, up, exc, x, y, spec;
    bool have_last = false;
    int conds = 0;                       // condition frames seen (for the upsampler)
    double phase = 0;
    float previous_f0 = 0;
    bool have_f0 = false;
    std::vector<float> periodic, noise;  // kPadLeft history + one frame
    std::vector<float> last_up, last_exc;
    // iSTFT overlap-add, padded coordinates (output sample n = padded n + 256)
    std::vector<float> ola, envelope;
    uint64_t ola_base = 0, steps = 0, emitted = 0;
    std::array<std::complex<float>, kNfft> buffer{};

    explicit Impl(const Weights& w)
        : bridge(w, "bridge", kCond, kMel, 5), input(w, "generator.input", kWidth, kCond, 7) {
        read0w = w.get("frontend.read.0.weight", {1024, 48});
        read0b = w.get("frontend.read.0.bias", {1024});
        read2w = w.get("frontend.read.2.weight", {kFrontWidth, 1024});
        read2b = w.get("frontend.read.2.bias", {kFrontWidth});
        burst = w.get("frontend.burst", {4, kFrontWidth});
        for (int i = 0; i < 6; ++i) fblocks.emplace_back(w, "frontend.blocks." + std::to_string(i), kFrontWidth, 3);
        fng = w.get("frontend.norm.weight", {kFrontWidth});
        fnb = w.get("frontend.norm.bias", {kFrontWidth});
        auxw = w.get("frontend.aux.weight", {kMel, kFrontWidth});
        auxb = w.get("frontend.aux.bias", {kMel});
        f0w = w.get("frontend.f0.weight", {kHead, kFrontWidth});
        f0b = w.get("frontend.f0.bias", {kHead});
        upw = w.get("generator.upsampler.weight", {kCond, 1, 6});
        upb = w.get("generator.upsampler.bias", {kCond});
        excw = w.get("generator.exc_proj.weight", {kWidth, kExc, 1});
        excb = w.get("generator.exc_proj.bias", {kWidth});
        ing = w.get("generator.input_norm.weight", {kWidth});
        inb = w.get("generator.input_norm.bias", {kWidth});
        for (int i = 0; i < 6; ++i) gblocks.emplace_back(w, "generator.blocks." + std::to_string(i), kWidth, 3);
        ong = w.get("generator.output_norm.weight", {kWidth});
        onb = w.get("generator.output_norm.bias", {kWidth});
        specw = w.get("generator.spectrum.weight", {2 * kBins, kWidth});
        specb = w.get("generator.spectrum.bias", {2 * kBins});
        h.resize(kFrontWidth); last_h.resize(kFrontWidth); feat.resize(kFrontWidth);
        head.resize(kHead); mel.resize(kMel); cond.resize(kCond); cond1.resize(kCond); cond2.resize(kCond); up.resize(kCond);
        exc.resize(kExc); x.resize(kWidth); y.resize(kWidth); spec.resize(2 * kBins);
        last_up.resize(kCond); last_exc.resize(kExc);
        reset();
    }
    void reset() {
        for (auto& b : fblocks) b.clear();
        for (auto& b : gblocks) b.clear();
        bridge.hist.clear();
        input.hist.clear();
        have_last = false;
        conds = 0;
        phase = 0;
        have_f0 = false;
        periodic.assign(kPadLeft, 0.f);
        noise.assign(kPadLeft, 0.f);
        ola.clear();
        envelope.clear();
        ola_base = steps = emitted = 0;
    }
};

Decoder::Decoder(const Weights& w) : p_(std::make_unique<Impl>(w)) {}
Decoder::~Decoder() = default;

void Decoder::reset() {
    p_->reset();
    pushed_ = 0;
}

float Decoder::noise() {
    if (noise_) return noise_used_ < noise_count_ ? noise_[noise_used_++] : 0.f;
    return gauss_(rng_);
}

void Decoder::push(const uint8_t* info, std::vector<float>& pcm) {
    auto& p = *p_;
    float in[kInfoBits], hidden[1024];
    for (int i = 0; i < kInfoBits; ++i) in[i] = info[i] ? 1.f : -1.f;
    linear(p.read0w, p.read0b, in, hidden, 1024, kInfoBits);
    for (auto& v : hidden) v = gelu(v);
    linear(p.read2w, p.read2b, hidden, p.h.data(), kFrontWidth, 1024);
    const float* burst = p.burst + (pushed_ % 4) * kFrontWidth;
    for (int c = 0; c < kFrontWidth; ++c) p.h[static_cast<std::size_t>(c)] += burst[c];
    for (auto& b : p.fblocks) { b.step(p.h.data(), p.feat.data()); std::swap(p.h, p.feat); }
    // one frame look-ahead: frame t is read from position t + 1
    if (p.have_last) {
        layer_norm(p.h.data(), p.fng, p.fnb, p.feat.data(), kFrontWidth);
        emit(p.feat.data(), pcm);
    }
    p.last_h = p.h;
    p.have_last = true;
    ++pushed_;
}

void Decoder::flush(std::vector<float>& pcm) {
    auto& p = *p_;
    if (pushed_ > 0) {
        // the last frame has no future: it repeats its own position (frontends.py)
        layer_norm(p.last_h.data(), p.fng, p.fnb, p.feat.data(), kFrontWidth);
        emit(p.feat.data(), pcm);
        // replicate padding of one generator step at the end (istft.py ISTFTGenerator.forward)
        step(p.last_up.data(), p.last_exc.data(), pcm);
        output(pcm, pushed_ * kFrameSamples);
    }
    reset();
}

void Decoder::emit(const float* features, std::vector<float>& pcm) {
    auto& p = *p_;
    linear(p.auxw, p.auxb, features, p.mel.data(), kMel, kFrontWidth);
    linear(p.f0w, p.f0b, features, p.head.data(), kHead, kFrontWidth);
    if (keep_probe) { probe.insert(probe.end(), p.mel.begin(), p.mel.end()); probe.insert(probe.end(), p.head.begin(), p.head.end()); }
    // condition = bridge(mel) (the frontend's own condition output is not used by the decoder)
    p.cond2 = p.cond1;
    p.cond1 = p.cond;
    p.bridge.step(p.mel.data(), p.cond.data());
    const int have = std::min(p.conds, 2);
    ++p.conds;
    // periodicity head: log F0, voicing (unused with band aperiodicity), aperiodicity x 5
    const float f0 = std::max(1.f, std::exp(p.head[0]));
    float ap[kBands];
    for (int b = 0; b < kBands; ++b) ap[b] = sigmoid(p.head[static_cast<std::size_t>(2 + b)]);
    // excitation.py waveform: causal ramp of F0 from the previous frame, harmonics below Nyquist
    const float from = p.have_f0 ? p.previous_f0 : f0;
    p.previous_f0 = f0;
    p.have_f0 = true;
    for (int j = 0; j < kFrameSamples; ++j) {
        const float lambda = static_cast<float>(j + 1) / kFrameSamples;
        const float fs = from + lambda * (f0 - from);
        p.phase += 2 * std::numbers::pi * static_cast<double>(fs) / 8000.0;
        if (p.phase > 2e4 * std::numbers::pi) p.phase = std::fmod(p.phase, 2 * std::numbers::pi);
        double sum = 0;
        int active = 0;
        for (int k = 1; k <= kHarmonics; ++k) {
            if (fs * static_cast<float>(k) >= 4000.f) break;
            sum += std::sin(p.phase * k);
            ++active;
        }
        p.periodic.push_back(static_cast<float>(sum / std::sqrt(std::max(active, 1))));
        p.noise.push_back(noise());
    }
    const auto& w = window();
    static const auto band = [] {
        std::array<int, kBins> b{};
        const float scale = static_cast<float>(kBands) / static_cast<float>(kBins);
        for (int i = 0; i < kBins; ++i) b[static_cast<std::size_t>(i)] = std::min(kBands - 1, static_cast<int>(std::floor(static_cast<float>(i) * scale)));
        return b;
    }();
    std::array<std::complex<float>, kNfft> P{}, N{};
    for (int r = 0; r < 2; ++r) {
        // upsampler: ConvTranspose1d(k = 6, stride 2), step 2f + r sees frames f, f-1, f-2
        for (int c = 0; c < kCond; ++c) {
            float v = p.upb[c] + p.cond[static_cast<std::size_t>(c)] * p.upw[c * 6 + r];
            if (have >= 1) v += p.cond1[static_cast<std::size_t>(c)] * p.upw[c * 6 + r + 2];
            if (have >= 2) v += p.cond2[static_cast<std::size_t>(c)] * p.upw[c * 6 + r + 4];
            p.up[static_cast<std::size_t>(c)] = v;
        }
        // excitation STFT frame 2f + r: window ending at the end of its hop
        const std::size_t start = static_cast<std::size_t>(r * kHop);
        for (int i = 0; i < kNfft; ++i) {
            P[static_cast<std::size_t>(i)] = {p.periodic[start + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.f};
            N[static_cast<std::size_t>(i)] = {p.noise[start + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.f};
        }
        fft().run(P.data(), false);
        fft().run(N.data(), false);
        for (int b = 0; b < kBins; ++b) {
            const float a = ap[band[static_cast<std::size_t>(b)]];
            const std::complex<float> s = P[static_cast<std::size_t>(b)] * (1.f - a) + N[static_cast<std::size_t>(b)] * (a * 0.5f);
            const float angle = std::atan2(s.imag(), s.real());
            p.exc[static_cast<std::size_t>(b)] = std::log(std::abs(s) + 1e-5f);
            p.exc[static_cast<std::size_t>(kBins + b)] = std::cos(angle);
            p.exc[static_cast<std::size_t>(2 * kBins + b)] = std::sin(angle);
        }
        step(p.up.data(), p.exc.data(), pcm);
    }
    p.periodic.erase(p.periodic.begin(), p.periodic.begin() + kFrameSamples);
    p.noise.erase(p.noise.begin(), p.noise.begin() + kFrameSamples);
}

void Decoder::step(const float* up, const float* exc, std::vector<float>& pcm) {
    auto& p = *p_;
    if (up != p.last_up.data()) std::copy(up, up + kCond, p.last_up.begin());
    if (exc != p.last_exc.data()) std::copy(exc, exc + kExc, p.last_exc.begin());
    p.input.step(up, p.x.data());
    accumulate(p.excw, exc, p.x.data(), kWidth, kExc);
    for (int c = 0; c < kWidth; ++c) p.x[static_cast<std::size_t>(c)] += p.excb[c];
    layer_norm(p.x.data(), p.ing, p.inb, p.y.data(), kWidth);
    for (auto& b : p.gblocks) { b.step(p.y.data(), p.x.data()); std::swap(p.x, p.y); }
    layer_norm(p.y.data(), p.ong, p.onb, p.x.data(), kWidth);
    linear(p.specw, p.specb, p.x.data(), p.spec.data(), 2 * kBins, kWidth);
    // iSTFT (n_fft 512, hop 80, Hann, centred): irfft ignores the imaginary parts of DC and Nyquist
    auto& z = p.buffer;
    for (int b = 0; b < kBins; ++b) {
        const float magnitude = std::exp(std::clamp(p.spec[static_cast<std::size_t>(b)], -12.f, 4.6f));
        const float ph = p.spec[static_cast<std::size_t>(kBins + b)];
        z[static_cast<std::size_t>(b)] = {magnitude * std::cos(ph), magnitude * std::sin(ph)};
    }
    z[0] = {z[0].real(), 0.f};
    z[kNfft / 2] = {z[kNfft / 2].real(), 0.f};
    for (int b = 1; b < kNfft / 2; ++b) z[static_cast<std::size_t>(kNfft - b)] = std::conj(z[static_cast<std::size_t>(b)]);
    fft().run(z.data(), true);
    const auto& w = window();
    const uint64_t first = p.steps * kHop;   // padded position of this frame
    const std::size_t need = static_cast<std::size_t>(first + kNfft - p.ola_base);
    if (p.ola.size() < need) { p.ola.resize(need, 0.f); p.envelope.resize(need, 0.f); }
    for (int i = 0; i < kNfft; ++i) {
        const std::size_t at = static_cast<std::size_t>(first - p.ola_base) + static_cast<std::size_t>(i);
        p.ola[at] += z[static_cast<std::size_t>(i)].real() / kNfft * w[static_cast<std::size_t>(i)];
        p.envelope[at] += w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
    }
    ++p.steps;
    // padded positions before the next frame are final
    const uint64_t final_padded = p.steps * kHop;
    if (final_padded > kNfft / 2) output(pcm, final_padded - kNfft / 2);
}

void Decoder::output(std::vector<float>& pcm, uint64_t until) {
    auto& p = *p_;
    for (; p.emitted < until; ++p.emitted) {
        const std::size_t at = static_cast<std::size_t>(p.emitted + kNfft / 2 - p.ola_base);
        const float env = p.envelope[at];
        pcm.push_back(std::clamp(env > 1e-11f ? p.ola[at] / env : 0.f, -1.f, 1.f));
    }
    // drop what is no longer needed
    const uint64_t keep = p.emitted + kNfft / 2;
    if (keep > p.ola_base + 4096) {
        const auto drop = static_cast<std::size_t>(keep - p.ola_base);
        p.ola.erase(p.ola.begin(), p.ola.begin() + static_cast<long>(drop));
        p.envelope.erase(p.envelope.begin(), p.envelope.begin() + static_cast<long>(drop));
        p.ola_base += drop;
    }
}

} // namespace spear::std_t99::tokuder
