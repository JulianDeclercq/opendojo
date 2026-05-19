#include "rmlui_backend.hpp"

#include "log.hpp"

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

// windows.h pollutes the global namespace with `GetNextSibling`, `GetFirstChild`,
// `GetClassName`, etc. as macros that expand to GW_*-style helpers. RmlUi's
// Element / Context classes declare methods with the same names. Undef the
// offenders so the RmlUi headers parse correctly.
#undef GetNextSibling
#undef GetFirstChild
#undef GetClassName
#undef GetObject
#undef LoadImage
#undef PostMessage
#undef SendMessage
#undef DrawText
#undef CreateFont

#include <RmlUi/Core.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/FileInterface.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace opendojo::rml_backend {

namespace {

// ---------------------------------------------------------------------------
// SystemInterface — clock + log routing.
// ---------------------------------------------------------------------------

class OpenDojoSystem final : public Rml::SystemInterface {
public:
    OpenDojoSystem() : start_(std::chrono::steady_clock::now()) {}

    double GetElapsedTime() override {
        using namespace std::chrono;
        return duration<double>(steady_clock::now() - start_).count();
    }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        const char* tag = "RML";
        switch (type) {
            case Rml::Log::LT_ERROR:   tag = "RML err"; break;
            case Rml::Log::LT_ASSERT:  tag = "RML assert"; break;
            case Rml::Log::LT_WARNING: tag = "RML warn"; break;
            default: break;
        }
        OPENDOJO_LOG("%s: %s", tag, message.c_str());
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_;
};

// ---------------------------------------------------------------------------
// FileInterface — resolve paths under our assets directory.
//
// RmlUi calls LoadDocument("main.rml") with a relative path; we tack on
// the asset-dir prefix so the user-visible API stays clean. CSS @import
// paths resolve relative to the importing file, which RmlUi handles
// internally.
// ---------------------------------------------------------------------------

class OpenDojoFile final : public Rml::FileInterface {
public:
    explicit OpenDojoFile(std::filesystem::path base) : base_(std::move(base)) {}

    Rml::FileHandle Open(const Rml::String& path) override {
        std::filesystem::path p = path;
        if (p.is_relative()) p = base_ / p;
        FILE* f = nullptr;
        if (_wfopen_s(&f, p.wstring().c_str(), L"rb") != 0 || !f) {
            OPENDOJO_LOG("rml_backend: Open failed: %s", p.string().c_str());
            return 0;
        }
        return reinterpret_cast<Rml::FileHandle>(f);
    }

    void Close(Rml::FileHandle h) override {
        if (h) std::fclose(reinterpret_cast<FILE*>(h));
    }

    size_t Read(void* buf, size_t size, Rml::FileHandle h) override {
        return std::fread(buf, 1, size, reinterpret_cast<FILE*>(h));
    }

    bool Seek(Rml::FileHandle h, long offset, int origin) override {
        return std::fseek(reinterpret_cast<FILE*>(h), offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle h) override {
        return static_cast<size_t>(std::ftell(reinterpret_cast<FILE*>(h)));
    }

    size_t Length(Rml::FileHandle h) override {
        long cur = std::ftell(reinterpret_cast<FILE*>(h));
        std::fseek(reinterpret_cast<FILE*>(h), 0, SEEK_END);
        long end = std::ftell(reinterpret_cast<FILE*>(h));
        std::fseek(reinterpret_cast<FILE*>(h), cur, SEEK_SET);
        return static_cast<size_t>(end);
    }

private:
    std::filesystem::path base_;
};

// ---------------------------------------------------------------------------
// RenderInterface — DX12 backend.
//
// Strategy:
//   * One PSO with vertex layout (float2 pos, uint32 RGBA colour, float2 uv).
//   * Root signature: 32-bit constants for transform + translation + flags,
//     descriptor table with one SRV slot for the bound texture, static
//     sampler.
//   * Geometry uploaded to mapped UPLOAD-heap buffers on CompileGeometry —
//     cheap and persistent for the lifetime of the compiled geometry.
//   * Textures live in DEFAULT heap, uploaded once via a dedicated synchronous
//     upload path. Lifetime managed with a small id->Resource map.
//   * A 1x1 opaque-white sentinel texture binds in the "no texture" case so
//     a single PSO covers both branches.
//
// All draws record into the command list the caller passes to end_frame().
// We never own the command list / RTV / fence — render_hook handles those.
// ---------------------------------------------------------------------------

struct VBView { D3D12_VERTEX_BUFFER_VIEW v; };
struct IBView { D3D12_INDEX_BUFFER_VIEW  i; };

struct CompiledGeom {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    UINT index_count = 0;
};

struct Texture {
    ComPtr<ID3D12Resource>      resource;
    UINT                        descriptor_index = 0;  // index in our SRV heap
    Rml::Vector2i               dim{};
};

constexpr UINT MAX_TEXTURES = 64;   // font atlases + decorator images

class OpenDojoRenderer final : public Rml::RenderInterface {
public:
    bool init(ID3D12Device* dev, ID3D12CommandQueue* queue,
              DXGI_FORMAT rtv_format) {
        device_ = dev;
        queue_  = queue;
        rtv_format_ = rtv_format;

        if (!build_root_signature()) return false;
        if (!build_pipeline_state()) return false;
        if (!build_srv_heap())       return false;
        if (!build_upload_resources()) return false;
        if (!build_sentinel_white()) return false;

        return true;
    }

    void shutdown() {
        flush_upload();
        compiled_.clear();
        textures_.clear();
        srv_heap_.Reset();
        sampler_heap_.Reset();
        pso_.Reset();
        root_sig_.Reset();
        upload_alloc_.Reset();
        upload_cmd_.Reset();
        upload_fence_.Reset();
        if (upload_event_) { CloseHandle(upload_event_); upload_event_ = nullptr; }
    }

    // ---- Per-frame state pushed by render_hook ----------------------------
    void set_frame_state(ID3D12GraphicsCommandList* cmd_list,
                         unsigned viewport_w, unsigned viewport_h) {
        cmd_list_   = cmd_list;
        viewport_w_ = viewport_w;
        viewport_h_ = viewport_h;
        scissor_enabled_ = false;
        has_transform_   = false;
        // Reset full scissor.
        D3D12_RECT r{0, 0, (LONG)viewport_w_, (LONG)viewport_h_};
        cmd_list_->RSSetScissorRects(1, &r);
    }

    void bind_pipeline() {
        cmd_list_->SetGraphicsRootSignature(root_sig_.Get());
        cmd_list_->SetPipelineState(pso_.Get());
        ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
        cmd_list_->SetDescriptorHeaps(1, heaps);
        cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_VIEWPORT vp{};
        vp.Width    = (float)viewport_w_;
        vp.Height   = (float)viewport_h_;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        cmd_list_->RSSetViewports(1, &vp);

        push_identity_transform();
    }

    // ---- RenderInterface impl ---------------------------------------------

    Rml::CompiledGeometryHandle CompileGeometry(
            Rml::Span<const Rml::Vertex> vertices,
            Rml::Span<const int>         indices) override {
        if (vertices.empty() || indices.empty()) return 0;

        CompiledGeom g;
        const size_t vb_size = vertices.size() * sizeof(Rml::Vertex);
        const size_t ib_size = indices.size() * sizeof(int);

        g.vb = create_upload_buffer(vb_size);
        g.ib = create_upload_buffer(ib_size);
        if (!g.vb || !g.ib) return 0;

        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (FAILED(g.vb->Map(0, &no_read, &mapped))) return 0;
        std::memcpy(mapped, vertices.data(), vb_size);
        g.vb->Unmap(0, nullptr);

        if (FAILED(g.ib->Map(0, &no_read, &mapped))) return 0;
        std::memcpy(mapped, indices.data(), ib_size);
        g.ib->Unmap(0, nullptr);

        g.vbv.BufferLocation = g.vb->GetGPUVirtualAddress();
        g.vbv.SizeInBytes    = (UINT)vb_size;
        g.vbv.StrideInBytes  = sizeof(Rml::Vertex);

        g.ibv.BufferLocation = g.ib->GetGPUVirtualAddress();
        g.ibv.SizeInBytes    = (UINT)ib_size;
        g.ibv.Format         = DXGI_FORMAT_R32_UINT;

        g.index_count = (UINT)indices.size();

        const Rml::CompiledGeometryHandle h = next_geom_id_++;
        compiled_.emplace(h, std::move(g));
        return h;
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        auto it = compiled_.find(geometry);
        if (it == compiled_.end() || !cmd_list_) return;
        const CompiledGeom& g = it->second;

        // Constants layout:
        //   slot 0: viewport_w, viewport_h, translation_x, translation_y  (4 floats)
        //   slot 1: has_transform flag                                    (1 uint)
        //   slot 2..17: optional 4x4 transform                            (16 floats)
        const float c[4] = {
            (float)viewport_w_, (float)viewport_h_,
            translation.x, translation.y,
        };
        cmd_list_->SetGraphicsRoot32BitConstants(0, 4, c, 0);
        const UINT flag = has_transform_ ? 1u : 0u;
        cmd_list_->SetGraphicsRoot32BitConstants(0, 1, &flag, 4);
        if (has_transform_) {
            cmd_list_->SetGraphicsRoot32BitConstants(0, 16, transform_, 5);
        }

        // Bind texture (sentinel white when texture==0).
        Texture* t = nullptr;
        if (texture != 0) {
            auto tit = textures_.find(texture);
            if (tit != textures_.end()) t = &tit->second;
        }
        const UINT desc_index = t ? t->descriptor_index : white_tex_index_;
        const UINT inc = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        gpu_h.ptr += (SIZE_T)desc_index * inc;
        cmd_list_->SetGraphicsRootDescriptorTable(1, gpu_h);

        cmd_list_->IASetVertexBuffers(0, 1, &g.vbv);
        cmd_list_->IASetIndexBuffer(&g.ibv);
        cmd_list_->DrawIndexedInstanced(g.index_count, 1, 0, 0, 0);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override {
        // The GPU may still be referencing this geometry from previous
        // command-list submissions. Defer release a few frames to be safe.
        auto it = compiled_.find(geometry);
        if (it == compiled_.end()) return;
        retired_.push_back({ frame_counter_ + 4, std::move(it->second) });
        compiled_.erase(it);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& /*dim*/,
                                   const Rml::String& source) override {
        // Our menu doesn't reference external images. Stub out to avoid
        // pulling in an image decoder.
        OPENDOJO_LOG("rml_backend: LoadTexture stub for %s — returning 0",
                     source.c_str());
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> data,
                                       Rml::Vector2i dim) override {
        if (dim.x <= 0 || dim.y <= 0 || data.empty()) return 0;

        Texture t;
        t.dim = dim;
        if (!create_rgba_texture(data.data(), dim.x, dim.y, t)) return 0;

        const Rml::TextureHandle h = next_tex_id_++;
        textures_.emplace(h, std::move(t));
        return h;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        auto it = textures_.find(texture);
        if (it == textures_.end()) return;
        // Free the descriptor slot so future textures can reuse it.
        free_descriptor_slot(it->second.descriptor_index);
        textures_.erase(it);
    }

    void EnableScissorRegion(bool enable) override {
        if (!cmd_list_) return;
        scissor_enabled_ = enable;
        if (!enable) {
            D3D12_RECT r{0, 0, (LONG)viewport_w_, (LONG)viewport_h_};
            cmd_list_->RSSetScissorRects(1, &r);
        } else {
            apply_scissor();
        }
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = region;
        if (scissor_enabled_) apply_scissor();
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        if (!transform) {
            has_transform_ = false;
            return;
        }
        has_transform_ = true;
        // RmlUi's Matrix4f is column-major; copy in directly.
        std::memcpy(transform_, transform->data(), sizeof(transform_));
    }

    // ---- End-of-frame bookkeeping -----------------------------------------
    void end_frame_bookkeeping() {
        ++frame_counter_;
        while (!retired_.empty() && retired_.front().drop_at <= frame_counter_) {
            retired_.pop_front();
        }
    }

private:
    // ---- D3D12 setup ------------------------------------------------------

    bool build_root_signature() {
        // 21 32-bit constants (max we touch) + 1 SRV table.
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace  = 0;
        params[0].Constants.Num32BitValues = 21;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters   = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            OPENDOJO_LOG("rml_backend: SerializeRootSignature failed: %s",
                         err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        return SUCCEEDED(device_->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&root_sig_)));
    }

    bool build_pipeline_state() {
        static const char* kHlsl = R"HLSL(
            cbuffer Constants : register(b0) {
                float2 viewport;
                float2 translation;
                uint   has_transform;
                float4x4 transform;
            };
            struct VSIn  { float2 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };
            struct VSOut { float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };
            Texture2D    tex0    : register(t0);
            SamplerState samp0   : register(s0);

            VSOut VSMain(VSIn vin) {
                VSOut vout;
                float2 p = vin.pos + translation;
                float4 world = float4(p, 0.0, 1.0);
                if (has_transform != 0) {
                    world = mul(transform, world);
                }
                // Pixel-space -> clip-space (top-left origin).
                float2 ndc;
                ndc.x =  (world.x / viewport.x) * 2.0 - 1.0;
                ndc.y =  1.0 - (world.y / viewport.y) * 2.0;
                vout.pos = float4(ndc, 0.0, 1.0);
                vout.col = vin.col;
                vout.uv  = vin.uv;
                return vout;
            }

            float4 PSMain(VSOut pin) : SV_TARGET {
                float4 t = tex0.Sample(samp0, pin.uv);
                return t * pin.col;
            }
        )HLSL";

        ComPtr<ID3DBlob> vs, ps, err;
        UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr,
                              "VSMain", "vs_5_0", flags, 0, &vs, &err))) {
            OPENDOJO_LOG("rml_backend: VS compile: %s",
                         err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), nullptr, nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &ps, &err))) {
            OPENDOJO_LOG("rml_backend: PS compile: %s",
                         err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }

        D3D12_INPUT_ELEMENT_DESC il[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_sig_.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout = { il, _countof(il) };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rtv_format_;
        pd.SampleDesc.Count = 1;
        pd.SampleMask = UINT_MAX;

        // Blend: premultiplied-alpha-friendly straight alpha blend.
        D3D12_BLEND_DESC& bd = pd.BlendState;
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend  = D3D12_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp   = D3D12_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // Rasterizer: cull-none, scissor on (we'll set tight scissor when RmlUi asks).
        D3D12_RASTERIZER_DESC& rd = pd.RasterizerState;
        rd.FillMode = D3D12_FILL_MODE_SOLID;
        rd.CullMode = D3D12_CULL_MODE_NONE;
        rd.DepthClipEnable = TRUE;

        // Depth-stencil: none.
        pd.DepthStencilState.DepthEnable   = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DSVFormat = DXGI_FORMAT_UNKNOWN;

        return SUCCEEDED(device_->CreateGraphicsPipelineState(
            &pd, IID_PPV_ARGS(&pso_)));
    }

    bool build_srv_heap() {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = MAX_TEXTURES;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(&srv_heap_))))
            return false;
        srv_free_.reserve(MAX_TEXTURES);
        for (int i = (int)MAX_TEXTURES - 1; i >= 0; --i) srv_free_.push_back((UINT)i);
        return true;
    }

    bool build_upload_resources() {
        if (FAILED(device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&upload_alloc_))))
            return false;
        if (FAILED(device_->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_alloc_.Get(),
                nullptr, IID_PPV_ARGS(&upload_cmd_))))
            return false;
        upload_cmd_->Close();
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&upload_fence_))))
            return false;
        upload_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return upload_event_ != nullptr;
    }

    bool build_sentinel_white() {
        const std::uint8_t pixel[4] = {255, 255, 255, 255};
        Texture t;
        t.dim = {1, 1};
        if (!create_rgba_texture(pixel, 1, 1, t)) return false;
        white_tex_index_ = t.descriptor_index;
        // Keep alive forever in textures_ map under handle 0... but RmlUi
        // uses 0 as "no texture" so we instead store it outside the map.
        white_tex_ = std::move(t);
        return true;
    }

    UINT alloc_descriptor_slot() {
        if (srv_free_.empty()) {
            OPENDOJO_LOG("rml_backend: SRV heap full — texture not bound");
            return UINT_MAX;
        }
        UINT i = srv_free_.back();
        srv_free_.pop_back();
        return i;
    }

    void free_descriptor_slot(UINT i) {
        if (i == UINT_MAX) return;
        srv_free_.push_back(i);
    }

    ComPtr<ID3D12Resource> create_upload_buffer(size_t bytes) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width    = bytes;
        rd.Height   = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format    = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buf;
        device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&buf));
        return buf;
    }

    bool create_rgba_texture(const void* data, int w, int h, Texture& out) {
        UINT slot = alloc_descriptor_slot();
        if (slot == UINT_MAX) return false;

        // Default-heap texture.
        D3D12_HEAP_PROPERTIES def{};
        def.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width   = w;
        td.Height  = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        if (FAILED(device_->CreateCommittedResource(
                &def, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&out.resource)))) {
            free_descriptor_slot(slot);
            return false;
        }

        // Upload heap, sized via GetCopyableFootprints.
        UINT64 upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT  rows = 0;
        UINT64 row_size = 0;
        device_->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows,
                                       &row_size, &upload_size);

        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = upload_size;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> upload;
        if (FAILED(device_->CreateCommittedResource(
                &up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&upload)))) {
            free_descriptor_slot(slot);
            return false;
        }

        std::uint8_t* dst = nullptr;
        D3D12_RANGE no_read{0,0};
        if (FAILED(upload->Map(0, &no_read, (void**)&dst))) {
            free_descriptor_slot(slot);
            return false;
        }
        const std::uint8_t* src = (const std::uint8_t*)data;
        const size_t src_pitch = (size_t)w * 4;
        for (UINT y = 0; y < (UINT)h; ++y) {
            std::memcpy(dst + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch,
                        src + y * src_pitch, src_pitch);
        }
        upload->Unmap(0, nullptr);

        // Synchronous copy via dedicated command list.
        upload_alloc_->Reset();
        upload_cmd_->Reset(upload_alloc_.Get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = out.resource.Get();
        dstLoc.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.Get();
        srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;
        upload_cmd_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = out.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        upload_cmd_->ResourceBarrier(1, &b);

        upload_cmd_->Close();
        ID3D12CommandList* lists[] = { upload_cmd_.Get() };
        queue_->ExecuteCommandLists(1, lists);

        const UINT64 fv = ++upload_fence_value_;
        queue_->Signal(upload_fence_.Get(), fv);
        if (upload_fence_->GetCompletedValue() < fv) {
            upload_fence_->SetEventOnCompletion(fv, upload_event_);
            WaitForSingleObject(upload_event_, INFINITE);
        }

        // Create SRV in our heap at the allocated slot.
        const UINT inc = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_h.ptr += (SIZE_T)slot * inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(out.resource.Get(), &sd, cpu_h);
        out.descriptor_index = slot;
        return true;
    }

    void flush_upload() {
        if (!upload_fence_ || !queue_) return;
        const UINT64 fv = ++upload_fence_value_;
        queue_->Signal(upload_fence_.Get(), fv);
        if (upload_fence_->GetCompletedValue() < fv) {
            upload_fence_->SetEventOnCompletion(fv, upload_event_);
            WaitForSingleObject(upload_event_, INFINITE);
        }
    }

    void apply_scissor() {
        auto clamp = [](LONG v, LONG lo, LONG hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        D3D12_RECT r{};
        r.left   = clamp((LONG)scissor_.p0.x, 0, (LONG)viewport_w_);
        r.top    = clamp((LONG)scissor_.p0.y, 0, (LONG)viewport_h_);
        r.right  = clamp((LONG)scissor_.p1.x, 0, (LONG)viewport_w_);
        r.bottom = clamp((LONG)scissor_.p1.y, 0, (LONG)viewport_h_);
        if (r.right < r.left)   r.right  = r.left;
        if (r.bottom < r.top)   r.bottom = r.top;
        cmd_list_->RSSetScissorRects(1, &r);
    }

    void push_identity_transform() {
        has_transform_ = false;
    }

    // ---- State ------------------------------------------------------------

    ID3D12Device*       device_ = nullptr;
    ID3D12CommandQueue* queue_  = nullptr;
    DXGI_FORMAT         rtv_format_ = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12RootSignature>      root_sig_;
    ComPtr<ID3D12PipelineState>      pso_;
    ComPtr<ID3D12DescriptorHeap>     srv_heap_;
    ComPtr<ID3D12DescriptorHeap>     sampler_heap_;  // unused — static sampler in root sig

    // One-shot upload path used only during texture creation.
    ComPtr<ID3D12CommandAllocator>     upload_alloc_;
    ComPtr<ID3D12GraphicsCommandList>  upload_cmd_;
    ComPtr<ID3D12Fence>                upload_fence_;
    HANDLE                             upload_event_ = nullptr;
    UINT64                             upload_fence_value_ = 0;

    // Per-frame state, set by render_hook.
    ID3D12GraphicsCommandList* cmd_list_ = nullptr;
    unsigned viewport_w_ = 0;
    unsigned viewport_h_ = 0;
    bool     scissor_enabled_ = false;
    Rml::Rectanglei scissor_{};
    bool     has_transform_ = false;
    float    transform_[16]{};

    // Geometry / textures.
    std::unordered_map<Rml::CompiledGeometryHandle, CompiledGeom> compiled_;
    std::unordered_map<Rml::TextureHandle,          Texture>      textures_;
    Rml::CompiledGeometryHandle next_geom_id_ = 1;
    Rml::TextureHandle          next_tex_id_  = 1;

    struct Retired { uint64_t drop_at; CompiledGeom g; };
    std::deque<Retired> retired_;
    uint64_t frame_counter_ = 0;

    Texture white_tex_{};
    UINT    white_tex_index_ = 0;

    // Stack of free descriptor slots in srv_heap_.
    std::vector<UINT> srv_free_;
};

// ---------------------------------------------------------------------------
// Translation table: Win32 VK -> Rml::Input::KeyIdentifier.
// Covers the keys we actually feed (arrows, enter, escape, tab, alphanumerics,
// modifiers). RmlUi's nav uses arrows + enter + esc.
// ---------------------------------------------------------------------------

Rml::Input::KeyIdentifier translate_vk(WPARAM vk) {
    using K = Rml::Input::KeyIdentifier;
    switch (vk) {
        case VK_LEFT:   return K::KI_LEFT;
        case VK_RIGHT:  return K::KI_RIGHT;
        case VK_UP:     return K::KI_UP;
        case VK_DOWN:   return K::KI_DOWN;
        case VK_RETURN: return K::KI_RETURN;
        case VK_ESCAPE: return K::KI_ESCAPE;
        case VK_TAB:    return K::KI_TAB;
        case VK_BACK:   return K::KI_BACK;
        case VK_SPACE:  return K::KI_SPACE;
        case VK_DELETE: return K::KI_DELETE;
        case VK_HOME:   return K::KI_HOME;
        case VK_END:    return K::KI_END;
        case VK_PRIOR:  return K::KI_PRIOR;
        case VK_NEXT:   return K::KI_NEXT;
        case VK_LSHIFT: case VK_SHIFT:   return K::KI_LSHIFT;
        case VK_LCONTROL: case VK_CONTROL: return K::KI_LCONTROL;
        case VK_LMENU:    case VK_MENU:    return K::KI_LMENU;
        default: break;
    }
    if (vk >= 'A' && vk <= 'Z') {
        return (Rml::Input::KeyIdentifier)(K::KI_A + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return (Rml::Input::KeyIdentifier)(K::KI_0 + (vk - '0'));
    }
    return K::KI_UNKNOWN;
}

int compute_modifiers() {
    int m = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= Rml::Input::KM_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= Rml::Input::KM_CTRL;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= Rml::Input::KM_ALT;
    return m;
}

// ---------------------------------------------------------------------------
// Module-static state.
// ---------------------------------------------------------------------------

struct State {
    OpenDojoSystem    sys{};
    OpenDojoFile*     file_iface = nullptr;
    OpenDojoRenderer  renderer{};

    Rml::Context*         context  = nullptr;
    Rml::ElementDocument* document = nullptr;

    ID3D12Device*       device    = nullptr;
    ID3D12CommandQueue* queue     = nullptr;
    IDXGISwapChain3*    swapchain = nullptr;
    HWND                hwnd      = nullptr;

    unsigned width  = 0;
    unsigned height = 0;
    bool     ready  = false;
};

State* g = nullptr;

}  // anonymous namespace

// ===========================================================================
//  Public API
// ===========================================================================

bool init(ID3D12Device* device, ID3D12CommandQueue* queue,
          IDXGISwapChain3* swapchain, HWND hwnd,
          DXGI_FORMAT rtv_format, unsigned /*buffer_count*/,
          const wchar_t* assets_dir) {
    if (g) return true;
    g = new State();
    g->device    = device;
    g->queue     = queue;
    g->swapchain = swapchain;
    g->hwnd      = hwnd;

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g->width  = desc.BufferDesc.Width;
    g->height = desc.BufferDesc.Height;

    if (!g->renderer.init(device, queue, rtv_format)) {
        OPENDOJO_LOG("rml_backend: renderer init failed");
        delete g; g = nullptr;
        return false;
    }

    g->file_iface = new OpenDojoFile(std::filesystem::path(assets_dir));

    Rml::SetSystemInterface(&g->sys);
    Rml::SetRenderInterface(&g->renderer);
    Rml::SetFileInterface(g->file_iface);

    if (!Rml::Initialise()) {
        OPENDOJO_LOG("rml_backend: Rml::Initialise failed");
        return false;
    }

    // Load every .ttf/.otf in opendojo/ui/font. Each TTF self-declares its
    // family name; the CSS references whichever family it wants. Scanning
    // the dir means the user can drop in any font (e.g. "Bebas Neue Pro
    // Regular.ttf") and reference it in main.rcss without touching C++.
    const std::filesystem::path font_dir =
        std::filesystem::path(assets_dir) / L"font";
    bool loaded_any = false;
    auto try_load = [&](const Rml::String& path) {
        if (Rml::LoadFontFace(path, /*fallback*/ !loaded_any)) {
            OPENDOJO_LOG("rml_backend: loaded font %s", path.c_str());
            loaded_any = true;
        }
    };

    std::error_code ec;
    if (std::filesystem::exists(font_dir, ec)) {
        for (const auto& entry :
                std::filesystem::directory_iterator(font_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            auto ext = entry.path().extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext != ".ttf" && ext != ".otf") continue;
            try_load(entry.path().string());
        }
    } else {
        OPENDOJO_LOG("rml_backend: font dir not found: %s",
                     font_dir.string().c_str());
    }

    // System-font glyph fallback (Win10+ ships Bahnschrift; Segoe / Arial
    // are universally present). Loaded last so they only fill in glyphs
    // the primary user font can't supply.
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::filesystem::path fonts_root =
        std::filesystem::path(windir) / L"Fonts";
    const std::filesystem::path system_fonts[] = {
        fonts_root / L"bahnschrift.ttf",
        fonts_root / L"seguisb.ttf",
        fonts_root / L"arial.ttf",
    };
    for (const auto& f : system_fonts) {
        if (std::filesystem::exists(f, ec)) try_load(f.string());
    }

    if (!loaded_any) {
        OPENDOJO_LOG("rml_backend: WARNING — no fonts could be loaded; "
                     "text will not render. Drop a TTF in opendojo/ui/font/.");
    }

    g->context = Rml::CreateContext(
        "opendojo",
        Rml::Vector2i((int)g->width, (int)g->height));
    if (!g->context) {
        OPENDOJO_LOG("rml_backend: CreateContext failed");
        return false;
    }

    g->document = g->context->LoadDocument("main.rml");
    if (!g->document) {
        OPENDOJO_LOG("rml_backend: LoadDocument main.rml failed");
    } else {
        g->document->Show();
    }

    g->ready = true;
    OPENDOJO_LOG("rml_backend: ready (%ux%u, rtv_fmt=%u)",
                 g->width, g->height, static_cast<unsigned>(rtv_format));
    return true;
}

void shutdown() {
    if (!g) return;
    if (g->context) {
        Rml::RemoveContext("opendojo");
        g->context = nullptr;
    }
    Rml::Shutdown();
    g->renderer.shutdown();
    delete g->file_iface;
    delete g; g = nullptr;
}

void begin_frame() {
    if (!g || !g->context) return;
    g->context->Update();
}

void end_frame(ID3D12GraphicsCommandList* cmd_list,
               D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle,
               unsigned viewport_width, unsigned viewport_height) {
    if (!g || !g->context) return;
    if (viewport_width != g->width || viewport_height != g->height) {
        resize(viewport_width, viewport_height);
    }
    cmd_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    g->renderer.set_frame_state(cmd_list, viewport_width, viewport_height);
    g->renderer.bind_pipeline();
    g->context->Render();
    g->renderer.end_frame_bookkeeping();
}

void resize(unsigned width, unsigned height) {
    if (!g) return;
    g->width  = width;
    g->height = height;
    if (g->context) {
        g->context->SetDimensions(Rml::Vector2i((int)width, (int)height));
    }
}

bool process_win32_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g || !g->context) return false;
    switch (msg) {
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            g->context->ProcessMouseMove(x, y, compute_modifiers());
            return true;
        }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
            const int btn = (msg == WM_LBUTTONDOWN) ? 0
                          : (msg == WM_RBUTTONDOWN) ? 1 : 2;
            g->context->ProcessMouseButtonDown(btn, compute_modifiers());
            return true;
        }
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
            const int btn = (msg == WM_LBUTTONUP) ? 0
                          : (msg == WM_RBUTTONUP) ? 1 : 2;
            g->context->ProcessMouseButtonUp(btn, compute_modifiers());
            return true;
        }
        case WM_MOUSEWHEEL: {
            const float dy = -((float)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA);
            g->context->ProcessMouseWheel(dy, compute_modifiers());
            return true;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            auto k = translate_vk(wparam);
            if (k != Rml::Input::KI_UNKNOWN) {
                g->context->ProcessKeyDown(k, compute_modifiers());
                return true;
            }
            return false;
        }
        case WM_KEYUP: case WM_SYSKEYUP: {
            auto k = translate_vk(wparam);
            if (k != Rml::Input::KI_UNKNOWN) {
                g->context->ProcessKeyUp(k, compute_modifiers());
                return true;
            }
            return false;
        }
        case WM_CHAR: {
            wchar_t wc = (wchar_t)wparam;
            char utf8[8] = {};
            int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8) - 1,
                                        nullptr, nullptr);
            if (n > 0) g->context->ProcessTextInput(Rml::String(utf8, (size_t)n));
            return true;
        }
        default: return false;
    }
}

void nav_move(NavDir dir) {
    if (!g || !g->context) return;
    using K = Rml::Input::KeyIdentifier;
    K k = K::KI_UNKNOWN;
    switch (dir) {
        case NavDir::Up:    k = K::KI_UP;    break;
        case NavDir::Down:  k = K::KI_DOWN;  break;
        case NavDir::Left:  k = K::KI_LEFT;  break;
        case NavDir::Right: k = K::KI_RIGHT; break;
    }
    if (k == K::KI_UNKNOWN) return;
    g->context->ProcessKeyDown(k, 0);
    g->context->ProcessKeyUp(k,   0);
}

void nav_activate() {
    if (!g || !g->context) return;
    g->context->ProcessKeyDown(Rml::Input::KI_RETURN, 0);
    g->context->ProcessKeyUp  (Rml::Input::KI_RETURN, 0);
}

void nav_back() {
    if (!g || !g->context) return;
    g->context->ProcessKeyDown(Rml::Input::KI_ESCAPE, 0);
    g->context->ProcessKeyUp  (Rml::Input::KI_ESCAPE, 0);
}

Rml::Context*         context()  { return g ? g->context  : nullptr; }
Rml::ElementDocument* document() { return g ? g->document : nullptr; }

}  // namespace opendojo::rml_backend
