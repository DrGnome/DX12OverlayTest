# DX12 Overlay Hook

A minimal Windows DLL that hooks `IDXGISwapChain::Present` and renders a
depth-tested, spinning colored cube over the target's back buffer.

## Build

Use a Visual Studio Developer PowerShell with CMake and the Windows SDK installed:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The resulting `DX12OverlayHook.dll` installs the hooks from a worker thread when
loaded into a process. It captures the target's direct command queue through
`ExecuteCommandLists`, draws immediately before `Present`, and releases its
back-buffer references before `ResizeBuffers`.

The CMake-generated Visual Studio solution also includes the
`DX12OverlayInjector` console target. Start the text application, then run:

```powershell
.\build\Release\DX12OverlayInjector.exe "DX12 Text Application"
```

The injector finds the top-level window by its exact title and loads
`DX12OverlayHook.dll` from the same directory as the injector (by default,
`build\Release`). To use a different DLL, pass its path as a third argument.
The title must match exactly and identify only one process; duplicate matching
windows are rejected instead of selecting an arbitrary instance. The injector
requires permission to open the target process and reports that the DLL loaded;
the hook's initialization runs asynchronously in the target.

## DX12 text application

The `DX12TextApp` target is a standalone 64-bit DX12 window with DirectWrite
text rendered through a D3D12 texture. It is also a simple target for exercising
the injected cube hook.

```powershell
.\build\Release\DX12TextApp.exe
```

Inject the hook by window title using the command above. The app and injector
must both be built for x64.

## Project layout

- `src\` — Present/queue hooks and the cube renderer
- `injector\` — command-line DLL injector
- `text_app\` — standalone DX12 text-rendering sample
- `build\` — generated Visual Studio solution and build outputs

The exported `InstallDx12PresentHook` and `RemoveDx12PresentHook` functions are
also available for explicit control. Keep the DLL loaded for the lifetime of the
target process; removal is not synchronized with in-flight hook calls.

## Current limitations

- This basic implementation renders to one swap chain at a time.
- The swap chain and command queue must use the same D3D12 device, and the target
  must submit work through a direct command queue.
- The temporary DXGI/D3D12 objects used to find vtables are expected to share
  their implementation vtables with the target objects.
- The DLL must match the target process architecture.
