#include "cube_renderer.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

struct Vertex
{
    float position[3];
    float color[3];
};

struct Constants
{
    float mvp[16];
};

struct QueueRecord
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};

struct Renderer
{
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<IDXGISwapChain3> swapChain3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> depthBuffer;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> constantsBuffer;
    std::vector<ComPtr<ID3D12Resource>> backBuffers;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    UINT rtvDescriptorSize = 0;
    UINT backBufferCount = 0;
    UINT width = 0;
    UINT height = 0;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    Constants* mappedConstants = nullptr;
};

std::recursive_mutex g_mutex;
std::vector<QueueRecord> g_queues;
Renderer g_renderer;
const auto g_startTime = std::chrono::steady_clock::now();

constexpr Vertex CubeVertices[] = {
    {{-1.0f, -1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{-1.0f, 1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{1.0f, 1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{-1.0f, -1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{1.0f, 1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{1.0f, -1.0f, -1.0f}, {1.0f, 0.2f, 0.2f}},
    {{-1.0f, -1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{-1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{-1.0f, -1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{1.0f, -1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 0.2f}},
    {{-1.0f, -1.0f, -1.0f}, {0.2f, 0.5f, 1.0f}},
    {{-1.0f, -1.0f, 1.0f}, {0.2f, 0.5f, 1.0f}},
    {{-1.0f, 1.0f, 1.0f}, {0.2f, 0.5f, 1.0f}},
    {{-1.0f, -1.0f, -1.0f}, {0.2f, 0.5f, 1.0f}},
    {{-1.0f, 1.0f, 1.0f}, {0.2f, 0.5f, 1.0f}},
    {{-1.0f, 1.0f, -1.0f}, {0.2f, 0.5f, 1.0f}},
    {{1.0f, -1.0f, -1.0f}, {1.0f, 0.8f, 0.2f}},
    {{1.0f, 1.0f, 1.0f}, {1.0f, 0.8f, 0.2f}},
    {{1.0f, -1.0f, 1.0f}, {1.0f, 0.8f, 0.2f}},
    {{1.0f, -1.0f, -1.0f}, {1.0f, 0.8f, 0.2f}},
    {{1.0f, 1.0f, -1.0f}, {1.0f, 0.8f, 0.2f}},
    {{1.0f, 1.0f, 1.0f}, {1.0f, 0.8f, 0.2f}},
    {{-1.0f, -1.0f, -1.0f}, {0.8f, 0.2f, 1.0f}},
    {{1.0f, -1.0f, 1.0f}, {0.8f, 0.2f, 1.0f}},
    {{-1.0f, -1.0f, 1.0f}, {0.8f, 0.2f, 1.0f}},
    {{-1.0f, -1.0f, -1.0f}, {0.8f, 0.2f, 1.0f}},
    {{1.0f, -1.0f, -1.0f}, {0.8f, 0.2f, 1.0f}},
    {{1.0f, -1.0f, 1.0f}, {0.8f, 0.2f, 1.0f}},
    {{-1.0f, 1.0f, -1.0f}, {0.2f, 1.0f, 1.0f}},
    {{-1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 1.0f}},
    {{1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 1.0f}},
    {{-1.0f, 1.0f, -1.0f}, {0.2f, 1.0f, 1.0f}},
    {{1.0f, 1.0f, 1.0f}, {0.2f, 1.0f, 1.0f}},
    {{1.0f, 1.0f, -1.0f}, {0.2f, 1.0f, 1.0f}},
};

void LogFailure(const wchar_t* operation, HRESULT result)
{
    wchar_t message[160]{};
    swprintf_s(message, L"DX12OverlayHook: %s failed (HRESULT 0x%08X).\n",
               operation, static_cast<unsigned int>(result));
    OutputDebugStringW(message);
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

HRESULT CompileShader(const char* source, const char* entry, const char* target,
                      ComPtr<ID3DBlob>& shader)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, strlen(source), nullptr, nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
    if (FAILED(result) && errors)
    {
        OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    }
    return result;
}

void ReleaseRenderer()
{
    if (g_renderer.mappedConstants && g_renderer.constantsBuffer)
    {
        g_renderer.constantsBuffer->Unmap(0, nullptr);
        g_renderer.mappedConstants = nullptr;
    }
    if (g_renderer.fenceEvent)
    {
        CloseHandle(g_renderer.fenceEvent);
        g_renderer.fenceEvent = nullptr;
    }
    g_renderer = Renderer{};
}

HRESULT WaitForRenderer()
{
    if (!g_renderer.fence || g_renderer.fenceValue == 0 ||
        g_renderer.fence->GetCompletedValue() >= g_renderer.fenceValue)
    {
        return S_OK;
    }

    HRESULT result = g_renderer.fence->SetEventOnCompletion(
        g_renderer.fenceValue, g_renderer.fenceEvent);
    if (FAILED(result))
    {
        return result;
    }

    const DWORD waitResult = WaitForSingleObject(g_renderer.fenceEvent, INFINITE);
    return waitResult == WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT CreateRenderer(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    HRESULT result = swapChain->GetDevice(IID_PPV_ARGS(&g_renderer.device));
    if (FAILED(result))
    {
        return result;
    }
    g_renderer.queue = queue;
    g_renderer.swapChain = swapChain;
    result = swapChain->QueryInterface(IID_PPV_ARGS(&g_renderer.swapChain3));
    if (FAILED(result))
    {
        return result;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    result = swapChain->GetDesc(&swapChainDescription);
    if (FAILED(result))
    {
        return result;
    }
    g_renderer.width = swapChainDescription.BufferDesc.Width;
    g_renderer.height = swapChainDescription.BufferDesc.Height;
    g_renderer.backBufferCount = swapChainDescription.BufferCount;
    if (g_renderer.width == 0 || g_renderer.height == 0 ||
        g_renderer.backBufferCount == 0)
    {
        return E_INVALIDARG;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = g_renderer.backBufferCount;
    result = g_renderer.device->CreateDescriptorHeap(
        &rtvDescription, IID_PPV_ARGS(&g_renderer.rtvHeap));
    if (FAILED(result))
    {
        return result;
    }
    g_renderer.rtvDescriptorSize =
        g_renderer.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    g_renderer.backBuffers.resize(g_renderer.backBufferCount);
    auto rtv = g_renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < g_renderer.backBufferCount; ++index)
    {
        result = swapChain->GetBuffer(index, IID_PPV_ARGS(&g_renderer.backBuffers[index]));
        if (FAILED(result))
        {
            return result;
        }
        g_renderer.device->CreateRenderTargetView(g_renderer.backBuffers[index].Get(), nullptr, rtv);
        rtv.ptr += g_renderer.rtvDescriptorSize;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
    dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDescription.NumDescriptors = 1;
    result = g_renderer.device->CreateDescriptorHeap(
        &dsvDescription, IID_PPV_ARGS(&g_renderer.dsvHeap));
    if (FAILED(result))
    {
        return result;
    }

    D3D12_RESOURCE_DESC depthDescription{};
    depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDescription.Width = g_renderer.width;
    depthDescription.Height = g_renderer.height;
    depthDescription.DepthOrArraySize = 1;
    depthDescription.MipLevels = 1;
    depthDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthDescription.SampleDesc.Count = 1;
    depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    result = g_renderer.device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE, &depthDescription,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
        IID_PPV_ARGS(&g_renderer.depthBuffer));
    if (FAILED(result))
    {
        return result;
    }
    g_renderer.device->CreateDepthStencilView(
        g_renderer.depthBuffer.Get(), nullptr,
        g_renderer.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    constexpr char shaderSource[] =
        "cbuffer Constants : register(b0) { row_major float4x4 mvp; };"
        "struct VSInput { float3 position : POSITION; float3 color : COLOR; };"
        "struct PSInput { float4 position : SV_POSITION; float3 color : COLOR; };"
        "PSInput VSMain(VSInput input) { PSInput output;"
        "output.position = mul(float4(input.position, 1.0), mvp);"
        "output.color = input.color; return output; }"
        "float4 PSMain(PSInput input) : SV_TARGET { return float4(input.color, 1.0); }";
    result = CompileShader(shaderSource, "VSMain", "vs_5_0", vertexShader);
    if (FAILED(result))
    {
        return result;
    }
    result = CompileShader(shaderSource, "PSMain", "ps_5_0", pixelShader);
    if (FAILED(result))
    {
        return result;
    }

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1;
    rootDescription.pParameters = &rootParameter;
    rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRoot;
    ComPtr<ID3DBlob> rootErrors;
    result = D3D12SerializeRootSignature(
        &rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRoot, &rootErrors);
    if (FAILED(result))
    {
        if (rootErrors)
        {
            OutputDebugStringA(static_cast<const char*>(rootErrors->GetBufferPointer()));
        }
        return result;
    }
    result = g_renderer.device->CreateRootSignature(
        0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
        IID_PPV_ARGS(&g_renderer.rootSignature));
    if (FAILED(result))
    {
        return result;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = g_renderer.rootSignature.Get();
    pipelineDescription.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pipelineDescription.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pipelineDescription.BlendState.AlphaToCoverageEnable = FALSE;
    pipelineDescription.BlendState.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultBlend = {
        FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    for (auto& target : pipelineDescription.BlendState.RenderTarget)
    {
        target = defaultBlend;
    }
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipelineDescription.RasterizerState.FrontCounterClockwise = FALSE;
    pipelineDescription.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDescription.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthEnable = TRUE;
    pipelineDescription.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipelineDescription.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipelineDescription.DepthStencilState.StencilEnable = FALSE;
    pipelineDescription.InputLayout = {inputLayout, static_cast<UINT>(_countof(inputLayout))};
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = swapChainDescription.BufferDesc.Format;
    pipelineDescription.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pipelineDescription.SampleDesc.Count = 1;
    result = g_renderer.device->CreateGraphicsPipelineState(
        &pipelineDescription, IID_PPV_ARGS(&g_renderer.pipeline));
    if (FAILED(result))
    {
        return result;
    }

    result = g_renderer.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_renderer.allocator));
    if (FAILED(result))
    {
        return result;
    }
    result = g_renderer.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_renderer.allocator.Get(),
        g_renderer.pipeline.Get(), IID_PPV_ARGS(&g_renderer.commandList));
    if (FAILED(result))
    {
        return result;
    }
    result = g_renderer.commandList->Close();
    if (FAILED(result))
    {
        return result;
    }

    D3D12_RESOURCE_DESC vertexDescription{};
    vertexDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vertexDescription.Width = sizeof(CubeVertices);
    vertexDescription.Height = 1;
    vertexDescription.DepthOrArraySize = 1;
    vertexDescription.MipLevels = 1;
    vertexDescription.SampleDesc.Count = 1;
    vertexDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    result = g_renderer.device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vertexDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_renderer.vertexBuffer));
    if (FAILED(result))
    {
        return result;
    }
    void* vertexData = nullptr;
    result = g_renderer.vertexBuffer->Map(0, nullptr, &vertexData);
    if (FAILED(result))
    {
        return result;
    }
    memcpy(vertexData, CubeVertices, sizeof(CubeVertices));
    g_renderer.vertexBuffer->Unmap(0, nullptr);
    g_renderer.vertexBufferView.BufferLocation =
        g_renderer.vertexBuffer->GetGPUVirtualAddress();
    g_renderer.vertexBufferView.SizeInBytes = sizeof(CubeVertices);
    g_renderer.vertexBufferView.StrideInBytes = sizeof(Vertex);

    vertexDescription.Width = 256;
    result = g_renderer.device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vertexDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_renderer.constantsBuffer));
    if (FAILED(result))
    {
        return result;
    }
    result = g_renderer.constantsBuffer->Map(
        0, nullptr, reinterpret_cast<void**>(&g_renderer.mappedConstants));
    if (FAILED(result))
    {
        return result;
    }

    result = g_renderer.device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderer.fence));
    if (FAILED(result))
    {
        return result;
    }
    g_renderer.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_renderer.fenceEvent)
    {
        const DWORD error = GetLastError();
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
    }

    g_renderer.viewport = {0.0f, 0.0f, static_cast<float>(g_renderer.width),
                           static_cast<float>(g_renderer.height), 0.0f, 1.0f};
    g_renderer.scissor = {0, 0, static_cast<LONG>(g_renderer.width),
                          static_cast<LONG>(g_renderer.height)};
    return S_OK;
}

void UpdateMvp(Constants& constants, float aspectRatio)
{
    const auto elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - g_startTime).count();
    const float angle = elapsed * 0.8f;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const float xCosine = std::cos(angle * 0.65f);
    const float xSine = std::sin(angle * 0.65f);

    const float rotation[] = {
        cosine, sine * xSine, -sine * xCosine, 0.0f,
        0.0f, xCosine, xSine, 0.0f,
        sine, -cosine * xSine, cosine * xCosine, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    const float yScale = 1.0f / std::tan(0.785398163f * 0.5f);
    const float xScale = yScale / aspectRatio;
    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 100.0f;
    const float projection[] = {
        xScale, 0.0f, 0.0f, 0.0f,
        0.0f, yScale, 0.0f, 0.0f,
        0.0f, 0.0f, farPlane / (farPlane - nearPlane), 1.0f,
        0.0f, 0.0f, -nearPlane * farPlane / (farPlane - nearPlane), 0.0f};
    const float translation[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 5.0f, 1.0f};

    float rotatedAndTranslated[16]{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int index = 0; index < 4; ++index)
            {
                rotatedAndTranslated[row * 4 + column] +=
                    rotation[row * 4 + index] * translation[index * 4 + column];
            }
        }
    }

    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            constants.mvp[row * 4 + column] = 0.0f;
            for (int index = 0; index < 4; ++index)
            {
                constants.mvp[row * 4 + column] +=
                    rotatedAndTranslated[row * 4 + index] * projection[index * 4 + column];
            }
        }
    }
}
} // namespace

void CaptureCommandQueue(ID3D12CommandQueue* queue)
{
    if (!queue)
    {
        return;
    }

    D3D12_COMMAND_QUEUE_DESC description = queue->GetDesc();
    if (description.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        return;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& record : g_queues)
    {
        if (record.device.Get() == device.Get())
        {
            record.queue = queue;
            return;
        }
    }

    g_queues.push_back({device, queue});
}

void ClearCapturedCommandQueues()
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_queues.clear();
}

void RenderSpinningCube(IDXGISwapChain* swapChain)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    ComPtr<ID3D12Device> device;
    HRESULT result = swapChain->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(result))
    {
        LogFailure(L"getting the swap-chain device", result);
        return;
    }

    ComPtr<ID3D12CommandQueue> queue;
    for (const auto& record : g_queues)
    {
        if (record.device.Get() == device.Get())
        {
            queue = record.queue;
            break;
        }
    }
    if (!queue)
    {
        return;
    }

    if (g_renderer.swapChain.Get() != swapChain)
    {
        if (g_renderer.fence && g_renderer.queue)
        {
            result = WaitForRenderer();
            if (FAILED(result))
            {
                LogFailure(L"waiting before changing swap chains", result);
                return;
            }
        }
        ReleaseRenderer();
        result = CreateRenderer(swapChain, queue.Get());
        if (FAILED(result))
        {
            LogFailure(L"initializing the cube renderer", result);
            ReleaseRenderer();
            return;
        }
    }

    result = WaitForRenderer();
    if (FAILED(result))
    {
        LogFailure(L"waiting for the previous cube frame", result);
        return;
    }
    result = g_renderer.allocator->Reset();
    if (FAILED(result))
    {
        LogFailure(L"resetting the command allocator", result);
        return;
    }
    result = g_renderer.commandList->Reset(
        g_renderer.allocator.Get(), g_renderer.pipeline.Get());
    if (FAILED(result))
    {
        LogFailure(L"resetting the command list", result);
        return;
    }

    const UINT bufferIndex = g_renderer.swapChain3->GetCurrentBackBufferIndex();
    if (bufferIndex >= g_renderer.backBuffers.size())
    {
        LogFailure(L"getting the current back-buffer index", E_FAIL);
        return;
    }
    auto rtv = g_renderer.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bufferIndex) * g_renderer.rtvDescriptorSize;
    const auto dsv = g_renderer.dsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderer.backBuffers[bufferIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_renderer.commandList->ResourceBarrier(1, &barrier);
    g_renderer.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    g_renderer.commandList->ClearDepthStencilView(
        dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g_renderer.commandList->RSSetViewports(1, &g_renderer.viewport);
    g_renderer.commandList->RSSetScissorRects(1, &g_renderer.scissor);
    g_renderer.commandList->SetGraphicsRootSignature(g_renderer.rootSignature.Get());
    g_renderer.commandList->SetPipelineState(g_renderer.pipeline.Get());
    g_renderer.commandList->SetGraphicsRootConstantBufferView(
        0, g_renderer.constantsBuffer->GetGPUVirtualAddress());
    UpdateMvp(*g_renderer.mappedConstants,
              static_cast<float>(g_renderer.width) / static_cast<float>(g_renderer.height));
    g_renderer.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_renderer.commandList->IASetVertexBuffers(0, 1, &g_renderer.vertexBufferView);
    g_renderer.commandList->DrawInstanced(_countof(CubeVertices), 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_renderer.commandList->ResourceBarrier(1, &barrier);
    result = g_renderer.commandList->Close();
    if (FAILED(result))
    {
        LogFailure(L"closing the command list", result);
        return;
    }
    ID3D12CommandList* commandLists[] = {g_renderer.commandList.Get()};
    queue->ExecuteCommandLists(1, commandLists);
    ++g_renderer.fenceValue;
    result = queue->Signal(g_renderer.fence.Get(), g_renderer.fenceValue);
    if (FAILED(result))
    {
        LogFailure(L"signaling the frame fence", result);
    }
    g_renderer.queue = queue;
}

void ReleaseCubeResources(IDXGISwapChain* swapChain)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (swapChain && g_renderer.swapChain.Get() != swapChain)
    {
        return;
    }

    const HRESULT result = WaitForRenderer();
    if (FAILED(result))
    {
        LogFailure(L"waiting before releasing swap-chain resources", result);
    }
    ReleaseRenderer();
}
