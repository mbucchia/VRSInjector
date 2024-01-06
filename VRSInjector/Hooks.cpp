// MIT License
//
// Copyright(c) 2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "Check.h"
#include "Tracing.h"

#pragma comment(lib, "d3d12.lib")

namespace {

#define DECLARE_DETOUR_FUNCTION(ReturnType, Callconv, FunctionName, ...)                                               \
    ReturnType(Callconv* original_##FunctionName)(##__VA_ARGS__) = nullptr;                                            \
    ReturnType Callconv hooked_##FunctionName(##__VA_ARGS__)

    template <class T, typename TMethod>
    void DetourMethodAttach(T* instance, unsigned int methodOffset, TMethod hooked, TMethod& original) {
        if (original) {
            // Already hooked.
            return;
        }

        LPVOID* vtable = *((LPVOID**)instance);
        LPVOID target = vtable[methodOffset];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        original = (TMethod)target;
        DetourAttach((PVOID*)&original, hooked);

        DetourTransactionCommit();
    }

    DECLARE_DETOUR_FUNCTION(void,
                            STDMETHODCALLTYPE,
                            ID3D12GraphicsCommandList_RSSetViewports,
                            ID3D12GraphicsCommandList* pCommandList,
                            UINT NumViewports,
                            const D3D12_VIEWPORT* pViewports) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "ID3D12GraphicsCommandList_RSSetViewports",
                               TLPArg(pCommandList, "CommandList"),
                               TLArg(NumViewports, "NumViewports"));

        if (IsTraceEnabled() && pViewports) {
            for (UINT i = 0; i < NumViewports; i++) {
                TraceLoggingWriteTagged(local,
                                        "ID3D12GraphicsCommandList_RSSetViewports",
                                        TLArg(i, "ViewportIndex"),
                                        TLArg(pViewports[i].TopLeftX, "TopLeftX"),
                                        TLArg(pViewports[i].TopLeftY, "TopLeftY"),
                                        TLArg(pViewports[i].Width, "Width"),
                                        TLArg(pViewports[i].Height, "Height"));
            }
        }

        assert(original_ID3D12GraphicsCommandList_RSSetViewports);
        original_ID3D12GraphicsCommandList_RSSetViewports(pCommandList, NumViewports, pViewports);

        TraceLoggingWriteStop(local, "ID3D12GraphicsCommandList_RSSetViewports");
    }

    DECLARE_DETOUR_FUNCTION(
        HRESULT, STDMETHODCALLTYPE, IDXGISwapChain_Present, IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "IDXGISwapChain_Present",
                               TLPArg(pSwapChain, "SwapChain"),
                               TLArg(SyncInterval, "SyncInterval"),
                               TLArg(Flags, "Flags"));

        assert(original_IDXGISwapChain_Present);
        const HRESULT result = original_IDXGISwapChain_Present(pSwapChain, SyncInterval, Flags);

        TraceLoggingWriteStop(local, "IDXGISwapChain_Present", TLArg(result, "Result"));

        return result;
    }

} // namespace

namespace Injector {

    void InstallHooks() {
        // Hook to the command list's RSSetViewports(), where we will decide whether or not to inject VRS commands.
        ComPtr<ID3D12Device> device;
        CHECK_HRCMD(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12CommandAllocator> commandAllocator;
        CHECK_HRCMD(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(commandAllocator.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12GraphicsCommandList> commandList;
        CHECK_HRCMD(device->CreateCommandList(1,
                                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              commandAllocator.Get(),
                                              nullptr,
                                              IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf())));

        DetourMethodAttach(commandList.Get(),
                           21, // RSSetViewports()
                           hooked_ID3D12GraphicsCommandList_RSSetViewports,
                           original_ID3D12GraphicsCommandList_RSSetViewports);

        // Hook to the swapchain presentation, where we will collect information on rendering.
        ComPtr<IDXGIFactory2> dxgiFactory;
        CHECK_HRCMD(CreateDXGIFactory2(0, IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));

        D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
        commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        commandQueueDesc.NodeMask = 1;
        ComPtr<ID3D12CommandQueue> commandQueue;
        CHECK_HRCMD(device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(commandQueue.ReleaseAndGetAddressOf())));

        DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
        swapchainDesc.BufferCount = 2;
        swapchainDesc.Width = 128;
        swapchainDesc.Height = 128;
        swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapchainDesc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> dxgiSwapchain;
        CHECK_HRCMD(dxgiFactory->CreateSwapChainForComposition(
            commandQueue.Get(), &swapchainDesc, nullptr, dxgiSwapchain.ReleaseAndGetAddressOf()));

        DetourMethodAttach(dxgiSwapchain.Get(),
                           8, // Present()
                           hooked_IDXGISwapChain_Present,
                           original_IDXGISwapChain_Present);
    }

} // namespace Injector
