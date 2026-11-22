// probe_runner: draws one full-screen triangle with a given pixel shader into a 1x1
// R32G32B32A32_UINT render target and prints the four raw 32-bit words it wrote.
//
// Usage:
//   probe_runner.exe --backend hardware|warp [--adapter <name substring>]
//                    --vs <vs.cso> --ps <ps.cso> [--constants <hex>,<hex>,...]
//
// The constants (up to 64 uint32 words, zero padded to 256 bytes) are bound as a root
// CBV at b0. Output is line oriented, "key=value", so a script can parse it:
//   adapter=<name>  vendor=<hex>  device=<hex>  driver=<a.b.c.d>  software=<0|1>
//   words=<hex8> <hex8> <hex8> <hex8>
// Exit codes: 0 success, 2 bad arguments, 3 D3D12 failure (the failing step and HRESULT
// are printed as error=...; e.g. a driver refusing the pipeline state for one build).
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Roan System Labs

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string backend = "hardware";
    std::wstring adapter_filter;
    std::string vs_path;
    std::string ps_path;
    std::vector<uint32_t> constants;
};

[[noreturn]] void fail(const char* step, HRESULT hr) {
    std::printf("error=%s hr=0x%08lx\n", step, static_cast<unsigned long>(hr));
    std::fflush(stdout);
    std::exit(3);
}

void check(HRESULT hr, const char* step) {
    if (FAILED(hr)) fail(step, hr);
}

[[noreturn]] void usage(const char* message) {
    std::fprintf(stderr, "probe_runner: %s\n", message);
    std::fprintf(stderr,
                 "usage: probe_runner --backend hardware|warp [--adapter NAME] --vs VS.cso --ps PS.cso "
                 "[--constants HEX,HEX,...]\n");
    std::exit(2);
}

std::vector<char> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) usage(("cannot read " + path).c_str());
    return std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    wide.resize(static_cast<size_t>(size - 1));
    return wide;
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size - 1));
    return out;
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value for " + arg).c_str());
            return argv[++i];
        };
        if (arg == "--backend") {
            options.backend = value();
        } else if (arg == "--adapter") {
            options.adapter_filter = widen(value());
        } else if (arg == "--vs") {
            options.vs_path = value();
        } else if (arg == "--ps") {
            options.ps_path = value();
        } else if (arg == "--constants") {
            std::string list = value();
            size_t start = 0;
            while (start <= list.size()) {
                const size_t end = list.find(',', start);
                const std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!item.empty()) options.constants.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
                if (end == std::string::npos) break;
                start = end + 1;
            }
        } else {
            usage(("unknown argument " + arg).c_str());
        }
    }
    if (options.backend != "hardware" && options.backend != "warp") usage("backend must be hardware or warp");
    if (options.vs_path.empty() || options.ps_path.empty()) usage("--vs and --ps are required");
    if (options.constants.size() > 64) usage("at most 64 constant words");
    return options;
}

bool contains_case_insensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = haystack, n = needle;
    for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
    return h.find(n) != std::wstring::npos;
}

ComPtr<IDXGIAdapter1> pick_adapter(IDXGIFactory4* factory, const Options& options) {
    ComPtr<IDXGIAdapter1> adapter;
    if (options.backend == "warp") {
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        return adapter;
    }
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!software && contains_case_insensitive(desc.Description, options.adapter_filter) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            return adapter;
        }
        adapter.Reset();
    }
    std::printf("error=no_matching_hardware_adapter hr=0x%08lx\n", static_cast<unsigned long>(DXGI_ERROR_NOT_FOUND));
    std::exit(3);
}

void print_adapter(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    LARGE_INTEGER umd{};
    std::string driver = "unknown";
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
        char text[64];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                      HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        driver = text;
    }
    std::printf("adapter=%s\n", narrow(desc.Description).c_str());
    std::printf("vendor=%04x\ndevice=%04x\n", desc.VendorId, desc.DeviceId);
    std::printf("driver=%s\n", driver.c_str());
    std::printf("software=%d\n", (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0);
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const std::vector<char> vs = read_file(options.vs_path);
    const std::vector<char> ps = read_file(options.ps_path);

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter = pick_adapter(factory.Get(), options);
    print_adapter(adapter.Get());

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_2};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))) {
        model.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    const bool has_options4 =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
    std::printf("shader_model=%d.%d\n", (model.HighestShaderModel >> 4) & 0xF, model.HighestShaderModel & 0xF);
    std::printf("native_16bit=%d\n", has_options4 && options4.Native16BitShaderOpsSupported ? 1 : 0);

    // Root signature: one root CBV at b0, visible to the pixel shader.
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ComPtr<ID3DBlob> root_blob, root_error;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &root_error),
          "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root_signature;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root_signature)),
          "CreateRootSignature");

    const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_signature.Get();
    pso_desc.VS = {vs.data(), vs.size()};
    pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");

    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};

    // Constants: 256 bytes (the CBV alignment), zero padded.
    ComPtr<ID3D12Resource> constants;
    const D3D12_RESOURCE_DESC constants_desc = buffer_desc(256);
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &constants_desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&constants)),
          "CreateCommittedResource(constants)");
    void* mapped = nullptr;
    check(constants->Map(0, nullptr, &mapped), "Map(constants)");
    std::memset(mapped, 0, 256);
    if (!options.constants.empty()) std::memcpy(mapped, options.constants.data(), options.constants.size() * 4);
    constants->Unmap(0, nullptr);

    // 1x1 integer render target, cleared to a sentinel so an unwritten pixel is visible.
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = format;
    target_desc.SampleDesc.Count = 1;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    // An integer target converts the clear floats to integers: every channel becomes 0x00003039,
    // a value no probe row can produce, so a draw that never ran cannot pass as "flushed".
    const FLOAT sentinel[4] = {12345.0f, 12345.0f, 12345.0f, 12345.0f};
    for (int i = 0; i < 4; ++i) clear.Color[i] = sentinel[i];
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
          "CreateCommittedResource(target)");

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    check(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readback_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
    ComPtr<ID3D12Resource> readback;
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(readback_size);
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "CreateCommittedResource(readback)");

    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                    IID_PPV_ARGS(&list)),
          "CreateCommandList");

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, 1, 1};
    list->ClearRenderTargetView(rtv, sentinel, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    check(list->Close(), "Close");

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), 1), "Signal");
    check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    check(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");

    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
    check(readback->Map(0, &read_range, &mapped), "Map(readback)");
    uint32_t words[4];
    std::memcpy(words, static_cast<const uint8_t*>(mapped) + footprint.Offset, sizeof(words));
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);

    std::printf("words=%08x %08x %08x %08x\n", words[0], words[1], words[2], words[3]);
    return 0;
}
