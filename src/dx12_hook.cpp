#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "cube_renderer.h"

#include <cstddef>
#include <stdio.h>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;
using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteCommandListsFunction = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

constexpr std::size_t PresentVtableIndex = 8;
constexpr std::size_t ResizeBuffersVtableIndex = 13;
constexpr std::size_t ExecuteCommandListsVtableIndex = 10;
constexpr wchar_t DummyWindowClassName[] = L"DX12OverlayHookDummyWindow";

SRWLOCK g_hookLock = SRWLOCK_INIT;
void** g_presentVtable = nullptr;
void** g_commandQueueVtable = nullptr;
PresentFunction g_originalPresent = nullptr;
ResizeBuffersFunction g_originalResizeBuffers = nullptr;
ExecuteCommandListsFunction g_originalExecuteCommandLists = nullptr;

void LogFailure(const wchar_t* operation, HRESULT result)
{
    wchar_t message[160]{};
    swprintf_s(message, L"DX12OverlayHook: %s failed (HRESULT 0x%08X).\n",
               operation, static_cast<unsigned int>(result));
    OutputDebugStringW(message);
}

void LogFailure(const wchar_t* operation)
{
    std::wstring message = L"DX12OverlayHook: ";
    message += operation;
    message += L" failed.\n";
    OutputDebugStringW(message.c_str());
}

LRESULT CALLBACK DummyWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

class DummyWindow
{
public:
    HRESULT Create()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DummyWindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = DummyWindowClassName;

        if (RegisterClassExW(&windowClass) != 0)
        {
            m_registeredClass = true;
        }
        else
        {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS)
            {
                const HRESULT result = HRESULT_FROM_WIN32(
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
                LogFailure(L"RegisterClassExW", result);
                return result;
            }
        }

        m_window = CreateWindowExW(
            0, DummyWindowClassName, L"", WS_OVERLAPPED,
            0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (!m_window)
        {
            const DWORD error = GetLastError();
            const HRESULT result = HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            LogFailure(L"CreateWindowExW", result);
            Cleanup();
            return result;
        }

        return S_OK;
    }

    ~DummyWindow()
    {
        Cleanup();
    }

    HWND Get() const
    {
        return m_window;
    }

private:
    void Cleanup()
    {
        if (m_window)
        {
            DestroyWindow(m_window);
            m_window = nullptr;
        }

        if (m_registeredClass)
        {
            UnregisterClassW(DummyWindowClassName, GetModuleHandleW(nullptr));
            m_registeredClass = false;
        }
    }

    HWND m_window = nullptr;
    bool m_registeredClass = false;
};

HRESULT GetPresentVtables(void*** vtable, void*** commandQueueVtable)
{
    DummyWindow window;
    HRESULT result = window.Create();
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<IDXGIFactory4> factory;
    result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<ID3D12Device> device;
    result = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(result))
    {
        return result;
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ComPtr<ID3D12CommandQueue> commandQueue;
    result = device->CreateCommandQueue(
        &queueDescription, IID_PPV_ARGS(&commandQueue));
    if (FAILED(result))
    {
        return result;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Width = 1;
    swapChainDescription.Height = 1;
    swapChainDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = 2;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    result = factory->CreateSwapChainForHwnd(
        commandQueue.Get(), window.Get(), &swapChainDescription,
        nullptr, nullptr, &swapChain1);
    if (FAILED(result))
    {
        return result;
    }

    ComPtr<IDXGISwapChain> swapChain;
    result = swapChain1.As(&swapChain);
    if (FAILED(result))
    {
        return result;
    }

    *vtable = *reinterpret_cast<void***>(swapChain.Get());
    *commandQueueVtable = *reinterpret_cast<void***>(commandQueue.Get());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    RenderSpinningCube(swapChain);
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags)
{
    ReleaseCubeResources(swapChain);
    return g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commandLists)
{
    CaptureCommandQueue(queue);
    g_originalExecuteCommandLists(queue, count, commandLists);
}

bool ReplaceVtableEntry(void** entry, void* expected, void* replacement)
{
    DWORD oldProtection = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        LogFailure(L"VirtualProtect");
        return false;
    }

    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(entry), replacement, expected);

    DWORD ignoredProtection = 0;
    const BOOL protectionRestored =
        VirtualProtect(entry, sizeof(void*), oldProtection, &ignoredProtection);

    if (observed != expected)
    {
        LogFailure(L"swap-chain Present entry changed unexpectedly");
        return false;
    }

    if (!protectionRestored)
    {
        LogFailure(L"restoring Present entry page protection");
    }

    return true;
}
} // namespace

extern "C" __declspec(dllexport) BOOL InstallDx12PresentHook()
{
    AcquireSRWLockExclusive(&g_hookLock);

    if (g_presentVtable)
    {
        ReleaseSRWLockExclusive(&g_hookLock);
        return TRUE;
    }

    void** vtable = nullptr;
    void** commandQueueVtable = nullptr;
    const HRESULT result = GetPresentVtables(&vtable, &commandQueueVtable);
    if (FAILED(result))
    {
        LogFailure(L"creating a temporary D3D12 swap chain", result);
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }

    void** presentEntry = &vtable[PresentVtableIndex];
    void** resizeBuffersEntry = &vtable[ResizeBuffersVtableIndex];
    void** executeCommandListsEntry = &commandQueueVtable[ExecuteCommandListsVtableIndex];
    auto originalPresent = reinterpret_cast<PresentFunction>(*presentEntry);
    auto originalResizeBuffers = reinterpret_cast<ResizeBuffersFunction>(*resizeBuffersEntry);
    auto originalExecuteCommandLists =
        reinterpret_cast<ExecuteCommandListsFunction>(*executeCommandListsEntry);
    if (!originalPresent || !originalResizeBuffers || !originalExecuteCommandLists)
    {
        LogFailure(L"reading a DXGI or D3D12 vtable entry");
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }

    g_originalPresent = originalPresent;
    g_originalResizeBuffers = originalResizeBuffers;
    g_originalExecuteCommandLists = originalExecuteCommandLists;
    if (!ReplaceVtableEntry(
            executeCommandListsEntry,
            reinterpret_cast<void*>(originalExecuteCommandLists),
            reinterpret_cast<void*>(&HookedExecuteCommandLists)))
    {
        g_originalPresent = nullptr;
        g_originalResizeBuffers = nullptr;
        g_originalExecuteCommandLists = nullptr;
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }
    if (!ReplaceVtableEntry(
            resizeBuffersEntry,
            reinterpret_cast<void*>(originalResizeBuffers),
            reinterpret_cast<void*>(&HookedResizeBuffers)))
    {
        ReplaceVtableEntry(executeCommandListsEntry,
                           reinterpret_cast<void*>(&HookedExecuteCommandLists),
                           reinterpret_cast<void*>(originalExecuteCommandLists));
        g_originalPresent = nullptr;
        g_originalResizeBuffers = nullptr;
        g_originalExecuteCommandLists = nullptr;
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }
    if (!ReplaceVtableEntry(
            presentEntry,
            reinterpret_cast<void*>(originalPresent),
            reinterpret_cast<void*>(&HookedPresent)))
    {
        ReplaceVtableEntry(resizeBuffersEntry,
                           reinterpret_cast<void*>(&HookedResizeBuffers),
                           reinterpret_cast<void*>(originalResizeBuffers));
        ReplaceVtableEntry(executeCommandListsEntry,
                           reinterpret_cast<void*>(&HookedExecuteCommandLists),
                           reinterpret_cast<void*>(originalExecuteCommandLists));
        g_originalPresent = nullptr;
        g_originalResizeBuffers = nullptr;
        g_originalExecuteCommandLists = nullptr;
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }

    g_presentVtable = vtable;
    g_commandQueueVtable = commandQueueVtable;
    ReleaseSRWLockExclusive(&g_hookLock);
    OutputDebugStringW(L"DX12OverlayHook: Present hook installed.\n");
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL RemoveDx12PresentHook()
{
    AcquireSRWLockExclusive(&g_hookLock);

    if (!g_presentVtable)
    {
        ReleaseSRWLockExclusive(&g_hookLock);
        return TRUE;
    }

    void** presentEntry = &g_presentVtable[PresentVtableIndex];
    void** resizeBuffersEntry = &g_presentVtable[ResizeBuffersVtableIndex];
    void** executeCommandListsEntry =
        &g_commandQueueVtable[ExecuteCommandListsVtableIndex];
    if (!ReplaceVtableEntry(
            presentEntry,
            reinterpret_cast<void*>(&HookedPresent),
            reinterpret_cast<void*>(g_originalPresent)))
    {
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }

    if (!ReplaceVtableEntry(
            resizeBuffersEntry,
            reinterpret_cast<void*>(&HookedResizeBuffers),
            reinterpret_cast<void*>(g_originalResizeBuffers)))
    {
        ReplaceVtableEntry(presentEntry,
                           reinterpret_cast<void*>(g_originalPresent),
                           reinterpret_cast<void*>(&HookedPresent));
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }
    if (!ReplaceVtableEntry(
            executeCommandListsEntry,
            reinterpret_cast<void*>(&HookedExecuteCommandLists),
            reinterpret_cast<void*>(g_originalExecuteCommandLists)))
    {
        ReplaceVtableEntry(resizeBuffersEntry,
                           reinterpret_cast<void*>(g_originalResizeBuffers),
                           reinterpret_cast<void*>(&HookedResizeBuffers));
        ReplaceVtableEntry(presentEntry,
                           reinterpret_cast<void*>(g_originalPresent),
                           reinterpret_cast<void*>(&HookedPresent));
        ReleaseSRWLockExclusive(&g_hookLock);
        return FALSE;
    }

    ReleaseCubeResources(nullptr);
    ClearCapturedCommandQueues();
    g_presentVtable = nullptr;
    g_commandQueueVtable = nullptr;
    g_originalPresent = nullptr;
    g_originalResizeBuffers = nullptr;
    g_originalExecuteCommandLists = nullptr;
    ReleaseSRWLockExclusive(&g_hookLock);
    OutputDebugStringW(L"DX12OverlayHook: Present hook removed.\n");
    return TRUE;
}

namespace
{
DWORD WINAPI InstallHookThread(void*)
{
    InstallDx12PresentHook();
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, InstallHookThread, nullptr, 0, nullptr);
        if (!thread)
        {
            OutputDebugStringW(L"DX12OverlayHook: CreateThread failed.\n");
            return FALSE;
        }
        CloseHandle(thread);
    }

    return TRUE;
}
