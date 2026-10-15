#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "text_bitmap.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

constexpr UINT FrameCount = 2;
constexpr wchar_t WindowClassName[] = L"DX12TextAppWindow";
constexpr wchar_t WindowTitle[] = L"DX12 Text Application";

struct Vertex
{
    float position[2];
    float uv[2];
};

struct Frame
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> backBuffer;
    UINT64 fenceValue = 0;
};

class TextApplication
{
public:
    ~TextApplication()
    {
        WaitForGpu();
        if (m_fenceEvent)
        {
            CloseHandle(m_fenceEvent);
        }
    }

    int Run(HINSTANCE instance, int showCommand)
    {
        m_instance = instance;
        HRESULT result = CreateWindowAndDevice(showCommand);
        if (FAILED(result))
        {
            ShowFailure(L"Initializing the DX12 text application", result);
            return 1;
        }

        MSG message{};
        bool running = true;
        while (running)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (!running)
            {
                break;
            }

            if (m_resizeRequested)
            {
                result = Resize(m_pendingWidth, m_pendingHeight);
                if (FAILED(result))
                {
                    ShowFailure(L"Resizing the swap chain", result);
                    return 1;
                }
                m_resizeRequested = false;
            }

            if (m_width && m_height)
            {
                result = Render();
                if (FAILED(result))
                {
                    ShowFailure(L"Rendering a frame", result);
                    return 1;
                }
            }
            else
            {
                WaitMessage();
            }
        }

        return static_cast<int>(message.wParam);
    }

    static void ShowFailure(const wchar_t* operation, HRESULT result)
    {
        wchar_t message[256]{};
        swprintf_s(
            message, L"%s failed (HRESULT 0x%08X).",
            operation, static_cast<unsigned int>(result));
        OutputDebugStringW(message);
        MessageBoxW(nullptr, message, WindowTitle, MB_OK | MB_ICONERROR);
    }

private:
    static LRESULT CALLBACK WindowProcedure(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto* app = reinterpret_cast<TextApplication*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            app = static_cast<TextApplication*>(create->lpCreateParams);
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        if (app)
        {
            switch (message)
            {
            case WM_SIZE:
                app->m_pendingWidth = LOWORD(lparam);
                app->m_pendingHeight = HIWORD(lparam);
                app->m_resizeRequested = true;
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HRESULT CreateWindowAndDevice(int showCommand)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = m_instance;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = WindowClassName;
        if (!RegisterClassExW(&windowClass))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        RECT bounds{0, 0, 1280, 720};
        if (!AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        m_window = CreateWindowExW(
            0, WindowClassName, WindowTitle, WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, m_instance, this);
        if (!m_window)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        RECT clientBounds{};
        if (!GetClientRect(m_window, &clientBounds))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        m_width = static_cast<UINT>(clientBounds.right - clientBounds.left);
        m_height = static_cast<UINT>(clientBounds.bottom - clientBounds.top);
        ShowWindow(m_window, showCommand);
        UpdateWindow(m_window);

        HRESULT result = CreateDevice();
        if (FAILED(result))
        {
            return result;
        }
        result = CreateTextTexture();
        if (FAILED(result))
        {
            return result;
        }
        return CreateSwapChainResources();
    }

    HRESULT CreateDevice()
    {
        UINT factoryFlags = 0;
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        HRESULT result = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
        if (FAILED(result))
        {
            return result;
        }
        result = D3D12CreateDevice(
            nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = m_device->CreateCommandQueue(
            &queueDescription, IID_PPV_ARGS(&m_queue));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
        rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDescription.NumDescriptors = FrameCount;
        result = m_device->CreateDescriptorHeap(
            &rtvDescription, IID_PPV_ARGS(&m_rtvHeap));
        if (FAILED(result))
        {
            return result;
        }
        m_rtvDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        result = CreatePipeline();
        if (FAILED(result))
        {
            return result;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
        srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDescription.NumDescriptors = 1;
        srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = m_device->CreateDescriptorHeap(
            &srvDescription, IID_PPV_ARGS(&m_srvHeap));
        if (FAILED(result))
        {
            return result;
        }

        result = m_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
        if (FAILED(result))
        {
            return result;
        }
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
        {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
        }

        for (auto& frame : m_frames)
        {
            result = m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&frame.allocator));
            if (FAILED(result))
            {
                return result;
            }
        }
        result = m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_frames[0].allocator.Get(), nullptr,
            IID_PPV_ARGS(&m_commandList));
        if (FAILED(result))
        {
            return result;
        }
        return m_commandList->Close();
    }

    HRESULT CreatePipeline()
    {
        constexpr char shaderSource[] =
            "Texture2D textImage : register(t0);"
            "SamplerState textSampler : register(s0);"
            "struct VSInput { float2 position : POSITION; float2 uv : TEXCOORD; };"
            "struct PSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD; };"
            "PSInput VSMain(VSInput input) { PSInput output;"
            "output.position = float4(input.position, 0.0, 1.0);"
            "output.uv = input.uv; return output; }"
            "float4 PSMain(PSInput input) : SV_TARGET {"
            "return textImage.Sample(textSampler, input.uv); }";

        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        HRESULT result = D3DCompile(
            shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
            &vertexShader, nullptr);
        if (FAILED(result))
        {
            return result;
        }
        result = D3DCompile(
            shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0,
            &pixelShader, nullptr);
        if (FAILED(result))
        {
            return result;
        }

        D3D12_DESCRIPTOR_RANGE descriptorRange{};
        descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRange.NumDescriptors = 1;
        descriptorRange.BaseShaderRegister = 0;
        descriptorRange.OffsetInDescriptorsFromTableStart =
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER rootParameter{};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRange;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = 1;
        rootDescription.pParameters = &rootParameter;
        rootDescription.NumStaticSamplers = 1;
        rootDescription.pStaticSamplers = &sampler;
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
                OutputDebugStringA(
                    static_cast<const char*>(rootErrors->GetBufferPointer()));
            }
            return result;
        }
        result = m_device->CreateRootSignature(
            0, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature));
        if (FAILED(result))
        {
            return result;
        }

        constexpr Vertex vertices[] = {
            {{-0.82f, 0.48f}, {0.0f, 0.0f}},
            {{0.82f, 0.48f}, {1.0f, 0.0f}},
            {{-0.82f, -0.48f}, {0.0f, 1.0f}},
            {{0.82f, -0.48f}, {1.0f, 1.0f}},
        };
        const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
        pipelineDescription.pRootSignature = m_rootSignature.Get();
        pipelineDescription.VS = {
            vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipelineDescription.PS = {
            pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        pipelineDescription.BlendState.AlphaToCoverageEnable = FALSE;
        pipelineDescription.BlendState.IndependentBlendEnable = FALSE;
        const D3D12_RENDER_TARGET_BLEND_DESC blend = {
            TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA,
            D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA,
            D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP,
            D3D12_COLOR_WRITE_ENABLE_ALL};
        for (auto& target : pipelineDescription.BlendState.RenderTarget)
        {
            target = blend;
        }
        pipelineDescription.SampleMask = UINT_MAX;
        pipelineDescription.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
        pipelineDescription.DepthStencilState.DepthEnable = FALSE;
        pipelineDescription.DepthStencilState.StencilEnable = FALSE;
        pipelineDescription.InputLayout = {
            inputLayout, static_cast<UINT>(_countof(inputLayout))};
        pipelineDescription.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDescription.NumRenderTargets = 1;
        pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipelineDescription.SampleDesc.Count = 1;
        result = m_device->CreateGraphicsPipelineState(
            &pipelineDescription, IID_PPV_ARGS(&m_pipeline));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC vertexDescription{};
        vertexDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vertexDescription.Width = sizeof(vertices);
        vertexDescription.Height = 1;
        vertexDescription.DepthOrArraySize = 1;
        vertexDescription.MipLevels = 1;
        vertexDescription.SampleDesc.Count = 1;
        vertexDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_vertexBuffer));
        if (FAILED(result))
        {
            return result;
        }
        void* vertexData = nullptr;
        result = m_vertexBuffer->Map(0, nullptr, &vertexData);
        if (FAILED(result))
        {
            return result;
        }
        memcpy(vertexData, vertices, sizeof(vertices));
        m_vertexBuffer->Unmap(0, nullptr);
        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.SizeInBytes = sizeof(vertices);
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);
        return S_OK;
    }

    HRESULT CreateTextTexture()
    {
        TextBitmap bitmap;
        HRESULT result = CreateTextBitmap(bitmap);
        if (FAILED(result))
        {
            return result;
        }

        D3D12_RESOURCE_DESC textureDescription{};
        textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDescription.Width = bitmap.width;
        textureDescription.Height = bitmap.height;
        textureDescription.DepthOrArraySize = 1;
        textureDescription.MipLevels = 1;
        textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;
        result = m_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDescription,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_textTexture));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(
            &textureDescription, 0, 1, 0, &footprint, &rows,
            &rowSize, &uploadSize);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC uploadDescription{};
        uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDescription.Width = uploadSize;
        uploadDescription.Height = 1;
        uploadDescription.DepthOrArraySize = 1;
        uploadDescription.MipLevels = 1;
        uploadDescription.SampleDesc.Count = 1;
        uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_textureUpload));
        if (FAILED(result))
        {
            return result;
        }

        std::uint8_t* mapped = nullptr;
        result = m_textureUpload->Map(
            0, nullptr, reinterpret_cast<void**>(&mapped));
        if (FAILED(result))
        {
            return result;
        }
        for (UINT row = 0; row < bitmap.height; ++row)
        {
            memcpy(mapped + footprint.Footprint.RowPitch * row,
                   bitmap.pixels.data() + bitmap.rowPitch * row,
                   bitmap.rowPitch);
        }
        m_textureUpload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> uploadAllocator;
        result = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAllocator));
        if (FAILED(result))
        {
            return result;
        }
        ComPtr<ID3D12GraphicsCommandList> uploadList;
        result = m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator.Get(),
            nullptr, IID_PPV_ARGS(&uploadList));
        if (FAILED(result))
        {
            return result;
        }

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_textTexture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = m_textureUpload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        uploadList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_textTexture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        uploadList->ResourceBarrier(1, &barrier);
        result = uploadList->Close();
        if (FAILED(result))
        {
            return result;
        }
        ID3D12CommandList* commandLists[] = {uploadList.Get()};
        m_queue->ExecuteCommandLists(1, commandLists);
        result = WaitForGpu();
        if (FAILED(result))
        {
            return result;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        viewDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        viewDescription.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(
            m_textTexture.Get(), &viewDescription,
            m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        return S_OK;
    }

    HRESULT CreateSwapChainResources()
    {
        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = m_width;
        swapDescription.Height = m_height;
        swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = FrameCount;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> swapChain;
        HRESULT result = m_factory->CreateSwapChainForHwnd(
            m_queue.Get(), m_window, &swapDescription, nullptr, nullptr,
            &swapChain);
        if (FAILED(result))
        {
            return result;
        }
        result = swapChain.As(&m_swapChain);
        if (FAILED(result))
        {
            return result;
        }
        result = m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(result))
        {
            return result;
        }

        return CreateBackBufferViews();
    }

    HRESULT CreateBackBufferViews()
    {
        auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < FrameCount; ++index)
        {
            HRESULT result = m_swapChain->GetBuffer(
                index, IID_PPV_ARGS(&m_frames[index].backBuffer));
            if (FAILED(result))
            {
                return result;
            }
            m_device->CreateRenderTargetView(
                m_frames[index].backBuffer.Get(), nullptr, handle);
            handle.ptr += m_rtvDescriptorSize;
        }
        return S_OK;
    }

    HRESULT Resize(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
        {
            m_width = 0;
            m_height = 0;
            return S_OK;
        }
        if (!m_swapChain)
        {
            m_width = width;
            m_height = height;
            return S_OK;
        }

        HRESULT result = WaitForGpu();
        if (FAILED(result))
        {
            return result;
        }
        for (auto& frame : m_frames)
        {
            frame.backBuffer.Reset();
            frame.fenceValue = 0;
        }
        result = m_swapChain->ResizeBuffers(
            FrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(result))
        {
            return result;
        }
        m_width = width;
        m_height = height;
        return CreateBackBufferViews();
    }

    HRESULT Render()
    {
        const UINT frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        Frame& frame = m_frames[frameIndex];
        HRESULT result = WaitForFrame(frame);
        if (FAILED(result))
        {
            return result;
        }
        result = frame.allocator->Reset();
        if (FAILED(result))
        {
            return result;
        }
        result = m_commandList->Reset(frame.allocator.Get(), m_pipeline.Get());
        if (FAILED(result))
        {
            return result;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.backBuffer.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex) * m_rtvDescriptorSize;
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        constexpr float background[] = {0.035f, 0.055f, 0.085f, 1.0f};
        m_commandList->ClearRenderTargetView(rtv, background, 0, nullptr);

        D3D12_VIEWPORT viewport{
            0.0f, 0.0f, static_cast<float>(m_width),
            static_cast<float>(m_height), 0.0f, 1.0f};
        D3D12_RECT scissor{
            0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_pipeline.Get());
        m_commandList->SetDescriptorHeaps(1, m_srvHeap.GetAddressOf());
        m_commandList->SetGraphicsRootDescriptorTable(
            0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        m_commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        m_commandList->DrawInstanced(4, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_commandList->ResourceBarrier(1, &barrier);
        result = m_commandList->Close();
        if (FAILED(result))
        {
            return result;
        }
        ID3D12CommandList* commandLists[] = {m_commandList.Get()};
        m_queue->ExecuteCommandLists(1, commandLists);

        result = m_swapChain->Present(1, 0);
        if (FAILED(result))
        {
            return result;
        }
        return SignalFrame(frame);
    }

    HRESULT WaitForFrame(Frame& frame)
    {
        if (frame.fenceValue == 0 ||
            m_fence->GetCompletedValue() >= frame.fenceValue)
        {
            return S_OK;
        }
        HRESULT result = m_fence->SetEventOnCompletion(
            frame.fenceValue, m_fenceEvent);
        if (FAILED(result))
        {
            return result;
        }
        return WaitForSingleObject(m_fenceEvent, INFINITE) == WAIT_OBJECT_0
            ? S_OK
            : HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT SignalFrame(Frame& frame)
    {
        const UINT64 value = ++m_nextFenceValue;
        const HRESULT result = m_queue->Signal(m_fence.Get(), value);
        if (SUCCEEDED(result))
        {
            frame.fenceValue = value;
        }
        return result;
    }

    HRESULT WaitForGpu()
    {
        if (!m_queue || !m_fence)
        {
            return S_OK;
        }
        const UINT64 value = ++m_nextFenceValue;
        HRESULT result = m_queue->Signal(m_fence.Get(), value);
        if (FAILED(result))
        {
            return result;
        }
        result = m_fence->SetEventOnCompletion(value, m_fenceEvent);
        if (FAILED(result))
        {
            return result;
        }
        return WaitForSingleObject(m_fenceEvent, INFINITE) == WAIT_OBJECT_0
            ? S_OK
            : HRESULT_FROM_WIN32(GetLastError());
    }

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_pendingWidth = 0;
    UINT m_pendingHeight = 0;
    bool m_resizeRequested = false;
    UINT m_rtvDescriptorSize = 0;
    UINT64 m_nextFenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    std::array<Frame, FrameCount> m_frames;
    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipeline;
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_textTexture;
    ComPtr<ID3D12Resource> m_textureUpload;
    ComPtr<ID3D12Fence> m_fence;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        TextApplication::ShowFailure(L"Initializing COM", comResult);
        return 1;
    }

    int result = 0;
    {
        TextApplication application;
        result = application.Run(instance, showCommand);
    }
    CoUninitialize();
    return result;
}
