#include "hybrid.hpp"
#include "ffnn.hpp"   // kInputDim
#include "models.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <vector>

namespace spear::std_t98::secret {

struct Hybrid::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "spear.std_t98.secret"};
    std::unique_ptr<Ort::Session> session;
    std::string in_name, out_name;
};

Hybrid::Hybrid() : impl_(std::make_unique<Impl>()) {}
Hybrid::~Hybrid() = default;
bool Hybrid::loaded() const { return impl_->session != nullptr; }
bool Hybrid::load_embedded(std::string* err) { return load(embedded_hybrid_onnx(), err); }

bool Hybrid::load(std::span<const uint8_t> bytes, std::string* err) {
    try {
        Ort::SessionOptions so;
        // 探索は専用ワーカースレッド 1 本の仕事。ORT にスレッドプールを作らせない(DSP thread から CPU を奪わない、
        // TSan から見えない同期を持ち込まない)。hybrid は候補数百件までなので単一スレッドで十分
        so.SetIntraOpNumThreads(1);
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, bytes.data(), bytes.size(), so);
        Ort::AllocatorWithDefaultOptions alloc;
        impl_->in_name = impl_->session->GetInputNameAllocated(0, alloc).get();
        impl_->out_name = impl_->session->GetOutputNameAllocated(0, alloc).get();
        const auto shape = impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 2 || shape[1] != kInputDim) { if (err) *err = "hybrid: unexpected input shape"; impl_->session.reset(); return false; }
        return true;
    } catch (const Ort::Exception& e) {
        if (err) *err = std::string("onnxruntime: ") + e.what();
        impl_->session.reset();
        return false;
    }
}

bool Hybrid::logits(const float* x, std::size_t n, float* out, std::string* err) const {
    if (!impl_->session) { if (err) *err = "hybrid model not loaded"; return false; }
    if (n == 0) return true;
    try {
        std::vector<float> in(x, x + n * kInputDim);
        const int64_t shape[2] = {static_cast<int64_t>(n), kInputDim};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 2);
        const char* in_names[] = {impl_->in_name.c_str()};
        const char* out_names[] = {impl_->out_name.c_str()};
        auto outs = impl_->session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
        const auto oshape = outs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (oshape.size() != 2 || oshape[0] != static_cast<int64_t>(n) || oshape[1] != 2) { if (err) *err = "hybrid: unexpected output shape"; return false; }
        const float* p = outs[0].GetTensorData<float>();
        std::copy(p, p + n * 2, out);
        return true;
    } catch (const Ort::Exception& e) {
        if (err) *err = std::string("onnxruntime: ") + e.what();
        return false;
    }
}

} // namespace spear::std_t98::secret
