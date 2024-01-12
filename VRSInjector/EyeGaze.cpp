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
#include "Tracing.h"

namespace {

    using namespace EyeGaze;

    // Retrieve eye gaze tracking data from a Tobii commercial sensor, such as the Tobii Eye Tracker 5.
    struct TobiiEyeGazeManager : IEyeGazeManager {
        struct GazeData {
            std::chrono::time_point<std::chrono::steady_clock> Timepoint;
            float GazeX;
            float GazeY;
            float Distance;
        };

        TobiiEyeGazeManager(const tobiiAPI& Api, HWND Hwnd) : m_Api(Api), m_Hwnd(Hwnd) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "TobiiCreate");

            m_Api.SetWindow(m_Hwnd);
            const bool started = m_Api.Start(false /* custom_thread */);
            m_Api.SubscribeToStream(TobiiSubscriptionUserPresence);
            m_Api.SubscribeToStream(TobiiSubscriptionFoveatedGaze);
            m_Api.SubscribeToStream(TobiiSubscriptionHeadTracking);

            TraceLoggingWriteStop(local,
                                  "TobiiCreate",
                                  TLArg(m_Api.IsInitialised(), "Initialized"),
                                  TLArg(started, "Started"),
                                  TLArg(m_Api.IsConnected(), "Connected"),
                                  TLArg(m_Api.IsReady(), "Ready"));
        }

        ~TobiiEyeGazeManager() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "TobiiDestroy");

            m_Api.Stop();

            TraceLoggingWriteStop(local, "TobiiDestroy");
        }

        void Update() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "TobiiUpdate");

            m_Api.Update();

            const enum UserPresence presence = m_Api.GetUserPresence();
            TraceLoggingWriteTagged(local,
                                    "TobiiUpdate",
                                    TLArg(m_Api.IsInitialised(), "Initialized"),
                                    TLArg(m_Api.IsConnected(), "Connected"),
                                    TLArg(m_Api.IsReady(), "Ready"),
                                    TLArg(presence == UserPresence::Present, "UserPresent"));
            if (presence == UserPresence::Present) {
                HeadPose* headPoses;
                int numHeadPoses = 0;
                m_Api.GetNewHeadPoses(&headPoses, &numHeadPoses);

                GazePoint* gazePoints;
                int numGazePoints = 0;
                m_Api.GetNewGazePoints(&gazePoints, &numGazePoints, UnitType::Normalized);

                TraceLoggingWriteTagged(
                    local, "TobiiUpdate", TLArg(numHeadPoses, "NumHeadPoses"), TLArg(numGazePoints, "NumGazePoints"));

                GazeData gazeData{};

                if (numHeadPoses) {
                    const auto& mostRecent = headPoses[numHeadPoses - 1];
                    m_LastDistance = std::sqrt(mostRecent.Position.X * mostRecent.Position.X +
                                               mostRecent.Position.Y * mostRecent.Position.Y +
                                               mostRecent.Position.Z * mostRecent.Position.Z);
                }

                // Update the cache if we have enough data.
                if (numGazePoints) {
                    const auto& mostRecent = gazePoints[numGazePoints - 1];
                    gazeData.GazeX = mostRecent.X;
                    gazeData.GazeY = mostRecent.Y;
                    gazeData.Distance = m_LastDistance;

                    gazeData.Timepoint = std::chrono::steady_clock::now();

                    m_GazeData = gazeData;
                }
            }

            TraceLoggingWriteStop(local, "TobiiUpdate");
        }

        bool GetGaze(float& X, float& Y, float& Distance) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "TobiiGetGaze");

            // Invalidate the latched gaze data when it is too old.
            if (m_GazeData && (std::chrono::steady_clock::now() - m_GazeData->Timepoint).count() >= 600'000'000) {
                m_GazeData.reset();
            }

            // Return the cached gaze data.
            if (m_GazeData) {
                X = m_GazeData->GazeX;
                Y = m_GazeData->GazeY;
                Distance = m_GazeData->Distance;
                TraceLoggingWriteStop(local, "TobiiGetGaze", TLArg(X), TLArg(Y), TLArg(Distance));
                return true;
            }
            TraceLoggingWriteStop(local, "TobiiGetGaze_NotAvailable");
            return false;
        }

        HWND GetHwnd() const override {
            return m_Hwnd;
        }

        const tobiiAPI m_Api;
        const HWND m_Hwnd;
        std::optional<GazeData> m_GazeData;
        float m_LastDistance{600.f};
    };

} // namespace

namespace EyeGaze {
    std::unique_ptr<IEyeGazeManager> CreateTobiiEyeGazeManager(HWND Hwnd) {
        tobiiAPI api{};
        if (!InitializeTobiiAPI(&api)) {
            TraceLoggingWrite(Tracing::g_traceProvider, "TobiiNotFound");
            return {};
        }
        return std::make_unique<TobiiEyeGazeManager>(api, Hwnd);
    }

} // namespace EyeGaze
