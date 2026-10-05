#include "hwrt.h"

#include "profiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef _WIN32
// ---------------------------------------------------------------------------
// Other platforms: no DirectX. (A Vulkan ray-query port would need SPIR-V
// tooling that this project does not depend on.)
// ---------------------------------------------------------------------------
struct HwRayTracer::Impl {};
HwRayTracer::HwRayTracer() = default;
HwRayTracer::~HwRayTracer() = default;
HwRtInfo HwRayTracer::probe(bool) {
    HwRtInfo i;
    i.reason = "hardware ray tracing uses DirectX 12 (Windows only)";
    return i;
}
bool HwRayTracer::init(bool, std::string& e) {
    info_ = probe(false);
    e = info_.reason;
    return false;
}
void HwRayTracer::shutdown() {}
bool HwRayTracer::ready() const { return false; }
bool HwRayTracer::start(const rt::SceneData&, const rt::View&, const rt::Settings&, int, int, int, std::string& e) {
    e = info_.reason;
    return false;
}
void HwRayTracer::stop() {}
void HwRayTracer::step(double) {}
void HwRayTracer::setTargetSamples(int) {}
bool HwRayTracer::running() const { return false; }
int HwRayTracer::samples() const { return 0; }
int HwRayTracer::width() const { return 0; }
int HwRayTracer::height() const { return 0; }
double HwRayTracer::elapsedSeconds() const { return 0; }
double HwRayTracer::samplesPerSecond() const { return 0; }
double HwRayTracer::gpuMsPerSubmit() const { return 0; }
bool HwRayTracer::takeImage(std::vector<uint8_t>&) { return false; }
void HwRayTracer::finish() {}

#else
// ---------------------------------------------------------------------------
// Windows: Direct3D 12 + DXR 1.1
// ---------------------------------------------------------------------------
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIDL_EXPLICIT_AGGREGATE_RETURNS  // correct ABI for the descriptor-handle getters
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace {

#include "shaders/hwrt_shader.inl"

// --- DXR declarations missing from MinGW's d3d12.h (layouts from the Windows SDK) ---
struct RtGpuVaStride {
    D3D12_GPU_VIRTUAL_ADDRESS start;
    UINT64 stride;
};
struct RtTrianglesDesc {
    D3D12_GPU_VIRTUAL_ADDRESS transform3x4;
    DXGI_FORMAT indexFormat;
    DXGI_FORMAT vertexFormat;
    UINT indexCount;
    UINT vertexCount;
    D3D12_GPU_VIRTUAL_ADDRESS indexBuffer;
    RtGpuVaStride vertexBuffer;
};
struct RtGeometryDesc {
    UINT type;   // 0 = triangles
    UINT flags;  // 1 = opaque
    union {
        RtTrianglesDesc triangles;
        struct {
            UINT64 count;
            RtGpuVaStride aabbs;
        } aabbs;
    };
};
struct RtBuildInputs {
    UINT type;  // 0 = top level, 1 = bottom level
    UINT flags; // 4 = prefer fast trace
    UINT numDescs;
    UINT descsLayout;  // 0 = array
    union {
        D3D12_GPU_VIRTUAL_ADDRESS instanceDescs;
        const RtGeometryDesc* geometryDescs;
    };
};
struct RtBuildDesc {
    D3D12_GPU_VIRTUAL_ADDRESS dest;
    RtBuildInputs inputs;
    D3D12_GPU_VIRTUAL_ADDRESS source;
    D3D12_GPU_VIRTUAL_ADDRESS scratch;
};
struct RtPrebuildInfo {
    UINT64 resultMax, scratch, updateScratch;
};
struct RtInstanceDesc {
    float transform[3][4];
    UINT instanceIdAndMask;          // InstanceID : 24, InstanceMask : 8
    UINT contributionAndFlags;       // hit group contribution : 24, flags : 8
    D3D12_GPU_VIRTUAL_ADDRESS blas;
};
static_assert(sizeof(RtInstanceDesc) == 64, "instance layout");
struct RtOptions5 {
    BOOL srvOnlyTiledResourceTier3;
    int renderPassesTier;
    int raytracingTier;
};
constexpr UINT kStateAccelStructure = 0x400000;  // D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
constexpr int kTier11 = 11;
// IIDs and vtable slots (ID3D12Device5::GetRaytracingAccelerationStructurePrebuildInfo = 63,
// ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure = 72).
const GUID kIID_Device5 = {0x8b4f173b, 0x2fea, 0x4b80, {0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d}};
const GUID kIID_List4 = {0x8754318e, 0xd3a9, 0x4541, {0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74}};
using PrebuildFn = void(STDMETHODCALLTYPE*)(void*, const RtBuildInputs*, RtPrebuildInfo*);
using BuildFn = void(STDMETHODCALLTYPE*)(void*, const RtBuildDesc*, UINT, const void*);

using PFN_CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_Serialize = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
using PFN_CreateFactory = HRESULT(WINAPI*)(REFIID, void**);

struct Api {
    HMODULE d3d12 = nullptr, dxgi = nullptr;
    PFN_CreateDevice createDevice = nullptr;
    PFN_Serialize serialize = nullptr;
    PFN_CreateFactory createFactory = nullptr;
    bool load() {
        if (createDevice) return true;
        d3d12 = LoadLibraryA("d3d12.dll");
        dxgi = LoadLibraryA("dxgi.dll");
        if (!d3d12 || !dxgi) return false;
        createDevice = (PFN_CreateDevice)(void*)GetProcAddress(d3d12, "D3D12CreateDevice");
        serialize = (PFN_Serialize)(void*)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
        createFactory = (PFN_CreateFactory)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
        return createDevice && serialize && createFactory;
    }
} g_api;

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string narrow(const wchar_t* w) {
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
}

int raytracingTier(ID3D12Device* dev) {
    RtOptions5 o5{};
    if (FAILED(dev->CheckFeatureSupport((D3D12_FEATURE)27, &o5, sizeof o5))) return 0;
    struct {
        int highest;
    } sm{0x65};
    if (FAILED(dev->CheckFeatureSupport((D3D12_FEATURE)7, &sm, sizeof sm)) || sm.highest < 0x65) return 0;
    return o5.raytracingTier;
}

// Picks the adapter: the first hardware GPU with DXR 1.1, else WARP if allowed.
IDXGIAdapter1* chooseAdapter(bool allowWarp, HwRtInfo& info) {
    info = HwRtInfo();
    if (!g_api.load()) {
        info.reason = "Direct3D 12 is not available on this system";
        return nullptr;
    }
    IDXGIFactory4* factory = nullptr;
    if (FAILED(g_api.createFactory(__uuidof(IDXGIFactory4), (void**)&factory))) {
        info.reason = "DXGI 1.4 is not available";
        return nullptr;
    }
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        const bool soft = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        ID3D12Device* dev = nullptr;
        int tier = 0;
        if (SUCCEEDED(g_api.createDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev))) {
            tier = raytracingTier(dev);
            dev->Release();
        }
        info.adapters.push_back(narrow(d.Description) + (soft ? " (software)" : "") + ": DXR " +
                                (tier >= kTier11 ? "1.1" : tier >= 10 ? "1.0" : "no"));
        if (!chosen && !soft && tier >= kTier11) {
            chosen = a;
            info.adapter = narrow(d.Description);
            continue;
        }
        a->Release();
    }
    if (!chosen && allowWarp) {
        IDXGIAdapter1* warp = nullptr;
        if (SUCCEEDED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&warp))) {
            ID3D12Device* dev = nullptr;
            int tier = 0;
            if (SUCCEEDED(g_api.createDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev))) {
                tier = raytracingTier(dev);
                dev->Release();
            }
            if (tier >= kTier11) {
                chosen = warp;
                info.adapter = "Microsoft WARP (software)";
                info.software = true;
            } else {
                warp->Release();
            }
        }
    }
    factory->Release();
    info.available = chosen != nullptr;
    if (!chosen)
        info.reason = "no GPU with DXR 1.1 ray tracing (RTX / RDNA2 / Arc) found";
    return chosen;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CreationNodeMask = p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<UINT64>(size, 256);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

struct alignas(16) Params {
    float eye[3], tanHalf;
    float forward[3], orthoHalf;
    float right[3], aspect;
    float up[3], lensRadius;
    float skyLow[3], focusDist;
    float skyHigh[3], clampIndirect;
    UINT width, height, maxBounces, lightCount;
    UINT ortho, blades;
    float exposure;
    UINT anyPass;
    UINT geom1Offset, pad1, pad2, pad3;
};

}  // namespace

struct HwRayTracer::Impl {
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    void* device5 = nullptr;  // ID3D12Device5 (raw vtable use)
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    void* list4 = nullptr;  // ID3D12GraphicsCommandList4
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    ID3D12RootSignature* rootSig = nullptr;
    ID3D12PipelineState* tracePso = nullptr, *resolvePso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT descSize = 0;
    // Scene resources
    ID3D12Resource *tris = nullptr, *mats = nullptr, *lights = nullptr, *verts = nullptr, *triMap = nullptr,
                   *texArray = nullptr,
                   *texUpload = nullptr, *blas = nullptr, *tlas = nullptr, *scratchB = nullptr, *scratchT = nullptr,
                   *instances = nullptr, *accum = nullptr, *outBuf = nullptr, *readback = nullptr, *params = nullptr;
    int w = 0, h = 0, target = 0, samples = 0, row = 0, lightCount = 0;
    bool running = false, resumable = false, inFlight = false, readbackPending = false;
    double startTime = 0, endTime = 0, submitTime = 0, lastResolve = 0, lastGpuMs = 0;
    int rowsPerSubmit = 8;
    double rowsTotal = 0;
    std::vector<uint8_t> image;
    bool imageDirty = false;
    rt::View view;
    float exposure = 1;

    void releaseScene() {
        for (ID3D12Resource** r : {&tris, &mats, &lights, &verts, &triMap, &texArray, &texUpload, &blas, &tlas, &scratchB,
                                   &scratchT, &instances, &accum, &outBuf, &readback, &params})
            release(*r);
    }
    void releaseAll() {
        waitIdle();
        releaseScene();
        release(heap);
        release(tracePso);
        release(resolvePso);
        release(rootSig);
        release(fence);
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        if (list4) ((IUnknown*)list4)->Release();
        list4 = nullptr;
        release(list);
        release(alloc);
        release(queue);
        if (device5) ((IUnknown*)device5)->Release();
        device5 = nullptr;
        release(device);
        release(adapter);
    }
    void waitIdle() {
        if (!queue || !fence) return;
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
    ID3D12Resource* buffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
                           D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES hp = heapProps(heapType);
        D3D12_RESOURCE_DESC d = bufferDesc(size, flags);
        ID3D12Resource* r = nullptr;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                   __uuidof(ID3D12Resource), (void**)&r)))
            return nullptr;
        return r;
    }
    ID3D12Resource* upload(const void* data, size_t size) {
        ID3D12Resource* r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!r) return nullptr;
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(r->Map(0, &none, &p))) {
            std::memcpy(p, data, size);
            r->Unmap(0, nullptr);
        }
        return r;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * descSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(int i) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)i * descSize;
        return h;
    }
    void submit() {
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++fenceValue);
        inFlight = true;
        submitTime = rt::nowSeconds();
    }
    bool beginList() {
        if (FAILED(alloc->Reset())) return false;
        return SUCCEEDED(list->Reset(alloc, nullptr));
    }
};

HwRayTracer::HwRayTracer() : impl_(new Impl) {}
HwRayTracer::~HwRayTracer() { shutdown(); }

HwRtInfo HwRayTracer::probe(bool allowWarp) {
    static bool cached = false, cachedWarp = false;
    static HwRtInfo info;
    if (cached && cachedWarp == allowWarp) return info;
    IDXGIAdapter1* a = chooseAdapter(allowWarp, info);
    if (a) a->Release();
    cached = true;
    cachedWarp = allowWarp;
    return info;
}

bool HwRayTracer::ready() const { return impl_ && impl_->tracePso != nullptr; }

bool HwRayTracer::init(bool allowWarp, std::string& error) {
    shutdown();
    impl_.reset(new Impl);
    Impl& m = *impl_;
    m.adapter = chooseAdapter(allowWarp, info_);
    if (!m.adapter) {
        error = info_.reason;
        return false;
    }
    auto fail = [&](const char* what) {
        error = what;
        m.releaseAll();
        info_.available = false;
        info_.reason = what;
        return false;
    };
    if (FAILED(g_api.createDevice(m.adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&m.device)))
        return fail("D3D12CreateDevice failed");
    if (FAILED(m.device->QueryInterface(kIID_Device5, &m.device5))) return fail("ID3D12Device5 is not supported");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&m.queue)))
        return fail("CreateCommandQueue failed");
    if (FAILED(m.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                (void**)&m.alloc)))
        return fail("CreateCommandAllocator failed");
    if (FAILED(m.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.alloc, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), (void**)&m.list)))
        return fail("CreateCommandList failed");
    m.list->Close();
    if (FAILED(m.list->QueryInterface(kIID_List4, &m.list4))) return fail("ID3D12GraphicsCommandList4 is not supported");
    if (FAILED(m.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&m.fence)))
        return fail("CreateFence failed");
    m.fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // Root signature: b0 parameters, b1 per-dispatch constants, t0 TLAS (root
    // SRV), table {t1..t4, u0..u1}, static linear/wrap sampler s0.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;
    ranges[0].BaseShaderRegister = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 4;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 2;
    params[3].DescriptorTable.pDescriptorRanges = ranges;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = 1000.0f;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob *blob = nullptr, *errBlob = nullptr;
    if (FAILED(g_api.serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob))) {
        release(errBlob);
        return fail("root signature serialization failed");
    }
    HRESULT hr = m.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               __uuidof(ID3D12RootSignature), (void**)&m.rootSig);
    release(blob);
    release(errBlob);
    if (FAILED(hr)) return fail("CreateRootSignature failed");
    for (int k = 0; k < 2; ++k) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m.rootSig;
        pd.CS.pShaderBytecode = k == 0 ? (const void*)kHwrtTraceDXIL : (const void*)kHwrtResolveDXIL;
        pd.CS.BytecodeLength = k == 0 ? sizeof kHwrtTraceDXIL : sizeof kHwrtResolveDXIL;
        if (FAILED(m.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                        (void**)(k == 0 ? &m.tracePso : &m.resolvePso))))
            return fail("CreateComputePipelineState failed (shader model 6.5 / DXR 1.1)");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 7;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&m.heap)))
        return fail("CreateDescriptorHeap failed");
    m.descSize = m.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void HwRayTracer::shutdown() {
    if (impl_) impl_->releaseAll();
}

bool HwRayTracer::start(const rt::SceneData& scene, const rt::View& view, const rt::Settings& settings, int w, int h,
                        int targetSamples, std::string& error) {
    Impl& m = *impl_;
    if (!ready()) {
        error = info_.reason.empty() ? "hardware ray tracing is not initialised" : info_.reason;
        return false;
    }
    PROF_SCOPE("hwrt upload + build");
    m.waitIdle();
    m.inFlight = false;
    m.releaseScene();
    rt::PackedScene packed;
    rt::packScene(scene, 1024, packed);
    const size_t triCount = scene.triangleCount();
    // Vertex positions: opaque triangles first (BLAS geometry 0, flagged
    // opaque), then see-through ones (geometry 1). triMap turns a geometry +
    // PrimitiveIndex() back into the packed triangle index.
    auto isPass = [&](size_t k) { return packed.tris[(k * rt::kTriTexels + 2) * 4 + 3] > 0.5f; };
    std::vector<uint32_t> order;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k < triCount; ++k)
            if (isPass(k) == (pass == 1)) order.push_back((uint32_t)k);
    size_t opaqueCount = 0;
    for (size_t k = 0; k < triCount; ++k) opaqueCount += !isPass(k);
    const size_t passCount = triCount - opaqueCount;
    std::vector<float> pos(std::max<size_t>(1, triCount) * 9, 0.0f);
    for (size_t j = 0; j < order.size(); ++j)
        for (int c = 0; c < 3; ++c)
            for (int a = 0; a < 3; ++a) pos[j * 9 + c * 3 + a] = packed.tris[(order[j] * rt::kTriTexels + c) * 4 + a];
    if (order.empty()) order.push_back(0);
    m.tris = m.upload(packed.tris.data(), packed.tris.size() * 4);
    m.mats = m.upload(packed.mats.data(), packed.mats.size() * 4);
    m.lights = m.upload(packed.lights.data(), packed.lights.size() * 4);
    m.verts = m.upload(pos.data(), pos.size() * 4);
    m.triMap = m.upload(order.data(), order.size() * 4);
    m.lightCount = packed.lightCount;
    if (!m.tris || !m.mats || !m.lights || !m.verts || !m.triMap) {
        error = "out of GPU memory (scene buffers)";
        return false;
    }

    // Texture array (default heap) + its upload buffer.
    const int layers = std::max(1, packed.texLayers), ts = packed.texSize;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = ts;
    td.Height = ts;
    td.DepthOrArraySize = (UINT16)layers;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(m.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, __uuidof(ID3D12Resource), (void**)&m.texArray))) {
        error = "out of GPU memory (textures)";
        return false;
    }
    const UINT rowPitch = (UINT)((ts * 4 + 255) & ~255);
    const UINT64 layerBytes = (UINT64)rowPitch * ts;
    m.texUpload = m.buffer(layerBytes * layers, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        uint8_t* p = nullptr;
        D3D12_RANGE none{0, 0};
        m.texUpload->Map(0, &none, (void**)&p);
        for (int l = 0; l < layers; ++l)
            for (int y = 0; y < ts; ++y) {
                const uint8_t* src = packed.texLayers ? &packed.texels[(((size_t)l * ts + y) * ts) * 4] : packed.texels.data();
                std::memcpy(p + l * layerBytes + (UINT64)y * rowPitch, src, packed.texLayers ? (size_t)ts * 4 : 4);
            }
        m.texUpload->Unmap(0, nullptr);
    }

    // Accumulation target, packed RGBA8 output, readback, parameters.
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 __uuidof(ID3D12Resource), (void**)&m.accum))) {
        error = "out of GPU memory (image)";
        return false;
    }
    m.outBuf = m.buffer((UINT64)w * h * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m.readback = m.buffer((UINT64)w * h * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    Params pr{};
    auto v3 = [](float* d, Vec3 v) { d[0] = v.x, d[1] = v.y, d[2] = v.z; };
    v3(pr.eye, view.eye);
    v3(pr.forward, view.forward);
    v3(pr.right, view.right);
    v3(pr.up, view.up);
    v3(pr.skyLow, scene.skyLow);
    v3(pr.skyHigh, scene.skyHigh);
    pr.tanHalf = view.tanHalfFov;
    pr.orthoHalf = view.orthoHalfHeight;
    pr.aspect = view.aspect;
    pr.lensRadius = view.lensRadius;
    pr.focusDist = view.focusDistance;
    pr.clampIndirect = settings.clampIndirect;
    pr.width = w;
    pr.height = h;
    pr.maxBounces = settings.maxBounces;
    pr.lightCount = packed.lightCount;
    pr.ortho = view.ortho ? 1 : 0;
    pr.blades = view.blades;
    pr.exposure = view.exposure;
    pr.anyPass = packed.anyShadowPass ? 1 : 0;
    pr.geom1Offset = opaqueCount > 0 && passCount > 0 ? (UINT)opaqueCount : 0;
    m.params = m.upload(&pr, sizeof pr);
    if (!m.outBuf || !m.readback || !m.params) {
        error = "out of GPU memory";
        return false;
    }

    // Descriptors: t1 tris, t2 mats, t3 lights, t4 textures, u0 accum, u1 output.
    auto srvBuffer = [&](ID3D12Resource* r, UINT count, int slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = count;
        s.Buffer.StructureByteStride = 16;
        m.device->CreateShaderResourceView(r, &s, m.cpuHandle(slot));
    };
    srvBuffer(m.tris, (UINT)(packed.tris.size() / 4), 0);
    srvBuffer(m.mats, (UINT)(packed.mats.size() / 4), 1);
    srvBuffer(m.lights, (UINT)(packed.lights.size() / 4), 2);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2DArray.MipLevels = 1;
        s.Texture2DArray.ArraySize = layers;
        m.device->CreateShaderResourceView(m.texArray, &s, m.cpuHandle(3));
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = (UINT)order.size();
        s.Buffer.StructureByteStride = 4;
        m.device->CreateShaderResourceView(m.triMap, &s, m.cpuHandle(4));
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m.device->CreateUnorderedAccessView(m.accum, nullptr, &u, m.cpuHandle(5));
        D3D12_UNORDERED_ACCESS_VIEW_DESC b{};
        b.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        b.Buffer.NumElements = (UINT)w * h;
        b.Buffer.StructureByteStride = 4;
        m.device->CreateUnorderedAccessView(m.outBuf, nullptr, &b, m.cpuHandle(6));
    }

    // Acceleration structures: one BLAS with every (world-space) triangle,
    // one TLAS with a single identity instance.
    RtGeometryDesc geoms[2] = {};
    UINT numGeoms = 0;
    auto addGeometry = [&](size_t first, size_t count, bool opaque) {
        RtGeometryDesc& g = geoms[numGeoms++];
        g.type = 0;
        g.flags = opaque ? 1 : 0;  // see-through triangles reach the shader as candidates
        g.triangles.vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.triangles.vertexCount = (UINT)(count * 3);
        g.triangles.vertexBuffer.start = m.verts->GetGPUVirtualAddress() + first * 36;
        g.triangles.vertexBuffer.stride = 12;
    };
    if (opaqueCount > 0 || triCount == 0) addGeometry(0, std::max<size_t>(1, opaqueCount), true);
    if (passCount > 0) addGeometry(opaqueCount, passCount, false);
    RtBuildInputs bin{};
    bin.type = 1;
    bin.flags = 4;  // prefer fast trace
    bin.numDescs = numGeoms;
    bin.geometryDescs = geoms;
    RtPrebuildInfo bpi{};
    void** dv = *(void***)m.device5;
    ((PrebuildFn)dv[63])(m.device5, &bin, &bpi);
    RtInstanceDesc inst{};
    inst.transform[0][0] = inst.transform[1][1] = inst.transform[2][2] = 1.0f;
    inst.instanceIdAndMask = 0xFFu << 24;
    RtBuildInputs tin{};
    tin.type = 0;
    tin.flags = 4;
    tin.numDescs = 1;
    RtPrebuildInfo tpi{};
    ((PrebuildFn)dv[63])(m.device5, &tin, &tpi);
    const D3D12_RESOURCE_FLAGS uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    m.blas = m.buffer(bpi.resultMax, D3D12_HEAP_TYPE_DEFAULT, (D3D12_RESOURCE_STATES)kStateAccelStructure, uavFlag);
    m.scratchB = m.buffer(bpi.scratch, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uavFlag);
    m.tlas = m.buffer(tpi.resultMax, D3D12_HEAP_TYPE_DEFAULT, (D3D12_RESOURCE_STATES)kStateAccelStructure, uavFlag);
    m.scratchT = m.buffer(tpi.scratch, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, uavFlag);
    if (!m.blas || !m.scratchB || !m.tlas || !m.scratchT) {
        error = "out of GPU memory (acceleration structure)";
        return false;
    }
    inst.blas = m.blas->GetGPUVirtualAddress();
    m.instances = m.upload(&inst, sizeof inst);

    if (!m.beginList()) {
        error = "command list reset failed";
        return false;
    }
    for (int l = 0; l < layers; ++l) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = m.texArray;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = (UINT)l;  // one mip: subresource = array slice
        src.pResource = m.texUpload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = l * layerBytes;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = ts;
        src.PlacedFootprint.Footprint.Height = ts;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = rowPitch;
        m.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER tb = transition(m.texArray, D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m.list->ResourceBarrier(1, &tb);
    void** lv = *(void***)m.list4;
    RtBuildDesc bd{};
    bd.dest = m.blas->GetGPUVirtualAddress();
    bd.inputs = bin;
    bd.scratch = m.scratchB->GetGPUVirtualAddress();
    ((BuildFn)lv[72])(m.list4, &bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER ub = uavBarrier(m.blas);
    m.list->ResourceBarrier(1, &ub);
    tin.instanceDescs = m.instances->GetGPUVirtualAddress();
    RtBuildDesc tdsc{};
    tdsc.dest = m.tlas->GetGPUVirtualAddress();
    tdsc.inputs = tin;
    tdsc.scratch = m.scratchT->GetGPUVirtualAddress();
    ((BuildFn)lv[72])(m.list4, &tdsc, 0, nullptr);
    ub = uavBarrier(m.tlas);
    m.list->ResourceBarrier(1, &ub);
    m.submit();
    m.waitIdle();  // build once, before tracing
    m.inFlight = false;

    m.view = view;
    m.exposure = view.exposure;
    m.w = w;
    m.h = h;
    m.target = targetSamples;
    m.samples = m.row = 0;
    m.rowsTotal = 0;
    m.rowsPerSubmit = std::max(1, std::min(h, 8));
    m.image.assign((size_t)w * h * 4, 0);
    m.imageDirty = true;
    m.readbackPending = false;
    m.running = m.resumable = true;
    m.startTime = rt::nowSeconds();
    m.endTime = 0;
    m.lastResolve = 0;
    return true;
}

void HwRayTracer::stop() {
    Impl& m = *impl_;
    m.running = false;
    m.resumable = false;
}

void HwRayTracer::setTargetSamples(int n) {
    Impl& m = *impl_;
    m.target = n;
    if (!m.running && m.resumable && m.samples < n) m.running = true;
}

void HwRayTracer::step(double budgetMs) {
    Impl& m = *impl_;
    if (!ready() || !m.accum) return;
    if (m.inFlight) {
        if (m.fence->GetCompletedValue() < m.fenceValue) return;  // GPU still busy: never block the UI
        m.inFlight = false;
        m.lastGpuMs = (rt::nowSeconds() - m.submitTime) * 1000.0;
        // Adapt the work per submission towards the GPU-time budget.
        if (m.lastGpuMs < budgetMs * 0.6) m.rowsPerSubmit = std::min(m.h * 64, m.rowsPerSubmit * 3 / 2 + 1);
        else if (m.lastGpuMs > budgetMs * 1.2) m.rowsPerSubmit = std::max(1, m.rowsPerSubmit * 2 / 3);
        if (m.readbackPending) {
            void* p = nullptr;
            D3D12_RANGE all{0, (SIZE_T)m.w * m.h * 4};
            if (SUCCEEDED(m.readback->Map(0, &all, &p))) {
                std::memcpy(m.image.data(), p, (size_t)m.w * m.h * 4);
                D3D12_RANGE none{0, 0};
                m.readback->Unmap(0, &none);
                m.imageDirty = true;
            }
            m.readbackPending = false;
        }
    }
    const bool finishedNow = !m.running;
    if (finishedNow && !m.readbackPending && m.samples > 0 && m.endTime != 0 && m.lastResolve >= m.endTime) return;
    if (!m.beginList()) return;
    ID3D12DescriptorHeap* heaps[] = {m.heap};
    m.list->SetDescriptorHeaps(1, heaps);
    m.list->SetComputeRootSignature(m.rootSig);
    m.list->SetComputeRootConstantBufferView(0, m.params->GetGPUVirtualAddress());
    m.list->SetComputeRootShaderResourceView(2, m.tlas->GetGPUVirtualAddress());
    m.list->SetComputeRootDescriptorTable(3, m.gpuHandle(0));
    int budgetRows = m.running ? m.rowsPerSubmit : 0;
    int drawn = 0;
    if (budgetRows > 0) m.list->SetPipelineState(m.tracePso);
    while (budgetRows > 0 && m.samples < m.target) {
        int rows = std::min(budgetRows, m.h - m.row);
        UINT c[4] = {(UINT)m.row, (UINT)rows, (UINT)m.samples * 2654435761u + 12345u, m.samples == 0 ? 1u : 0u};
        m.list->SetComputeRoot32BitConstants(1, 4, c, 0);
        m.list->Dispatch((m.w + 7) / 8, (rows + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b = uavBarrier(m.accum);
        m.list->ResourceBarrier(1, &b);
        m.row += rows;
        drawn += rows;
        budgetRows -= rows;
        if (m.row >= m.h) {
            m.row = 0;
            ++m.samples;
        }
    }
    m.rowsTotal += drawn;
    if (m.samples >= m.target && m.running) {
        m.running = false;
        m.endTime = rt::nowSeconds();
    }
    // A few times a second (and at the end): tone-map on the GPU and copy back.
    const double now = rt::nowSeconds();
    if (m.samples > 0 && (now - m.lastResolve > 0.1 || !m.running)) {
        m.list->SetPipelineState(m.resolvePso);
        m.list->Dispatch((m.w + 7) / 8, (m.h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER b[2] = {uavBarrier(m.outBuf),
                                       transition(m.outBuf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_COPY_SOURCE)};
        m.list->ResourceBarrier(2, b);
        m.list->CopyBufferRegion(m.readback, 0, m.outBuf, 0, (UINT64)m.w * m.h * 4);
        D3D12_RESOURCE_BARRIER back = transition(m.outBuf, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.list->ResourceBarrier(1, &back);
        m.readbackPending = true;
        m.lastResolve = now;
    }
    if (drawn == 0 && !m.readbackPending) {
        m.list->Close();
        return;
    }
    m.submit();
}

void HwRayTracer::finish() {
    Impl& m = *impl_;
    for (int guard = 0; guard < 100000 && ready(); ++guard) {
        m.waitIdle();
        step(1e9);
        if (!m.running && !m.inFlight && !m.readbackPending) break;
    }
    m.waitIdle();
    step(1e9);  // collect the last readback
}

bool HwRayTracer::running() const { return impl_->running; }
int HwRayTracer::samples() const { return impl_->samples; }
int HwRayTracer::width() const { return impl_->w; }
int HwRayTracer::height() const { return impl_->h; }
double HwRayTracer::elapsedSeconds() const {
    const Impl& m = *impl_;
    if (m.startTime == 0) return 0;
    return (m.running || m.endTime == 0 ? rt::nowSeconds() : m.endTime) - m.startTime;
}
double HwRayTracer::samplesPerSecond() const {
    double t = elapsedSeconds();
    return t > 0 ? impl_->rowsTotal * impl_->w / t : 0.0;
}
double HwRayTracer::gpuMsPerSubmit() const { return impl_->lastGpuMs; }
bool HwRayTracer::takeImage(std::vector<uint8_t>& rgba) {
    Impl& m = *impl_;
    if (!m.imageDirty) return false;
    m.imageDirty = false;
    rgba = m.image;
    return true;
}

#endif  // _WIN32
