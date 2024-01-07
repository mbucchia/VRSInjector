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
#include "VRS.h"

namespace {

    using namespace VRS;

#define Align(value, pad_to) (((value) + (pad_to)-1) & ~((pad_to)-1))

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
        struct D3D12ReusableCommandList {
            ComPtr<ID3D12CommandAllocator> Allocator;
            ComPtr<ID3D12GraphicsCommandList> CommandList;
            uint64_t CompletedFenceValue{0};
        };

        // TODO: Implement aging of the entries.
        struct ShadingRateMap {
            ComPtr<ID3D12Resource> ShadingRateTexture;
            ComPtr<ID3D12Resource> ShadingRateUpload;
            uint64_t CompletedFenceValue{0};
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
            m_TileSize = options.ShadingRateImageTileSize;

            // Create a command queue where we will perform the generation of the shading rate textures.
            D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
            commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            commandQueueDesc.NodeMask = 1;
            CHECK_HRCMD(
                m_Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(m_CommandQueue.ReleaseAndGetAddressOf())));

            CHECK_HRCMD(m_Device->CreateFence(
                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_CommandListPoolFence.ReleaseAndGetAddressOf())));

            TraceLoggingWriteStop(local, "VRSCreate");
        }

        void Enable(ID3D12CommandList* pCommandList, const D3D12_VIEWPORT& Viewport) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "VRSEnable", TLPArg(pCommandList, "CommandList"));

            if (m_Device) {
                std::unique_lock lock(m_ShadingRateMapsMutex);

                const TiledResolution shadingRateMapResolution{
                    Align(static_cast<UINT>(Viewport.Width + DBL_EPSILON), m_TileSize) / m_TileSize,
                    Align(static_cast<UINT>(Viewport.Height + DBL_EPSILON), m_TileSize) / m_TileSize};

                auto it = m_ShadingRateMaps.find(shadingRateMapResolution);
                if (it != m_ShadingRateMaps.end() && IsCommandListCompleted(it->second.CompletedFenceValue)) {
                    ComPtr<ID3D12GraphicsCommandList5> vrsCommandList;
                    CHECK_HRCMD(pCommandList->QueryInterface(vrsCommandList.GetAddressOf()));

                    // Release the upload buffer.
                    it->second.ShadingRateUpload.Reset();

                    // RSSetShadingRate() function sets both the combiners and the per-drawcall shading rate.
                    // We set to 1X1 for all sources and all combiners to MAX, so that the coarsest wins (per-drawcall,
                    // per-primitive, VRS surface).
                    static const D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {
                        D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX};
                    vrsCommandList->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
                    vrsCommandList->RSSetShadingRateImage(it->second.ShadingRateTexture.Get());
                } else {
                    // Request the shading rate map to be generated for a future pass.
                    RequestShadingRateMap(shadingRateMapResolution);
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

        void RequestShadingRateMap(const TiledResolution& Resolution) {
            const int rowPitch = Align(Resolution.Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

            // TODO: Move this process to a compute shader.
            ShadingRateMap newShadingRateMap;
            const D3D12_HEAP_PROPERTIES defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
                DXGI_FORMAT_R8_UINT, Resolution.Width, Resolution.Height, 1 /* arraySize */, 1 /* mipLevels */);
            CHECK_HRCMD(m_Device->CreateCommittedResource(
                &defaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &textureDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(newShadingRateMap.ShadingRateTexture.ReleaseAndGetAddressOf())));

            // Create an upload buffer.
            const D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(rowPitch * Resolution.Height);
            CHECK_HRCMD(m_Device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &uploadDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(newShadingRateMap.ShadingRateUpload.ReleaseAndGetAddressOf())));

            // Generate the pattern and copy it to the upload buffer.
            std::vector<uint8_t> pattern;
            GenerateFoveationPattern(pattern,
                                     Resolution,
                                     rowPitch,
                                     Resolution.Width / 2,
                                     Resolution.Height / 2,
                                     0.5f,
                                     0.8f,
                                     1.f,
                                     D3D12_SHADING_RATE_1X1,
                                     D3D12_SHADING_RATE_2X2,
                                     D3D12_SHADING_RATE_4X4);
            {
                void* mappedBuffer = nullptr;
                newShadingRateMap.ShadingRateUpload->Map(0, nullptr, &mappedBuffer);
                memcpy(mappedBuffer, pattern.data(), pattern.size());
                newShadingRateMap.ShadingRateUpload->Unmap(0, nullptr);
            }

            D3D12ReusableCommandList commandList = GetCommandList();

            // Copy to the the shading rate texture.
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
            ZeroMemory(&footprint, sizeof(footprint));
            footprint.Footprint.Width = Resolution.Width;
            footprint.Footprint.Height = Resolution.Height;
            footprint.Footprint.Depth = 1;
            footprint.Footprint.RowPitch = rowPitch;
            footprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
            CD3DX12_TEXTURE_COPY_LOCATION src(newShadingRateMap.ShadingRateUpload.Get(), footprint);
            CD3DX12_TEXTURE_COPY_LOCATION dst(newShadingRateMap.ShadingRateTexture.Get(), 0);
            commandList.CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            // Transition to the correct state for use with VRS.
            const D3D12_RESOURCE_BARRIER barrier =
                CD3DX12_RESOURCE_BARRIER::Transition(newShadingRateMap.ShadingRateTexture.Get(),
                                                     D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
            commandList.CommandList->ResourceBarrier(1, &barrier);

            newShadingRateMap.CompletedFenceValue = SubmitCommandList(commandList);
            m_ShadingRateMaps.insert_or_assign(Resolution, std::move(newShadingRateMap));
        }

        void GenerateFoveationPattern(std::vector<uint8_t>& Pattern,
                                      const TiledResolution& Resolution,
                                      size_t RowPitch,
                                      UINT FoveaCenterX,
                                      UINT FoveaCenterY,
                                      float InnerRadius,
                                      float OuterRadius,
                                      float SemiMajorFactor,
                                      uint8_t InnerValue,
                                      uint8_t MiddleValue,
                                      uint8_t OuterValue) const {
            Pattern.resize(RowPitch * Resolution.Height);

            const UINT innerSemiMinor = static_cast<UINT>(Resolution.Height * InnerRadius / 2);
            const UINT innerSemiMajor = static_cast<UINT>(SemiMajorFactor * innerSemiMinor);
            const UINT outerSemiMinor = static_cast<UINT>(Resolution.Height * OuterRadius / 2);
            const UINT outerSemiMajor = static_cast<UINT>(SemiMajorFactor * outerSemiMinor);

            auto isInsideEllipsis = [](INT h, INT k, INT x, INT y, UINT a, UINT b) {
                return (pow((x - h), 2) / pow(a, 2)) + (pow((y - k), 2) / pow(b, 2));
            };

            for (UINT y = 0; y < Resolution.Height; y++) {
                for (UINT x = 0; x < Resolution.Width; x++) {
                    uint8_t rate = OuterValue;
                    if (isInsideEllipsis(FoveaCenterX, FoveaCenterY, x, y, innerSemiMajor, innerSemiMinor) < 1) {
                        rate = InnerValue;
                    } else if (isInsideEllipsis(FoveaCenterX, FoveaCenterY, x, y, outerSemiMajor, outerSemiMinor) < 1) {
                        rate = MiddleValue;
                    }

                    Pattern[y * RowPitch + x] = rate;
                }
            }
        }

        D3D12ReusableCommandList GetCommandList() {
            std::unique_lock lock(m_CommandListPoolMutex);

            if (m_AvailableCommandList.empty()) {
                // Recycle completed command lists.
                while (!m_PendingCommandList.empty() &&
                       IsCommandListCompleted(m_PendingCommandList.front().CompletedFenceValue)) {
                    m_AvailableCommandList.push_back(std::move(m_PendingCommandList.front()));
                    m_PendingCommandList.pop_front();
                }
            }

            D3D12ReusableCommandList commandList;
            if (m_AvailableCommandList.empty()) {
                // Allocate a new command list if needed.
                CHECK_HRCMD(m_Device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandList.Allocator.ReleaseAndGetAddressOf())));
                CHECK_HRCMD(
                    m_Device->CreateCommandList(0,
                                                D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                commandList.Allocator.Get(),
                                                nullptr,
                                                IID_PPV_ARGS(commandList.CommandList.ReleaseAndGetAddressOf())));
            } else {
                commandList = m_AvailableCommandList.front();
                m_AvailableCommandList.pop_front();

                // Reset the command list before reuse.
                CHECK_HRCMD(commandList.CommandList->Reset(commandList.Allocator.Get(), nullptr));
            }
            return commandList;
        }

        uint64_t SubmitCommandList(D3D12ReusableCommandList CommandList) {
            std::unique_lock lock(m_CommandListPoolMutex);

            CHECK_HRCMD(CommandList.CommandList->Close());
            m_CommandQueue->ExecuteCommandLists(
                1, reinterpret_cast<ID3D12CommandList**>(CommandList.CommandList.GetAddressOf()));
            CommandList.CompletedFenceValue = m_CommandListPoolFenceValue + 1;
            m_CommandQueue->Signal(m_CommandListPoolFence.Get(), CommandList.CompletedFenceValue);
            m_PendingCommandList.push_back(std::move(CommandList));

            return CommandList.CompletedFenceValue;
        }

        bool IsCommandListCompleted(uint64_t CompletedFenceValue) {
            return m_CommandListPoolFence->GetCompletedValue() >= CompletedFenceValue;
        }

        ComPtr<ID3D12Device> m_Device;
        ComPtr<ID3D12CommandQueue> m_CommandQueue;

        std::mutex m_ShadingRateMapsMutex;
        std::unordered_map<TiledResolution, ShadingRateMap, TiledResolution> m_ShadingRateMaps;
        UINT m_TileSize{0};

        std::mutex m_CommandListPoolMutex;
        std::deque<D3D12ReusableCommandList> m_AvailableCommandList;
        std::deque<D3D12ReusableCommandList> m_PendingCommandList;
        ComPtr<ID3D12Fence> m_CommandListPoolFence;
        uint64_t m_CommandListPoolFenceValue{0};
    };

} // namespace

namespace VRS {

    std::unique_ptr<ICommandManager> CreateCommandManager(ID3D12Device* Device) {
        return std::make_unique<CommandManager>(Device);
    }

} // namespace VRS
