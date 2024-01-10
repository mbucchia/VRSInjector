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
#include "EyeGaze.h"
#include "Injector.h"
#include "Tracing.h"
#include "VRS.h"

namespace {

    using namespace Injector;
    using namespace EyeGaze;

    struct InjectionManager : IInjectionManager {
        struct Resolution {
            UINT Width{0};
            UINT Height{0};
        };

        struct RenderingContext {
            std::unique_ptr<VRS::ICommandManager> CommandManager;
            Resolution PresentResolution;
        };

        void OnSetViewports(ID3D12CommandList* pCommandList, const D3D12_VIEWPORT& Viewport) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "OnSetViewports", TLPArg(pCommandList, "CommandList"));

            ComPtr<ID3D12Device> device;
            CHECK_HRCMD(pCommandList->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

            auto it = m_Contexts.find(device.Get());
            if (it != m_Contexts.end()) {
                VRS::ICommandManager* const commandManager = it->second.CommandManager.get();
                const Resolution& presentResolution = it->second.PresentResolution;

                if (IsViewportEligible(presentResolution, Viewport)) {
                    // Update the eye gaze input as late as possible.
                    if (m_EyeGazeManager && !m_GazeUpdatedThisFrame) {
                        m_EyeGazeManager->Update();
                        m_GazeUpdatedThisFrame = true;
                    }

                    commandManager->Enable(pCommandList, Viewport, m_EyeGazeManager.get());
                } else {
                    commandManager->Disable(pCommandList);
                }
            }

            TraceLoggingWriteStop(local, "OnSetViewports");
        }

        void OnExecuteCommandLists(ID3D12CommandQueue* pCommandQueue,
                                   const std::vector<ID3D12CommandList*>& CommandLists) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "OnExecuteCommandLists", TLPArg(pCommandQueue, "CommandQueue"));

            ComPtr<ID3D12Device> device;
            CHECK_HRCMD(pCommandQueue->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

            auto it = m_Contexts.find(device.Get());
            if (it != m_Contexts.end()) {
                VRS::ICommandManager* const commandManager = it->second.CommandManager.get();

                commandManager->SyncQueue(pCommandQueue, CommandLists);
            }

            TraceLoggingWriteStop(local, "OnExecuteCommandLists");
        }

        void OnFramePresent(IDXGISwapChain* pSwapChain) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "OnFramePresent", TLPArg(pSwapChain, "SwapChain"));

            ComPtr<ID3D12Resource> buffer;
            const HRESULT result = pSwapChain->GetBuffer(0, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf()));
            if (SUCCEEDED(result)) {
                ComPtr<ID3D12Device> device;
                CHECK_HRCMD(buffer->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

                // Update the output resolution we should use for our heuristic.
                DXGI_SWAP_CHAIN_DESC swapChainDesc{};
                CHECK_HRCMD(pSwapChain->GetDesc(&swapChainDesc));

                auto it = m_Contexts.find(device.Get());
                if (it != m_Contexts.end()) {
                    TraceLoggingWriteTagged(local,
                                            "OnFramePresent_UpdatePresentResolution",
                                            TLPArg(device.Get(), "Device"),
                                            TLArg(swapChainDesc.BufferDesc.Width, "Width"),
                                            TLArg(swapChainDesc.BufferDesc.Height, "Height"));
                    it->second.PresentResolution = {swapChainDesc.BufferDesc.Width, swapChainDesc.BufferDesc.Height};

                    VRS::ICommandManager* const commandManager = it->second.CommandManager.get();
                    commandManager->Present();

                } else {
                    // First time we see this device, let's create a VRS command manager for it.
                    TraceLoggingWriteTagged(local,
                                            "OnFramePresent_CreateContext",
                                            TLPArg(device.Get(), "Device"),
                                            TLArg(swapChainDesc.BufferDesc.Width, "Width"),
                                            TLArg(swapChainDesc.BufferDesc.Height, "Height"));
                    RenderingContext newContext;
                    newContext.CommandManager = VRS::CreateCommandManager(device.Get());
                    newContext.PresentResolution = {swapChainDesc.BufferDesc.Width, swapChainDesc.BufferDesc.Height};
                    m_Contexts.insert_or_assign(device.Get(), std::move(newContext));
                }

                // Try to attach an eye gaze manager.
                ComPtr<IDXGISwapChain1> dxgiSwapchain1;
                HWND hwnd{};
                if (SUCCEEDED(pSwapChain->QueryInterface(dxgiSwapchain1.ReleaseAndGetAddressOf())) &&
                    SUCCEEDED(dxgiSwapchain1->GetHwnd(&hwnd))) {
                    TraceLoggingWriteTagged(local, "OnFramePresent_HasHWND", TLPArg(hwnd, "HWND"));

                    // TODO: An application may present to multiple windows. We need to implement a mechanism to avoid
                    // bouncing the tracker from a window to another, eg: use the window with the largest dimension, or
                    // with the focus.
                    if (!m_EyeGazeManager || m_EyeGazeManager->GetHwnd() != hwnd) {
                        m_EyeGazeManager = CreateTobiiEyeGazeManager(hwnd);
                    }
                    m_EyeGazeManagerAging = 0;
                    m_GazeUpdatedThisFrame = false;
                }

            } else {
                // This could just be a hybrid rendering app also using D3D11 for presentation. Log the error and move
                // on.
                TraceLoggingWriteTagged(local, "OnFramePresent_GetBuffer", TLArg(result, "Error"));
            }

            // Age the eye gaze manager and garbage-collect it when it is not being used.
            if (++m_EyeGazeManagerAging > 100) {
                m_EyeGazeManager.reset();
            }

            {
                static bool wasKeyPressed = false;
                const bool isKeyPressed =
                    GetAsyncKeyState(VK_MENU) < 0 && GetAsyncKeyState('F') < 0 && GetAsyncKeyState('R') < 0;
                if (isKeyPressed && !wasKeyPressed) {
                    m_Enabled = !m_Enabled;
                }
                wasKeyPressed = isKeyPressed;
            }

            TraceLoggingWriteStop(local, "OnFramePresent");
        }

        bool IsViewportEligible(const Resolution& PresentResolution, const D3D12_VIEWPORT& Viewport) const {
            if (!m_Enabled) {
                return false;
            }

            if (!Viewport.Width || !Viewport.Height) {
                return false;
            }

            const double targetAspectRatio = static_cast<double>(PresentResolution.Height) / PresentResolution.Width;
            const double viewportAspectRatio = static_cast<double>(Viewport.Height) / Viewport.Width;
            const double scaleOfTarget = static_cast<double>(Viewport.Width) / PresentResolution.Width;

            return std::abs(targetAspectRatio - viewportAspectRatio) < 0.0001 &&
                   scaleOfTarget >= 0.32; // DLSS/FSR "Ultra Performance" might render at 33% of the final resolution.
        }

        bool m_Enabled{true};
        std::unordered_map<ID3D12Device*, RenderingContext> m_Contexts;

        std::unique_ptr<IEyeGazeManager> m_EyeGazeManager;
        unsigned int m_EyeGazeManagerAging{0};
        bool m_GazeUpdatedThisFrame{false};
    };

} // namespace

namespace Injector {

    std::unique_ptr<IInjectionManager> CreateInjectionManager() {
        return std::make_unique<InjectionManager>();
    }

} // namespace Injector
