#pragma once

#include <d3d12.h>
#include <dxgi.h>

void CaptureCommandQueue(ID3D12CommandQueue* queue);
void ClearCapturedCommandQueues();
void RenderSpinningCube(IDXGISwapChain* swapChain);
void ReleaseCubeResources(IDXGISwapChain* swapChain);
