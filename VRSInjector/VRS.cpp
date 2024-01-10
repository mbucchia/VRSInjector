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
#include "D3D12Utils.h"
#include "EyeGaze.h"
#include "Tracing.h"
#include "VRS.h"

#include <GenerateShadingRateMapCS.h>

namespace {

    using namespace VRS;
    using namespace D3D12Utils;

#define Align(value, pad_to) (((value) + (pad_to)-1) & ~((pad_to)-1))

    // We will use Root Constants to pass these values to the shader.
    struct GenerateShadingRateMapConstants {
        float CenterX;
        float CenterY;
        float InnerRing;
        float OuterRing;
        uint32_t Rate1x1;
        uint32_t RateMedium;
        uint32_t RateLow;
    };
    static_assert(!(sizeof(GenerateShadingRateMapConstants) % 4), "Constants size must be a multiple of 4 bytes");
    static_assert(sizeof(GenerateShadingRateMapConstants) / 4 < 64, "Maximum of 64 constants");

    struct TiledResolution {
        UINT Width;
        UINT Height;

        bool operator==(const TiledResolution& other) const {
            return (Width == other.Width && Height == other.Height);
        }

        size_t operator()(const TiledResolution& key) const {
            auto hash1 = std::hash<UINT>{}(key.Width);
            auto hash2 = std::hash<UINT>{}(key.Height);

            if (hash1 != hash2) {
                return hash1 ^ hash2;
            }

            // If hash1 == hash2, their XOR is zero.
            return hash1;
        }
    };

    struct CommandManager : ICommandManager {
        struct ShadingRateMap {
            uint64_t Generation{0};
            unsigned int Age{0};
            ComPtr<ID3D12Resource> ShadingRateTexture;
            D3D12_CPU_DESCRIPTOR_HANDLE UAV;
            D3D12_GPU_DESCRIPTOR_HANDLE UAVDescriptor;
            uint64_t CompletedFenceValue{0};
        };

        struct CommandListDependency {
            uint64_t FenceValue;
            unsigned int Age{0};
        };

        CommandManager(ID3D12Device* Device) : m_Device(Device) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "VRSCreate", TLPArg(Device, "Device"));

            // Check for support on this device.
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
            if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options))) ||
                options.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_2 ||
                options.ShadingRateImageTileSize < 2u) {
                TraceLoggingWriteTagged(local,
                                        "VRSDisable_NotSupported",
                                        TLArg((UINT)options.VariableShadingRateTier, "VariableShadingRateTier"),
                                        TLArg(options.ShadingRateImageTileSize, "ShadingRateImageTileSize"));
                return;
            }
            m_VRSTileSize = options.ShadingRateImageTileSize;

            // Create a command context where we will perform the generation of the shading rate textures.
            m_Context = std::make_unique<CommandContext>(m_Device.Get(), L"Shading Rate Map Creation");

            // Create resources for the GenerateShadingRateMap compute shader.
            D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
            D3D12_DESCRIPTOR_RANGE uavRange;
            CD3DX12_DESCRIPTOR_RANGE::Init(uavRange, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            D3D12_ROOT_PARAMETER rootParameters[2];
            CD3DX12_ROOT_PARAMETER::InitAsDescriptorTable(rootParameters[0], 1, &uavRange);
            CD3DX12_ROOT_PARAMETER::InitAsConstants(rootParameters[1], sizeof(GenerateShadingRateMapConstants) / 4, 0);
            rootSignatureDesc.pParameters = rootParameters;
            rootSignatureDesc.NumParameters = ARRAYSIZE(rootParameters);

            ComPtr<ID3DBlob> rootSignatureBlob;
            ComPtr<ID3DBlob> error;
            CHECK_MSG(SUCCEEDED(D3D12SerializeRootSignature(&rootSignatureDesc,
                                                            D3D_ROOT_SIGNATURE_VERSION_1,
                                                            rootSignatureBlob.GetAddressOf(),
                                                            error.GetAddressOf())),
                      (char*)error->GetBufferPointer());

            CHECK_HRCMD(m_Device->CreateRootSignature(0,
                                                      rootSignatureBlob->GetBufferPointer(),
                                                      rootSignatureBlob->GetBufferSize(),
                                                      IID_PPV_ARGS(m_GenerateRootSignature.ReleaseAndGetAddressOf())));
            m_GenerateRootSignature->SetName(L"GenerateShadingRateMapCS Root Signature");

            D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
            computeDesc.CS.pShaderBytecode = g_GenerateShadingRateMapCS;
            computeDesc.CS.BytecodeLength = ARRAYSIZE(g_GenerateShadingRateMapCS);
            computeDesc.pRootSignature = m_GenerateRootSignature.Get();
            CHECK_HRCMD(m_Device->CreateComputePipelineState(&computeDesc,
                                                             IID_PPV_ARGS(m_GeneratePSO.ReleaseAndGetAddressOf())));
            m_GeneratePSO->SetName(L"GenerateShadingRateMapCS PSO");

            // Create a descriptor heap for the UAVs for our shading rate textures.
            m_HeapForUAVs = std::make_unique<DescriptorHeap>(
                m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 128u, L"Shading Rate Map UAV");

            TraceLoggingWriteStop(local, "VRSCreate");
        }

        void Enable(ID3D12CommandList* pCommandList,
                    const D3D12_VIEWPORT& Viewport,
                    EyeGaze::IEyeGazeManager* eyeGazeManager = nullptr) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "VRSEnable", TLPArg(pCommandList, "CommandList"));

            if (m_Device) {
                const TiledResolution shadingRateMapResolution{
                    Align(static_cast<UINT>(Viewport.Width + DBL_EPSILON), m_VRSTileSize) / m_VRSTileSize,
                    Align(static_cast<UINT>(Viewport.Height + DBL_EPSILON), m_VRSTileSize) / m_VRSTileSize};
                TraceLoggingWriteTagged(local,
                                        "VRSEnable",
                                        TLArg(shadingRateMapResolution.Width, "TiledWidth"),
                                        TLArg(shadingRateMapResolution.Height, "TiledHeight"));

                float gazeX = 0.5f, gazeY = 0.5f, distance = 600.f /* mm */;
                const bool wasUsingEyeGaze = m_UsingEyeGaze;
                m_UsingEyeGaze = eyeGazeManager && eyeGazeManager->GetGaze(gazeX, gazeY, distance);
                // When eye gaze becomes unavailable, we revert to fixed foveation, and we need to perform one last
                // update of the shading rate map with the default values above.
                const bool isEyeGazeAvailable = m_UsingEyeGaze || wasUsingEyeGaze;
                const float scaleFactor = std::clamp(distance / 600.f, 0.1f, 1.5f);

                bool skipDependency = false;
                ShadingRateMap shadingRateMap{};
                {
                    std::unique_lock lock(m_ShadingRateMapsMutex);

                    auto it = m_ShadingRateMaps.find(shadingRateMapResolution);
                    if (it != m_ShadingRateMaps.end()) {
                        if (isEyeGazeAvailable) {
                            ShadingRateMap& updatableShadingRateMap = it->second;

                            if (updatableShadingRateMap.Generation != m_CurrentGeneration) {
                                UpdateShadingRateMap(
                                    shadingRateMapResolution, updatableShadingRateMap, gazeX, gazeY, scaleFactor);
                            }
                        }

                        it->second.Age = 0;
                        shadingRateMap = it->second;

                        // No need to create a dependency on the GPU.
                        skipDependency = m_Context->IsCommandListCompleted(shadingRateMap.CompletedFenceValue);

                        TraceLoggingWriteTagged(local, "VRSEnable_Reuse", TLArg(!skipDependency, "NeedDependency"));

                    } else {
                        // Request the shading rate map to be generated.
                        shadingRateMap = RequestShadingRateMap(shadingRateMapResolution, gazeX, gazeY, scaleFactor);
                    }

                    ComPtr<ID3D12GraphicsCommandList5> vrsCommandList;
                    CHECK_HRCMD(pCommandList->QueryInterface(vrsCommandList.GetAddressOf()));

                    // RSSetShadingRate() function sets both the combiners and the per-drawcall shading rate.
                    // We set to 1X1 for all sources and all combiners to MAX, so that the coarsest wins
                    // (per-drawcall, per-primitive, VRS surface).
                    static const D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {
                        D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX};
                    vrsCommandList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
                    vrsCommandList->RSSetShadingRateImage(shadingRateMap.ShadingRateTexture.Get());
                }

                if (!skipDependency) {
                    std::unique_lock lock(m_CommandListDependenciesMutex);

                    // Add a dependency for command list submission.
                    CommandListDependency dependency{};
                    dependency.FenceValue = shadingRateMap.CompletedFenceValue;
                    m_CommandListDependencies.insert_or_assign(pCommandList, std::move(dependency));
                }
            } else {
                TraceLoggingWriteTagged(local, "VRSEnable_NotSupported");
            }

            TraceLoggingWriteStop(local, "VRSEnable");
        }

        void Disable(ID3D12CommandList* pCommandList) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "VRSDisable", TLPArg(pCommandList, "CommandList"));

            if (m_Device) {
                ComPtr<ID3D12GraphicsCommandList5> vrsCommandList;
                CHECK_HRCMD(pCommandList->QueryInterface(vrsCommandList.GetAddressOf()));

                vrsCommandList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
                vrsCommandList->RSSetShadingRateImage(nullptr);
            } else {
                TraceLoggingWriteTagged(local, "VRSDisable_NotSupported");
            }

            TraceLoggingWriteStop(local, "VRSDisable");
        }

        void SyncQueue(ID3D12CommandQueue* pCommandQueue,
                       const std::vector<ID3D12CommandList*>& CommandLists) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "SyncQueue", TLPArg(pCommandQueue, "CommandQueue"));

            std::unique_lock lock(m_CommandListDependenciesMutex);

            for (const auto& commandList : CommandLists) {
                auto it = m_CommandListDependencies.find(commandList);
                if (it != m_CommandListDependencies.end()) {
                    const CommandListDependency& dependency = it->second;

                    // Insert a wait to ensure the shading rate map is ready for use.
                    TraceLoggingWriteTagged(local,
                                            "SyncQueue_Wait",
                                            TLPArg(commandList, "CommandList"),
                                            TLArg(dependency.FenceValue, "FenceValue"));
                    pCommandQueue->Wait(m_Context->GetCompletionFence(), dependency.FenceValue);

                    // Retire the dependency.
                    m_CommandListDependencies.erase(it);
                }
            }

            TraceLoggingWriteStop(local, "SyncQueue", TLPArg(pCommandQueue, "CommandQueue"));
        }

        void Present() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "VRSPresent");

            {
                std::unique_lock lock(m_ShadingRateMapsMutex);

                TraceLoggingWriteTagged(
                    local, "VRSPresent_Cleanup_ShadingRateMaps", TLArg(m_ShadingRateMaps.size(), "NumShadingRateMaps"));
                for (auto it = m_ShadingRateMaps.begin(); it != m_ShadingRateMaps.end();) {
                    // Age the unused masks and garbage-collect them.
                    if (++it->second.Age > 100) {
                        TraceLoggingWriteTagged(local,
                                                "VRSPresent_Cleanup_ShadingRateMaps",
                                                TLArg(it->first.Width, "TiledWidth"),
                                                TLArg(it->first.Height, "TiledHeight"));
                        it = m_ShadingRateMaps.erase(it);
                    } else {
                        it++;
                    }
                }
            }
            {
                std::unique_lock lock(m_CommandListDependenciesMutex);

                TraceLoggingWriteTagged(local,
                                        "VRSPresent_Cleanup_CommandListDependencies",
                                        TLArg(m_CommandListDependencies.size(), "NumCommandListDependencies"));
                for (auto it = m_CommandListDependencies.begin(); it != m_CommandListDependencies.end();) {
                    // Age the unused command list dependencies and garbage-collect them.
                    // An application may have started then abandoned a command list.
                    if (++it->second.Age > 100) {
                        TraceLoggingWriteTagged(local,
                                                "VRSPresent_Cleanup_CommandListDependencies",
                                                TLPArg(it->first, "CommandList"),
                                                TLArg(it->second.FenceValue, "FenceValue"));
                        it = m_CommandListDependencies.erase(it);
                    } else {
                        it++;
                    }
                }
            }

            m_CurrentGeneration++;

            TraceLoggingWriteStop(local, "VRSPresent", TLArg(m_CurrentGeneration, "CurrentGeneration"));
        }

        ShadingRateMap
        RequestShadingRateMap(const TiledResolution& Resolution, float CenterX, float CenterY, float ScaleFactor) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "VRSCreateShadingRateMap",
                                   TLArg(Resolution.Width, "TiledWidth"),
                                   TLArg(Resolution.Height, "TiledHeight"));

            const int rowPitch = Align(Resolution.Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

            ShadingRateMap newShadingRateMap;

            // Create the resources for the texture.
            const D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC textureDesc =
                CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8_UINT,
                                             Resolution.Width,
                                             Resolution.Height,
                                             1 /* arraySize */,
                                             1 /* mipLevels */,
                                             1 /* sampleCount */,
                                             0 /* sampleQuality */,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            CHECK_HRCMD(m_Device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &textureDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(newShadingRateMap.ShadingRateTexture.ReleaseAndGetAddressOf())));
            newShadingRateMap.ShadingRateTexture->SetName(L"Shading Rate Texture");

            newShadingRateMap.UAV = m_HeapForUAVs->AllocateDescriptor();
            newShadingRateMap.UAVDescriptor = m_HeapForUAVs->GetGPUDescriptor(newShadingRateMap.UAV);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Format = DXGI_FORMAT_R8_UINT;
            m_Device->CreateUnorderedAccessView(
                newShadingRateMap.ShadingRateTexture.Get(), nullptr, &uavDesc, newShadingRateMap.UAV);

            UpdateShadingRateMap(
                Resolution, newShadingRateMap, CenterX, CenterY, ScaleFactor, true /* IsFreshTexture */);

            m_ShadingRateMaps.insert_or_assign(Resolution, newShadingRateMap);

            TraceLoggingWriteStop(
                local, "VRSCreateShadingRateMap", TLArg(newShadingRateMap.CompletedFenceValue, "CompletedFenceValue"));

            return newShadingRateMap;
        }

        void UpdateShadingRateMap(const TiledResolution& Resolution,
                                  ShadingRateMap& ShadingRateMap,
                                  float CenterX,
                                  float CenterY,
                                  float ScaleFactor,
                                  bool IsFreshTexture = false) {
            // Prepare a command list.
            CommandList commandList = m_Context->GetCommandList();
            ID3D12DescriptorHeap* heaps[] = {m_HeapForUAVs->GetDescriptorHeap()};
            commandList.Commands->SetDescriptorHeaps(ARRAYSIZE(heaps), heaps);

            if (!IsFreshTexture) {
                // Transition to UAV state for the compute shader.
                const D3D12_RESOURCE_BARRIER barrier =
                    CD3DX12_RESOURCE_BARRIER::Transition(ShadingRateMap.ShadingRateTexture.Get(),
                                                         D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commandList.Commands->ResourceBarrier(1, &barrier);
            }

            // Dispatch the compute shader to generate the map.
            GenerateShadingRateMapConstants constants{};
            constants.CenterX = CenterX * Resolution.Width;
            constants.CenterY = CenterY * Resolution.Height;
            // TODO: Customize these.
            constants.InnerRing = 0.25f * Resolution.Height;
            constants.OuterRing = 0.8f * Resolution.Height;
            constants.Rate1x1 = D3D12_SHADING_RATE_1X1;
            constants.RateMedium = D3D12_SHADING_RATE_2X2;
            constants.RateLow = D3D12_SHADING_RATE_4X4;
            commandList.Commands->SetComputeRootSignature(m_GenerateRootSignature.Get());
            commandList.Commands->SetPipelineState(m_GeneratePSO.Get());
            commandList.Commands->SetComputeRootDescriptorTable(0, ShadingRateMap.UAVDescriptor);
            commandList.Commands->SetComputeRoot32BitConstants(
                1, sizeof(GenerateShadingRateMapConstants) / 4, &constants, 0);
            commandList.Commands->Dispatch(Align(Resolution.Width, 8) / 8, Align(Resolution.Height, 8) / 8, 1);

            // Transition to the correct state for use with VRS.
            const D3D12_RESOURCE_BARRIER barrier =
                CD3DX12_RESOURCE_BARRIER::Transition(ShadingRateMap.ShadingRateTexture.Get(),
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
            commandList.Commands->ResourceBarrier(1, &barrier);

            ShadingRateMap.CompletedFenceValue = m_Context->SubmitCommandList(commandList);
            ShadingRateMap.Generation = m_CurrentGeneration;
        }

        ComPtr<ID3D12Device> m_Device;
        UINT m_VRSTileSize{0};

        std::unique_ptr<CommandContext> m_Context;
        std::unique_ptr<DescriptorHeap> m_HeapForUAVs;

        ComPtr<ID3D12RootSignature> m_GenerateRootSignature;
        ComPtr<ID3D12PipelineState> m_GeneratePSO;

        std::mutex m_ShadingRateMapsMutex;
        std::unordered_map<TiledResolution, ShadingRateMap, TiledResolution> m_ShadingRateMaps;
        uint64_t m_CurrentGeneration{0};

        bool m_UsingEyeGaze{false};

        std::mutex m_CommandListDependenciesMutex;
        std::unordered_map<ID3D12CommandList*, CommandListDependency> m_CommandListDependencies;
    };

} // namespace

namespace VRS {

    std::unique_ptr<ICommandManager> CreateCommandManager(ID3D12Device* Device) {
        return std::make_unique<CommandManager>(Device);
    }

} // namespace VRS
