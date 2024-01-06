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

#include "Injector.h"
#include "Tracing.h"

namespace Tracing {

    // {cbf3adcd-42b1-4c38-830b-95980af201f6}
    TRACELOGGING_DEFINE_PROVIDER(g_traceProvider,
                                 "VRSInjector",
                                 (0xcbf3adcd, 0x42b1, 0x4e38, 0x93, 0x0b, 0x95, 0x98, 0x0a, 0xf2, 0x01, 0xf6));
} // namespace Tracing

namespace {

    std::future<void> g_deferredHook;

    void DeferredHooking() {
        Injector::InstallHooks();
    }

} // namespace

// Detours require at least one exported symbol.
void __declspec(dllexport) dummy() {
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        TraceLoggingRegister(Tracing::g_traceProvider);
        TraceLoggingWrite(Tracing::g_traceProvider, "Hello");
        // We cannot create certain COM/D3D objects from DllMain - defer it to later.
        g_deferredHook = std::async(std::launch::async, [] { DeferredHooking(); });
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
