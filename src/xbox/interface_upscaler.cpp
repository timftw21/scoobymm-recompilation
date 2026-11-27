#include "interface_upscaler.h"
#include "shaders.h"
#include "upscaler_model.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>

namespace xbox_native {
namespace {
void checked_ui(HRESULT status, const char* operation) {
    if (FAILED(status)) throw std::runtime_error(std::format("{} failed (0x{:08X})", operation, static_cast<unsigned>(status)));
}
// Mirrors the HLSL Job and Layer constant buffers in shaders.cpp.
struct JobConstants {
    std::int32_t image[2]{}, origin[2]{}, extent[2]{}, halo[2]{}, interior[2]{};
    std::uint32_t tile_slot = 0, factor = 1;
    std::int32_t target[2]{}, padding[2]{};
};
static_assert(sizeof(JobConstants) == 64);
struct LayerConstants {
    std::array<Float4, 2304> weights{};
    std::array<Float4, 4> bias{}, slope{};
    std::array<std::uint32_t, 4> layer{};
};
static_assert(sizeof(LayerConstants) == 2313 * 16);
struct BatchConstants { std::int32_t extents[interface_batch][4]{}; };
static_assert(sizeof(BatchConstants) == interface_batch * 16);
constexpr unsigned channel_groups(const UpscalerLayer& layer) { return layer.outputs / 16; }
constexpr unsigned divide(unsigned value, unsigned by) { return (value + by - 1) / by; }
}

struct InterfaceUpscaler::Job {
    enum class Stage { classify, decide, bleed, network, filter, mips, done };
    std::uint64_t id;
    Ptr<ID3D11ShaderResourceView> source;
    unsigned width, height, factor;
    Stage stage = Stage::classify;
    Ptr<ID3D11Buffer> statistics, readback;
    Ptr<ID3D11UnorderedAccessView> statistics_view;
    InterfaceDecision decision;
    std::array<Ptr<ID3D11Texture2D>, 2> bleed;
    std::array<Ptr<ID3D11ShaderResourceView>, 2> bleed_view;
    std::array<Ptr<ID3D11UnorderedAccessView>, 2> bleed_output;
    unsigned bleed_pass = 0;
    Ptr<ID3D11Texture2D> output;
    Ptr<ID3D11ShaderResourceView> view;
    Ptr<ID3D11UnorderedAccessView> output_view;
    std::size_t bytes = 0, working = 0;
    unsigned tiles = 0, next_tile = 0, composed = 0;
    ID3D11ShaderResourceView* colour() const {
        return decision.transparent ? bleed_view[(interface_bleed_passes - 1) % 2].Get() : source.Get();
    }
};

InterfaceUpscaler::InterfaceUpscaler(ID3D11Device* device, ID3D11DeviceContext* context, unsigned tile)
    : device_(device), context_(context), tile_(tile) {
    if (tile < 8 || tile > interface_tile) throw std::runtime_error("Invalid HUD upscaler tile size");
    const std::pair<ShaderKind, Ptr<ID3D11ComputeShader>*> shaders[]{
        {ShaderKind::interface_classify, &classify_}, {ShaderKind::interface_bleed, &bleed_},
        {ShaderKind::interface_conv_first, &first_}, {ShaderKind::interface_conv, &conv_},
        {ShaderKind::interface_compose, &compose_}, {ShaderKind::interface_filter, &filter_}};
    for (const auto& [kind, shader] : shaders) {
        const auto binary = shader_binary(kind);
        checked_ui(device->CreateComputeShader(binary.bytes.data(), binary.bytes.size(), nullptr, shader->ReleaseAndGetAddressOf()),
                   "HUD upscaler shader");
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(JobConstants); buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    checked_ui(device->CreateBuffer(&buffer, nullptr, &job_constants_), "HUD upscaler parameters");
    buffer.ByteWidth = sizeof(BatchConstants);
    checked_ui(device->CreateBuffer(&buffer, nullptr, &batch_constants_), "HUD upscaler batch");
    // Repack each convolution once into constant buffers of 16 outputs, so
    // every unrolled multiply reads a uniform constant.
    const auto model = read_upscaler_model(upscaler_checkpoint());
    const auto layout = upscaler_layout();
    auto constants = std::make_unique<LayerConstants>();
    for (unsigned l = 0; l < layout.size(); ++l) {
        const auto& layer = layout[l];
        layer_index_[l] = static_cast<unsigned>(layers_.size());
        for (unsigned g = 0; g < channel_groups(layer); ++g) {
            *constants = {};
            constants->layer = {g * 4, 0, 0, 0};
            for (unsigned k = 0; k < 4; ++k) for (unsigned m = 0; m < 4; ++m) {
                const auto o = g * 16 + k * 4 + m;
                constants->bias[k][m] = model[layer.bias + o];
                constants->slope[k][m] = layer.slope == SIZE_MAX ? 1.0f : model[layer.slope + o];
                for (unsigned c = 0; c < layer.inputs; ++c) for (unsigned t = 0; t < 9; ++t)
                    constants->weights[(t * layer.inputs + c) * 4 + k][m] = model[layer.weights + (std::size_t(o) * layer.inputs + c) * 9 + t];
            }
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(LayerConstants); desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const D3D11_SUBRESOURCE_DATA data{constants.get(), 0, 0};
            checked_ui(device->CreateBuffer(&desc, &data, &layers_.emplace_back()), "HUD upscaler weights");
        }
    }
    layer_index_.back() = static_cast<unsigned>(layers_.size());
    for (auto& timing : timings_) {
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        checked_ui(device->CreateQuery(&query, &timing.disjoint), "HUD upscaler timing");
        query.Query = D3D11_QUERY_TIMESTAMP;
        checked_ui(device->CreateQuery(&query, &timing.start), "HUD upscaler timing");
        checked_ui(device->CreateQuery(&query, &timing.finish), "HUD upscaler timing");
    }
}
InterfaceUpscaler::~InterfaceUpscaler() = default;

void InterfaceUpscaler::request(std::uint64_t id, Ptr<ID3D11ShaderResourceView> source, unsigned width, unsigned height, unsigned factor) {
    if (!source || !width || !height || factor < 2 || factor > 4 || width * factor > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height * factor > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) throw std::runtime_error("Invalid HUD upscaling request");
    cancel(id);
    auto& job = jobs_.emplace_back();
    job.id = id; job.source = std::move(source); job.width = width; job.height = height; job.factor = factor;
    if (std::min(width, height) < interface_network_minimum) job.stage = Job::Stage::filter;
}
InterfaceUpscaler::Job* InterfaceUpscaler::find(std::uint64_t id) {
    const auto found = std::ranges::find_if(jobs_, [&](const Job& job) { return job.id == id; });
    return found == jobs_.end() ? nullptr : &*found;
}
void InterfaceUpscaler::abort_batch(std::uint64_t cancelled) {
    // Tiles already composed stay done; every other job takes its pending
    // tiles back for a later batch.
    const std::size_t pending = batch_layer_ == upscaler_layers ? batch_index_ : 0;
    for (std::size_t i = pending; i < batch_.size(); ++i) if (batch_[i].job != cancelled)
        if (auto* job = find(batch_[i].job)) job->next_tile = std::min(job->next_tile, batch_[i].tile);
    batch_.clear(); batch_layer_ = batch_index_ = 0;
}
void InterfaceUpscaler::cancel(std::uint64_t id) {
    if (std::ranges::any_of(batch_, [&](const Entry& entry) { return entry.job == id; })) abort_batch(id);
    std::erase_if(jobs_, [&](const Job& job) { return job.id == id; });
}
void InterfaceUpscaler::clear() { jobs_.clear(); batch_.clear(); batch_layer_ = batch_index_ = 0; features_ = {}; }
bool InterfaceUpscaler::queued(std::uint64_t id) const {
    return std::ranges::any_of(jobs_, [&](const Job& job) { return job.id == id; });
}
std::size_t InterfaceUpscaler::bytes() const {
    std::size_t total = 0;
    if (features_[0].texture) total = std::size_t(tile_ + 2 * interface_halo) * (tile_ + 2 * interface_halo) * 8 * 16 * interface_batch * 2;
    for (const auto& job : jobs_) total += job.working + job.bytes;
    return total;
}
void InterfaceUpscaler::constants(ID3D11Buffer* buffer, const void* data, std::size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    checked_ui(context_->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "HUD upscaler parameters");
    std::memcpy(mapped.pData, data, size);
    context_->Unmap(buffer, 0);
}
void InterfaceUpscaler::dispatch(ID3D11ComputeShader* shader, std::initializer_list<ID3D11ShaderResourceView*> inputs,
                                 ID3D11UnorderedAccessView* output, std::initializer_list<ID3D11Buffer*> buffers,
                                 unsigned x, unsigned y, unsigned z) {
    // Release prior inputs first; an output may have been the previous input.
    ID3D11ShaderResourceView* const empty[4]{};
    context_->CSSetShaderResources(0, 4, empty);
    context_->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context_->CSSetShaderResources(0, static_cast<UINT>(inputs.size()), inputs.begin());
    context_->CSSetConstantBuffers(0, static_cast<UINT>(buffers.size()), buffers.begin());
    context_->CSSetShader(shader, nullptr, 0);
    context_->Dispatch(x, y, z);
}
void InterfaceUpscaler::allocate_features() {
    if (features_[0].texture) return;
    const unsigned size = tile_ + 2 * interface_halo;
    for (auto& plane : features_) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = size; texture.MipLevels = 1; texture.ArraySize = 16 * interface_batch;
        texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; texture.SampleDesc.Count = 1;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        checked_ui(device_->CreateTexture2D(&texture, nullptr, &plane.texture), "HUD upscaler features");
        checked_ui(device_->CreateShaderResourceView(plane.texture.Get(), nullptr, &plane.view), "HUD upscaler feature view");
        D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
        access.Format = texture.Format; access.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        access.Texture2DArray.ArraySize = texture.ArraySize;
        checked_ui(device_->CreateUnorderedAccessView(plane.texture.Get(), &access, &plane.output), "HUD upscaler feature output");
    }
}
void InterfaceUpscaler::create_output(Job& job) {
    if (job.output) return;
    const auto w = job.width * job.factor, h = job.height * job.factor;
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = w; texture.Height = h; texture.MipLevels = 0; texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    checked_ui(device_->CreateTexture2D(&texture, nullptr, &job.output), "Upscaled HUD texture");
    checked_ui(device_->CreateShaderResourceView(job.output.Get(), nullptr, &job.view), "Upscaled HUD view");
    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = texture.Format; access.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    checked_ui(device_->CreateUnorderedAccessView(job.output.Get(), &access, &job.output_view), "Upscaled HUD output");
    for (unsigned mw = w, mh = h;; mw = std::max(1U, mw / 2), mh = std::max(1U, mh / 2)) {
        job.bytes += std::size_t(mw) * mh * 4;
        if (mw == 1 && mh == 1) break;
    }
}
bool InterfaceUpscaler::decide(Job& job, bool wait) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto status = context_->Map(job.readback.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (status == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    checked_ui(status, "HUD texture classification");
    std::uint32_t values[2];
    std::memcpy(values, mapped.pData, sizeof(values));
    context_->Unmap(job.readback.Get(), 0);
    job.statistics.Reset(); job.statistics_view.Reset(); job.readback.Reset();
    job.decision.edge = values[0] / 65535.0f;
    job.decision.transparent = values[1] != 0;
    job.decision.network = job.decision.edge >= interface_network_edge;
    if (!job.decision.network) { job.stage = Job::Stage::filter; return true; }
    job.tiles = divide(job.width, tile_) * divide(job.height, tile_);
    if (job.decision.transparent) {
        for (unsigned i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = job.width; texture.Height = job.height; texture.MipLevels = texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; texture.SampleDesc.Count = 1;
            texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            checked_ui(device_->CreateTexture2D(&texture, nullptr, &job.bleed[i]), "HUD colour spread");
            checked_ui(device_->CreateShaderResourceView(job.bleed[i].Get(), nullptr, &job.bleed_view[i]), "HUD colour spread view");
            checked_ui(device_->CreateUnorderedAccessView(job.bleed[i].Get(), nullptr, &job.bleed_output[i]), "HUD colour spread output");
        }
        job.working = std::size_t(job.width) * job.height * 16;
        job.stage = Job::Stage::bleed;
    } else job.stage = Job::Stage::network;
    return true;
}
std::uint64_t InterfaceUpscaler::run(Job& job) {
    const auto w = job.width, h = job.height, f = job.factor;
    JobConstants c;
    c.image[0] = int(w); c.image[1] = int(h); c.factor = f;
    switch (job.stage) {
    case Job::Stage::filter:
        create_output(job);
        constants(job_constants_.Get(), &c, sizeof(c));
        dispatch(filter_.Get(), {job.source.Get()}, job.output_view.Get(), {job_constants_.Get()}, divide(w * f, 8), divide(h * f, 8));
        job.stage = Job::Stage::mips;
        return std::uint64_t(w) * h * f * f * 64;
    case Job::Stage::bleed: {
        constants(job_constants_.Get(), &c, sizeof(c));
        const auto pass = job.bleed_pass;
        dispatch(bleed_.Get(), {pass ? job.bleed_view[(pass - 1) % 2].Get() : job.source.Get()}, job.bleed_output[pass % 2].Get(),
                 {job_constants_.Get()}, divide(w, 8), divide(h, 8));
        if (++job.bleed_pass == interface_bleed_passes) job.stage = Job::Stage::network;
        return std::uint64_t(w) * h * 36;
    }
    case Job::Stage::mips:
        context_->GenerateMips(job.view.Get());
        job.bleed = {}; job.bleed_view = {}; job.bleed_output = {}; job.working = 0;
        job.stage = Job::Stage::done;
        return std::uint64_t(w) * h * f * f * 4;
    default: throw std::runtime_error("HUD upscaler job is not runnable");
    }
}
bool InterfaceUpscaler::form_batch() {
    // Coverage uses the original artwork, so transparent tiles also need
    // just one colour inference.
    batch_.clear(); batch_layer_ = batch_index_ = 0;
    for (auto& job : jobs_) {
        if (job.stage != Job::Stage::network) continue;
        const unsigned tiles_x = divide(job.width, tile_);
        while (job.next_tile < job.tiles && batch_.size() < interface_batch) {
            const unsigned tx = job.next_tile % tiles_x, ty = job.next_tile / tiles_x;
            Entry entry{job.id, job.next_tile, {}, {}, {}};
            entry.interior[0] = int(std::min(tile_, job.width - tx * tile_));
            entry.interior[1] = int(std::min(tile_, job.height - ty * tile_));
            entry.origin[0] = int(tx * tile_) - int(interface_halo);
            entry.origin[1] = int(ty * tile_) - int(interface_halo);
            entry.extent[0] = entry.interior[0] + 2 * int(interface_halo);
            entry.extent[1] = entry.interior[1] + 2 * int(interface_halo);
            batch_.push_back(entry);
            create_output(job);
            ++job.next_tile;
        }
        if (batch_.size() == interface_batch) break;
    }
    if (batch_.empty()) return false;
    allocate_features();
    BatchConstants batch;
    for (std::size_t i = 0; i < batch_.size(); ++i) { batch.extents[i][0] = batch_[i].extent[0]; batch.extents[i][1] = batch_[i].extent[1]; }
    constants(batch_constants_.Get(), &batch, sizeof(batch));
    return true;
}
std::uint64_t InterfaceUpscaler::run_batch() {
    static const auto layout = upscaler_layout();
    const auto job_constants = [&](const Entry& entry, const Job& job) {
        JobConstants c;
        c.image[0] = int(job.width); c.image[1] = int(job.height); c.factor = job.factor;
        for (unsigned axis = 0; axis < 2; ++axis) {
            c.origin[axis] = entry.origin[axis]; c.extent[axis] = entry.extent[axis]; c.interior[axis] = entry.interior[axis];
            c.halo[axis] = int(interface_halo);
            c.target[axis] = (entry.origin[axis] + int(interface_halo)) * int(job.factor);
        }
        return c;
    };
    if (!batch_layer_) {
        const auto slot = batch_index_ / 4, group = batch_index_ % 4;
        const auto& entry = batch_[slot];
        auto* job = find(entry.job);
        if (!job) throw std::runtime_error("HUD upscaler batch lost its job");
        auto c = job_constants(entry, *job);
        c.tile_slot = slot;
        constants(job_constants_.Get(), &c, sizeof(c));
        dispatch(first_.Get(), {job->colour()}, features_[0].output.Get(),
                 {job_constants_.Get(), layers_[layer_index_[0] + group].Get()}, divide(unsigned(entry.extent[0]), 8),
                 divide(unsigned(entry.extent[1]), 8));
        if (++batch_index_ == batch_.size() * 4) { batch_index_ = 0; batch_layer_ = 1; }
        return std::uint64_t(entry.extent[0]) * entry.extent[1] * 3 * 144;
    }
    if (batch_layer_ < upscaler_layers) {
        const auto& layer = layout[batch_layer_];
        unsigned width = 0, height = 0;
        std::uint64_t work = 0;
        for (const auto& entry : batch_) {
            width = std::max(width, unsigned(entry.extent[0])); height = std::max(height, unsigned(entry.extent[1]));
            work += std::uint64_t(entry.extent[0]) * entry.extent[1] * layer.inputs * 144;
        }
        dispatch(conv_.Get(), {features_[(batch_layer_ - 1) % 2].view.Get()}, features_[batch_layer_ % 2].output.Get(),
                 {job_constants_.Get(), layers_[layer_index_[batch_layer_] + batch_index_].Get(), batch_constants_.Get()},
                 divide(width, 8), divide(height, 8), static_cast<unsigned>(batch_.size()));
        if (++batch_index_ == channel_groups(layer)) { batch_index_ = 0; ++batch_layer_; }
        return work;
    }
    // The final layer wrote features_[1]; compose one tile per step.
    const auto& entry = batch_[batch_index_];
    auto* job = find(entry.job);
    if (!job) throw std::runtime_error("HUD upscaler batch lost its job");
    auto c = job_constants(entry, *job);
    c.tile_slot = batch_index_;
    constants(job_constants_.Get(), &c, sizeof(c));
    dispatch(compose_.Get(), {job->colour(), job->source.Get(), features_[1].view.Get()}, job->output_view.Get(), {job_constants_.Get()},
             divide(unsigned(entry.interior[0]) * job->factor, 8), divide(unsigned(entry.interior[1]) * job->factor, 8));
    ++batch_index_;
    if (++job->composed == job->tiles) job->stage = Job::Stage::mips;
    const auto work = std::uint64_t(entry.interior[0]) * entry.interior[1] * job->factor * job->factor * 64;
    if (batch_index_ >= batch_.size()) { batch_.clear(); batch_layer_ = batch_index_ = 0; }
    return work;
}
void InterfaceUpscaler::collect_timings() {
    for (auto& timing : timings_) if (timing.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};
        UINT64 start = 0, finish = 0;
        const auto status = context_->GetData(timing.disjoint.Get(), &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        checked_ui(status, "HUD upscaler timing");
        if (status != S_OK) continue;
        const auto first = context_->GetData(timing.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const auto last = context_->GetData(timing.finish.Get(), &finish, sizeof(finish), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        checked_ui(first, "HUD upscaler timing"); checked_ui(last, "HUD upscaler timing");
        if (first != S_OK || last != S_OK) continue;
        timing.pending = false;
        if (result.Disjoint || !result.Frequency || finish < start) continue;
        const double ns = double(finish - start) * 1e9 / double(result.Frequency);
        // Learn this device's throughput from steps large enough to time.
        if (timing.work >= (8u << 20) && ns > 0) work_per_ns_ = std::clamp(0.75 * work_per_ns_ + 0.25 * double(timing.work) / ns, 50.0, 50000.0);
    }
}
std::vector<InterfaceUpscaler::Finished> InterfaceUpscaler::step(bool finish, double target_ms) {
    collect_timings();
    std::vector<Finished> finished;
    if (jobs_.empty()) return finished;
    auto& timing = timings_[timing_next_];
    const bool timed = !finish && !timing.pending;
    if (timed) { context_->Begin(timing.disjoint.Get()); context_->End(timing.start.Get()); }
    const auto budget = finish ? UINT64_MAX : static_cast<std::uint64_t>(std::clamp(target_ms * 1e6 * work_per_ns_, double(16u << 20), 2e10));
    std::uint64_t used = 0;
    for (auto& job : jobs_) if (job.stage == Job::Stage::classify) {
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 8; buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        checked_ui(device_->CreateBuffer(&buffer, nullptr, &job.statistics), "HUD classification");
        D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
        access.Format = DXGI_FORMAT_R32_UINT; access.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; access.Buffer.NumElements = 2;
        checked_ui(device_->CreateUnorderedAccessView(job.statistics.Get(), &access, &job.statistics_view), "HUD classification view");
        buffer = {};
        buffer.ByteWidth = 8; buffer.Usage = D3D11_USAGE_STAGING; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked_ui(device_->CreateBuffer(&buffer, nullptr, &job.readback), "HUD classification readback");
        const UINT zero[4]{};
        context_->ClearUnorderedAccessViewUint(job.statistics_view.Get(), zero);
        JobConstants c;
        c.image[0] = int(job.width); c.image[1] = int(job.height);
        constants(job_constants_.Get(), &c, sizeof(c));
        dispatch(classify_.Get(), {job.source.Get()}, job.statistics_view.Get(), {job_constants_.Get()}, divide(job.width, 8), divide(job.height, 8));
        context_->CopyResource(job.readback.Get(), job.statistics.Get());
        job.stage = Job::Stage::decide;
        used += std::uint64_t(job.width) * job.height * 16;
    }
    for (;;) {
        if (!batch_.empty()) used += run_batch();
        else {
            // Single-texture work first, then a fresh batch of network tiles.
            Job* next = nullptr;
            for (auto& job : jobs_) {
                if (job.stage == Job::Stage::decide && !decide(job, finish)) continue;
                if (job.stage == Job::Stage::bleed || job.stage == Job::Stage::filter || job.stage == Job::Stage::mips) { next = &job; break; }
            }
            if (next) used += run(*next);
            else if (form_batch()) used += run_batch();
            else break;
        }
        for (auto job = jobs_.begin(); job != jobs_.end();) {
            if (job->stage != Job::Stage::done) { ++job; continue; }
            finished.push_back({job->id, {job->view, job->width * job->factor, job->height * job->factor, job->bytes, job->decision.network}});
            job = jobs_.erase(job);
        }
        if (used >= budget) break;
    }
    if (timed) {
        context_->End(timing.finish.Get()); context_->End(timing.disjoint.Get());
        timing.work = used; timing.pending = true; timing_next_ = (timing_next_ + 1) % timings_.size();
    }
    ID3D11ShaderResourceView* const empty[4]{};
    ID3D11UnorderedAccessView* const none = nullptr;
    context_->CSSetShaderResources(0, 4, empty);
    context_->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    context_->CSSetShader(nullptr, nullptr, 0);
    return finished;
}
}
