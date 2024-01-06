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

    struct CommandManager : ICommandManager {
        CommandManager(ID3D12Device* Device) : m_Device(Device) {
        }

        void Enable(ID3D12CommandList* pCommandList, const D3D12_VIEWPORT& Viewport) override {
        }

        void Disable(ID3D12CommandList* pCommandList) override {
        }

        ComPtr<ID3D12Device> m_Device;
    };

}

namespace VRS {

    std::unique_ptr<ICommandManager> CreateCommandManager(ID3D12Device* Device) {
        return std::make_unique<CommandManager>(Device);
    }
}
