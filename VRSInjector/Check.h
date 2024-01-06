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

#pragma once

namespace Check {

#define CHK_STRINGIFY(x) #x
#define TOSTRING(x) CHK_STRINGIFY(x)
#define FILE_AND_LINE __FILE__ ":" TOSTRING(__LINE__)

#define CHECK_HRCMD(cmd) Check::CheckHResult(cmd, #cmd, FILE_AND_LINE)

    [[noreturn]] inline void Throw(std::string failureMessage,
                                   const char* originator = nullptr,
                                   const char* sourceLocation = nullptr) {
        if (originator != nullptr) {
            failureMessage += fmt::format("\n    Origin: %s", originator);
        }
        if (sourceLocation != nullptr) {
            failureMessage += fmt::format("\n    Source: %s", sourceLocation);
        }

        throw std::logic_error(failureMessage);
    }

    [[noreturn]] inline void ThrowHResult(HRESULT hr,
                                          const char* originator = nullptr,
                                          const char* sourceLocation = nullptr) {
        Throw(fmt::format("HRESULT failure [%x]", hr), originator, sourceLocation);
    }

    inline HRESULT CheckHResult(HRESULT hr, const char* originator = nullptr, const char* sourceLocation = nullptr) {
        if (FAILED(hr)) {
            ThrowHResult(hr, originator, sourceLocation);
        }

        return hr;
    }

} // namespace Check
