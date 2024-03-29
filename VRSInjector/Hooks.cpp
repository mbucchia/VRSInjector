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
#include "Injector.h"
#include "Tracing.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

namespace {

    // From DXSample framework.
    void GetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapter, bool requestHighPerformanceAdapter) {
        *ppAdapter = nullptr;

        ComPtr<IDXGIAdapter1> adapter;

        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
            for (UINT adapterIndex = 0; SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                     adapterIndex,
                     requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                           : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                     IID_PPV_ARGS(&adapter)));
                 ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(
                        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    break;
                }
            }
        }

        if (adapter.Get() == nullptr) {
            for (UINT adapterIndex = 0; SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, &adapter)); ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(
                        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    break;
                }
            }
        }

        *ppAdapter = adapter.Detach();
    }

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

    std::unique_ptr<Injector::IInjectionManager> g_InjectionManager;

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

        // Invoke the hook after the state has been set on the command list.
        assert(g_InjectionManager);
        g_InjectionManager->OnSetViewports(pCommandList, NumViewports > 0 ? pViewports[0] : D3D12_VIEWPORT{});

        TraceLoggingWriteStop(local, "ID3D12GraphicsCommandList_RSSetViewports");
    }

    DECLARE_DETOUR_FUNCTION(void,
                            STDMETHODCALLTYPE,
                            ID3D12CommandQueue_ExecuteCommandLists,
                            ID3D12CommandQueue* pCommandQueue,
                            UINT NumCommandLists,
                            ID3D12CommandList* const* ppCommandLists) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "ID3D12CommandQueue_ExecuteCommandLists",
                               TLPArg(pCommandQueue, "CommandQueue"),
                               TLArg(NumCommandLists, "NumCommandLists"));

        if (IsTraceEnabled() && ppCommandLists) {
            for (UINT i = 0; i < NumCommandLists; i++) {
                TraceLoggingWriteTagged(
                    local, "ID3D12CommandQueue_ExecuteCommandLists", TLPArg(ppCommandLists[i], "pCommandList"));
            }
        }

        // Invoke the hook before the real execution, in order to inject Wait() commands if needed.
        assert(g_InjectionManager);
        std::vector<ID3D12CommandList*> commandLists(ppCommandLists, ppCommandLists + NumCommandLists);
        g_InjectionManager->OnExecuteCommandLists(pCommandQueue, commandLists);

        assert(original_ID3D12CommandQueue_ExecuteCommandLists);
        original_ID3D12CommandQueue_ExecuteCommandLists(pCommandQueue, NumCommandLists, ppCommandLists);

        TraceLoggingWriteStop(local, "ID3D12CommandQueue_ExecuteCommandLists");
    }

    DECLARE_DETOUR_FUNCTION(
        HRESULT, STDMETHODCALLTYPE, IDXGISwapChain_Present, IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local,
                               "IDXGISwapChain_Present",
                               TLPArg(pSwapChain, "SwapChain"),
                               TLArg(SyncInterval, "SyncInterval"),
                               TLArg(Flags, "Flags"));

        // Invoke the hook prior to presenting, in case we wish to enqueue more work before any v-sync.
        assert(g_InjectionManager);
        g_InjectionManager->OnFramePresent(pSwapChain);

        assert(original_IDXGISwapChain_Present);
        const HRESULT result = original_IDXGISwapChain_Present(pSwapChain, SyncInterval, Flags);

        TraceLoggingWriteStop(local, "IDXGISwapChain_Present", TLArg(result, "Result"));

        return result;
    }

} // namespace

namespace Injector {

    void InstallHooks(std::unique_ptr<IInjectionManager> Manager) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "InstallHooks");

        g_InjectionManager = std::move(Manager);

        ComPtr<IDXGIFactory2> dxgiFactory;
        CHECK_HRCMD(CreateDXGIFactory2(0, IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(dxgiFactory.Get(), &hardwareAdapter, true);

        // Hook to the command list's RSSetViewports(), where we will decide whether or not to inject VRS commands.
        ComPtr<ID3D12Device> device;
        CHECK_HRCMD(D3D12CreateDevice(
            hardwareAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12CommandAllocator> commandAllocator;
        CHECK_HRCMD(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(commandAllocator.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12GraphicsCommandList> commandList;
        CHECK_HRCMD(device->CreateCommandList(0,
                                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              commandAllocator.Get(),
                                              nullptr,
                                              IID_PPV_ARGS(commandList.ReleaseAndGetAddressOf())));

        TraceLoggingWriteTagged(local, "InstallHooks_Detour_RSViewports", TLPArg(commandList.Get(), "CommandList"));
        DetourMethodAttach(commandList.Get(),
                           21, // RSSetViewports()
                           hooked_ID3D12GraphicsCommandList_RSSetViewports,
                           original_ID3D12GraphicsCommandList_RSSetViewports);

        // Hook to the command queue's ExecuteCommandLists() in order to add synchronization between our command lists.
        D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
        commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> commandQueue;
        CHECK_HRCMD(device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(commandQueue.ReleaseAndGetAddressOf())));

        TraceLoggingWriteTagged(
            local, "InstallHooks_Detour_ExecuteCommandLists", TLPArg(commandQueue.Get(), "CommandQueue"));
        DetourMethodAttach(commandQueue.Get(),
                           10, // ExecuteCommandLists()
                           hooked_ID3D12CommandQueue_ExecuteCommandLists,
                           original_ID3D12CommandQueue_ExecuteCommandLists);

        // Hook to the swapchain presentation, where we will collect information on rendering.
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Width = 128;
        swapChainDesc.Height = 128;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> dxgiSwapchain;
        CHECK_HRCMD(dxgiFactory->CreateSwapChainForComposition(
            commandQueue.Get(), &swapChainDesc, nullptr, dxgiSwapchain.ReleaseAndGetAddressOf()));

        TraceLoggingWriteTagged(local, "InstallHooks_Detour_Preset", TLPArg(dxgiSwapchain.Get(), "DXGISwapchain"));
        DetourMethodAttach(dxgiSwapchain.Get(),
                           8, // Present()
                           hooked_IDXGISwapChain_Present,
                           original_IDXGISwapChain_Present);

        TraceLoggingWriteStop(local, "InstallHooks");
    }

} // namespace Injector
