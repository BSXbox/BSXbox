#include <windows.h>
#include <unknwn.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <roapi.h>
#include <winstring.h>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/controller.hpp>
#include <shims/uwphost.hpp>

namespace bsx::uwp {
    namespace wf = winrt::Windows::Foundation;
    namespace wfc = winrt::Windows::Foundation::Collections;
    namespace wam = winrt::Windows::ApplicationModel;
    namespace wama = winrt::Windows::ApplicationModel::Activation;
    namespace wamc = winrt::Windows::ApplicationModel::Core;
    namespace ws = winrt::Windows::System;
    namespace wst = winrt::Windows::Storage;
    namespace wss = winrt::Windows::Storage::Streams;
    namespace wdi = winrt::Windows::Devices::Input;
    namespace wgd = winrt::Windows::Graphics::Display;
    namespace wuc = winrt::Windows::UI::Core;
    namespace wuv = winrt::Windows::UI::ViewManagement;

    struct CoreWindowImpl;
    wuc::CoreWindow coreWindow();
    wamc::CoreApplicationView coreApplicationView();
    void closeCoreWindow();

    namespace members {
        struct SdlMouseMotionEvent { uint32_t type, timestamp, windowId, which, state; int32_t x, y, xrel, yrel; };
        struct SdlMouseButtonEvent { uint32_t type, timestamp, windowId, which; uint8_t button, state, clicks, padding; int32_t x, y; };
        struct SdlMouseWheelEvent { uint32_t type, timestamp, windowId, which; int32_t x, y; uint32_t direction; float preciseX, preciseY; int32_t mouseX, mouseY; };
        struct SdlWindowEvent { uint32_t type, timestamp, windowId; uint8_t event, padding[3]; int32_t data1, data2; };
        union SdlEvent { uint32_t type; SdlMouseMotionEvent motion; SdlMouseButtonEvent button; SdlMouseWheelEvent wheel; SdlWindowEvent window; uint8_t padding[56]; };

        constexpr uint32_t sdlQuit = 0x100;
        constexpr uint32_t sdlWindowEvent = 0x200;
        constexpr uint32_t sdlMouseMotion = 0x400;
        constexpr uint32_t sdlMouseButtonDown = 0x401;
        constexpr uint32_t sdlMouseButtonUp = 0x402;
        constexpr uint32_t sdlMouseWheel = 0x403;
        constexpr uint32_t sdlWindowShown = 1;
        constexpr uint32_t sdlWindowFocusGained = 12;
        constexpr uint32_t sdlWindowFocusLost = 13;
        constexpr uint32_t sdlWindowId = 1;

        std::wstring m_installedPath;
        std::wstring m_localStatePath;
        std::atomic<HWND> m_hwnd{nullptr};
        CoreWindowImpl* m_coreWindow = nullptr;
        constexpr wchar_t m_windowClassName[] = L"BSXboxUWPHostWindow";
        constexpr int m_defaultWindowWidth = 1280;
        constexpr int m_defaultWindowHeight = 720;
        std::mutex m_windowMutex;
        std::condition_variable m_windowReady;
        bool m_windowStarted = false;

        void ensureWindow();

        void log(const char* format, ...) {
            va_list args;
            va_start(args, format);
            char message[1024] = {};
            std::vsnprintf(message, sizeof(message), format, args);
            va_end(args);
            bsx::logf("uwp", "%s", message);
            std::fflush(stderr);
        }

        bool pushSdlEvent(SdlEvent& event) {
            using PushEventFn = int (*)(SdlEvent*);
            static PushEventFn push = reinterpret_cast<PushEventFn>(bsx::findMappedExport("SDL2.dll", "SDL_PushEvent"));
            return push && push(&event) == 0;
        }

        uint32_t sdlMouseState(WPARAM wparam) {
            uint32_t state = 0;
            if (wparam & MK_LBUTTON) state |= 1;
            if (wparam & MK_RBUTTON) state |= 4;
            if (wparam & MK_MBUTTON) state |= 2;
            return state;
        }

        void pushSdlMouseMove(WPARAM wparam, int x, int y, int xrel, int yrel) {
            SdlEvent event{};
            event.motion.type = sdlMouseMotion;
            event.motion.windowId = sdlWindowId;
            event.motion.which = 0;
            event.motion.state = sdlMouseState(wparam);
            event.motion.x = x;
            event.motion.y = y;
            event.motion.xrel = xrel;
            event.motion.yrel = yrel;
            pushSdlEvent(event);
        }

        void pushSdlMouseButton(UINT message, int x, int y) {
            uint8_t button = 1;
            if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) button = 3;
            else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) button = 2;
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN;
            SdlEvent event{};
            event.button.type = down ? sdlMouseButtonDown : sdlMouseButtonUp;
            event.button.windowId = sdlWindowId;
            event.button.which = 0;
            event.button.button = button;
            event.button.state = down ? 1 : 0;
            event.button.clicks = 1;
            event.button.x = x;
            event.button.y = y;
            pushSdlEvent(event);
        }

        void pushSdlMouseWheel(int x, int y, short delta) {
            SdlEvent event{};
            event.wheel.type = sdlMouseWheel;
            event.wheel.windowId = sdlWindowId;
            event.wheel.which = 0;
            event.wheel.x = x;
            event.wheel.y = y;
            event.wheel.direction = 0;
            event.wheel.preciseX = 0.0f;
            event.wheel.preciseY = static_cast<float>(delta) / 120.0f;
            event.wheel.mouseX = x;
            event.wheel.mouseY = y;
            pushSdlEvent(event);
        }

        void pushSdlQuit() {
            SdlEvent event{};
            event.type = sdlQuit;
            pushSdlEvent(event);
        }

        void pushSdlWindowEvent(uint32_t windowEvent) {
            SdlEvent event{};
            event.window.type = sdlWindowEvent;
            event.window.windowId = sdlWindowId;
            event.window.event = static_cast<uint8_t>(windowEvent);
            pushSdlEvent(event);
        }

        int m_lastMouseX = 0;
        int m_lastMouseY = 0;

        LRESULT CALLBACK coreWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            const int x = static_cast<short>(LOWORD(lparam));
            const int y = static_cast<short>(HIWORD(lparam));
            switch (message) {
                case WM_CLOSE:
                    pushSdlQuit();
                    closeCoreWindow();
                    return 0;
                case WM_DESTROY:
                    pushSdlQuit();
                    if (m_hwnd.load() == hwnd) m_hwnd = nullptr;
                    PostQuitMessage(0);
                    return 0;
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYUP:
                    bsx::input::pushKeyboardKey(static_cast<UINT>(wparam), message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
                    return 0;
                case WM_CHAR:
                case WM_SYSCHAR:
                    bsx::input::pushKeyboardText(static_cast<wchar_t>(wparam));
                    return 0;
                case WM_MOUSEMOVE: {
                    const int xrel = x - m_lastMouseX;
                    const int yrel = y - m_lastMouseY;
                    m_lastMouseX = x;
                    m_lastMouseY = y;
                    pushSdlMouseMove(wparam, x, y, xrel, yrel);
                    return 0;
                }
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONUP:
                    pushSdlMouseButton(message, x, y);
                    return 0;
                case WM_MOUSEWHEEL: {
                    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
                    ScreenToClient(hwnd, &point);
                    pushSdlMouseWheel(point.x, point.y, static_cast<short>(HIWORD(wparam)));
                    return 0;
                }
                case WM_SETFOCUS:
                    pushSdlWindowEvent(sdlWindowFocusGained);
                    return 0;
                case WM_KILLFOCUS:
                    pushSdlWindowEvent(sdlWindowFocusLost);
                    return 0;
                default:
                    return DefWindowProcW(hwnd, message, wparam, lparam);
            }
        }

        void windowThreadMain() {
            WNDCLASSEXW windowClass{sizeof(windowClass)};
            windowClass.lpfnWndProc = coreWindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = m_windowClassName;
            RegisterClassExW(&windowClass);
            RECT work{};
            int width = m_defaultWindowWidth;
            int height = m_defaultWindowHeight;
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
                const int maxWidth = work.right - work.left - 16;
                const int maxHeight = work.bottom - work.top - 16;
                if (width > maxWidth) width = maxWidth > 0 ? maxWidth : width;
                if (height > maxHeight) height = maxHeight > 0 ? maxHeight : height;
            }
            m_hwnd = CreateWindowExW(0, m_windowClassName, L"BSXbox", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
            m_windowReady.notify_all();
            if (!m_hwnd) return;
            log("host window created on its own thread (%dx%d)", width, height);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        void ensureWindow() {
            if (m_hwnd) return;
            std::unique_lock lock(m_windowMutex);
            if (!m_windowStarted) {
                m_windowStarted = true;
                std::thread(windowThreadMain).detach();
            }
            m_windowReady.wait(lock, [] { return m_hwnd.load() != nullptr; });
        }

        void showWindow() {
            ensureWindow();
            const HWND hwnd = m_hwnd.load();
            if (!hwnd) return;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            pushSdlWindowEvent(sdlWindowShown);
            pushSdlWindowEvent(sdlWindowFocusGained);
        }
    }

    wuc::CoreDispatcher makeDispatcher();

    struct CompletedAsyncAction : winrt::implements<CompletedAsyncAction, wf::IAsyncAction> {
        uint32_t Id() const { return 1; }
        wf::AsyncStatus Status() const { return wf::AsyncStatus::Completed; }
        winrt::hresult ErrorCode() const { return 0; }
        void Cancel() const {}
        void Close() const {}
        wf::AsyncActionCompletedHandler Completed() const { return nullptr; }
        void Completed(wf::AsyncActionCompletedHandler const& handler) const { if (handler) handler(*this, wf::AsyncStatus::Completed); }
        void GetResults() const {}
    };

    struct CoreDispatcherImpl : winrt::implements<CoreDispatcherImpl, wuc::ICoreDispatcher> {
        bool HasThreadAccess() const { return true; }

        void ProcessEvents(wuc::CoreProcessEventsOption const&) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        wf::IAsyncAction RunAsync(wuc::CoreDispatcherPriority const&, wuc::DispatchedHandler const& handler) {
            if (handler) handler();
            return winrt::make<CompletedAsyncAction>();
        }

        wf::IAsyncAction RunIdleAsync(wuc::IdleDispatchedHandler const& handler) {
            if (handler) handler(wuc::IdleDispatchedHandlerArgs{nullptr});
            return winrt::make<CompletedAsyncAction>();
        }
    };

    wuc::CoreDispatcher makeDispatcher() {
        auto abi = winrt::make<CoreDispatcherImpl>().as<wuc::ICoreDispatcher>();
        return wuc::CoreDispatcher{winrt::detach_abi(abi), winrt::take_ownership_from_abi};
    }

    using PointerHandler = wf::TypedEventHandler<wuc::CoreWindow, wuc::PointerEventArgs>;
    using KeyHandler = wf::TypedEventHandler<wuc::CoreWindow, wuc::KeyEventArgs>;

    struct CoreWindowImpl : winrt::implements<CoreWindowImpl, wuc::ICoreWindow, ICoreWindowInterop> {
        CoreWindowImpl() { members::m_coreWindow = this; }

        wf::IInspectable AutomationHostProvider() const { return nullptr; }

        wf::Rect Bounds() const {
            members::ensureWindow();
            RECT client{};
            int width = members::m_defaultWindowWidth;
            int height = members::m_defaultWindowHeight;
            if (members::m_hwnd && GetClientRect(members::m_hwnd, &client) && client.right > 0 && client.bottom > 0) {
                width = client.right;
                height = client.bottom;
            }
            return {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        }

        wfc::IPropertySet CustomProperties() const { return wfc::PropertySet(); }
        wuc::CoreDispatcher Dispatcher() const { return makeDispatcher(); }
        wuc::CoreWindowFlowDirection FlowDirection() const { return wuc::CoreWindowFlowDirection::LeftToRight; }
        void FlowDirection(wuc::CoreWindowFlowDirection const&) const {}
        bool IsInputEnabled() const { return true; }
        void IsInputEnabled(bool) const {}
        wuc::CoreCursor PointerCursor() const { return nullptr; }
        void PointerCursor(wuc::CoreCursor const&) const {}

        wf::Point PointerPosition() const {
            POINT position{};
            GetCursorPos(&position);
            if (members::m_hwnd) ScreenToClient(members::m_hwnd, &position);
            return {static_cast<float>(position.x), static_cast<float>(position.y)};
        }

        bool Visible() const { return members::m_hwnd && IsWindowVisible(members::m_hwnd) != FALSE; }

        void Activate() {
            members::ensureWindow();
            if (members::m_hwnd) {
                ShowWindow(members::m_hwnd, SW_SHOWNORMAL);
                SetForegroundWindow(members::m_hwnd);
            }
            members::log("CoreWindow.Activate (HWND %p)", static_cast<void*>(members::m_hwnd));
        }

        void Close() {
            if (members::m_hwnd) PostMessageW(members::m_hwnd, WM_CLOSE, 0, 0);
            else raiseClosed();
        }

        void raiseClosed() {
            members::log("CoreWindow.Close requested");
            try {
                m_closed(coreWindow(), wuc::CoreWindowEventArgs{nullptr});
            } catch (...) {
                members::log("CoreWindow.Closed handler threw an exception");
            }
        }

        wuc::CoreVirtualKeyStates GetAsyncKeyState(ws::VirtualKey const& key) const {
            return (::GetAsyncKeyState(static_cast<int>(key)) & 0x8000) ? wuc::CoreVirtualKeyStates::Down : wuc::CoreVirtualKeyStates::None;
        }

        wuc::CoreVirtualKeyStates GetKeyState(ws::VirtualKey const& key) const {
            return (::GetKeyState(static_cast<int>(key)) & 0x8000) ? wuc::CoreVirtualKeyStates::Down : wuc::CoreVirtualKeyStates::None;
        }

        void ReleasePointerCapture() const {}
        void SetPointerCapture() const {}

        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::WindowActivatedEventArgs>> m_activated;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::AutomationProviderRequestedEventArgs>> m_automationProviderRequested;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::CharacterReceivedEventArgs>> m_characterReceived;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::CoreWindowEventArgs>> m_closed;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::InputEnabledEventArgs>> m_inputEnabled;
        winrt::event<KeyHandler> m_keyDown;
        winrt::event<KeyHandler> m_keyUp;
        winrt::event<PointerHandler> m_pointerCaptureLost;
        winrt::event<PointerHandler> m_pointerEntered;
        winrt::event<PointerHandler> m_pointerExited;
        winrt::event<PointerHandler> m_pointerMoved;
        winrt::event<PointerHandler> m_pointerPressed;
        winrt::event<PointerHandler> m_pointerReleased;
        winrt::event<PointerHandler> m_pointerWheelChanged;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::TouchHitTestingEventArgs>> m_touchHitTesting;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::WindowSizeChangedEventArgs>> m_sizeChanged;
        winrt::event<wf::TypedEventHandler<wuc::CoreWindow, wuc::VisibilityChangedEventArgs>> m_visibilityChanged;

        winrt::event_token Activated(wf::TypedEventHandler<wuc::CoreWindow, wuc::WindowActivatedEventArgs> const& handler) { return m_activated.add(handler); }
        void Activated(winrt::event_token const& token) noexcept { m_activated.remove(token); }
        winrt::event_token AutomationProviderRequested(wf::TypedEventHandler<wuc::CoreWindow, wuc::AutomationProviderRequestedEventArgs> const& handler) { return m_automationProviderRequested.add(handler); }
        void AutomationProviderRequested(winrt::event_token const& token) noexcept { m_automationProviderRequested.remove(token); }
        winrt::event_token CharacterReceived(wf::TypedEventHandler<wuc::CoreWindow, wuc::CharacterReceivedEventArgs> const& handler) { return m_characterReceived.add(handler); }
        void CharacterReceived(winrt::event_token const& token) noexcept { m_characterReceived.remove(token); }
        winrt::event_token Closed(wf::TypedEventHandler<wuc::CoreWindow, wuc::CoreWindowEventArgs> const& handler) { return m_closed.add(handler); }
        void Closed(winrt::event_token const& token) noexcept { m_closed.remove(token); }
        winrt::event_token InputEnabled(wf::TypedEventHandler<wuc::CoreWindow, wuc::InputEnabledEventArgs> const& handler) { return m_inputEnabled.add(handler); }
        void InputEnabled(winrt::event_token const& token) noexcept { m_inputEnabled.remove(token); }
        winrt::event_token KeyDown(KeyHandler const& handler) { return m_keyDown.add(handler); }
        void KeyDown(winrt::event_token const& token) noexcept { m_keyDown.remove(token); }
        winrt::event_token KeyUp(KeyHandler const& handler) { return m_keyUp.add(handler); }
        void KeyUp(winrt::event_token const& token) noexcept { m_keyUp.remove(token); }
        winrt::event_token PointerCaptureLost(PointerHandler const& handler) { return m_pointerCaptureLost.add(handler); }
        void PointerCaptureLost(winrt::event_token const& token) noexcept { m_pointerCaptureLost.remove(token); }
        winrt::event_token PointerEntered(PointerHandler const& handler) { return m_pointerEntered.add(handler); }
        void PointerEntered(winrt::event_token const& token) noexcept { m_pointerEntered.remove(token); }
        winrt::event_token PointerExited(PointerHandler const& handler) { return m_pointerExited.add(handler); }
        void PointerExited(winrt::event_token const& token) noexcept { m_pointerExited.remove(token); }
        winrt::event_token PointerMoved(PointerHandler const& handler) { return m_pointerMoved.add(handler); }
        void PointerMoved(winrt::event_token const& token) noexcept { m_pointerMoved.remove(token); }
        winrt::event_token PointerPressed(PointerHandler const& handler) { return m_pointerPressed.add(handler); }
        void PointerPressed(winrt::event_token const& token) noexcept { m_pointerPressed.remove(token); }
        winrt::event_token PointerReleased(PointerHandler const& handler) { return m_pointerReleased.add(handler); }
        void PointerReleased(winrt::event_token const& token) noexcept { m_pointerReleased.remove(token); }
        winrt::event_token PointerWheelChanged(PointerHandler const& handler) { return m_pointerWheelChanged.add(handler); }
        void PointerWheelChanged(winrt::event_token const& token) noexcept { m_pointerWheelChanged.remove(token); }
        winrt::event_token TouchHitTesting(wf::TypedEventHandler<wuc::CoreWindow, wuc::TouchHitTestingEventArgs> const& handler) { return m_touchHitTesting.add(handler); }
        void TouchHitTesting(winrt::event_token const& token) noexcept { m_touchHitTesting.remove(token); }
        winrt::event_token SizeChanged(wf::TypedEventHandler<wuc::CoreWindow, wuc::WindowSizeChangedEventArgs> const& handler) { return m_sizeChanged.add(handler); }
        void SizeChanged(winrt::event_token const& token) noexcept { m_sizeChanged.remove(token); }
        winrt::event_token VisibilityChanged(wf::TypedEventHandler<wuc::CoreWindow, wuc::VisibilityChangedEventArgs> const& handler) { return m_visibilityChanged.add(handler); }
        void VisibilityChanged(winrt::event_token const& token) noexcept { m_visibilityChanged.remove(token); }

        HRESULT __stdcall get_WindowHandle(HWND* value) noexcept override {
            if (!value) return E_POINTER;
            members::ensureWindow();
            *value = members::m_hwnd;
            return S_OK;
        }

        HRESULT __stdcall put_MessageHandled(BOOL) noexcept override { return S_OK; }
    };

    wuc::CoreWindow coreWindow() {
        static wuc::ICoreWindow window = winrt::make<CoreWindowImpl>();
        wuc::ICoreWindow copy = window;
        return wuc::CoreWindow{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
    }

    void closeCoreWindow() {
        if (members::m_coreWindow) members::m_coreWindow->raiseClosed();
    }

    struct CoreApplicationViewImpl : winrt::implements<CoreApplicationViewImpl, wamc::ICoreApplicationView> {
        wuc::CoreWindow CoreWindow() const { return coreWindow(); }
        bool IsHosted() const { return false; }
        bool IsMain() const { return true; }

        winrt::event<wf::TypedEventHandler<wamc::CoreApplicationView, wama::IActivatedEventArgs>> m_activated;
        winrt::event_token Activated(wf::TypedEventHandler<wamc::CoreApplicationView, wama::IActivatedEventArgs> const& handler) { return m_activated.add(handler); }
        void Activated(winrt::event_token const& token) noexcept { m_activated.remove(token); }
    };

    wamc::CoreApplicationView coreApplicationView() {
        static wamc::ICoreApplicationView view = winrt::make<CoreApplicationViewImpl>();
        wamc::ICoreApplicationView copy = view;
        return wamc::CoreApplicationView{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
    }

    struct CoreWindowStaticImpl : winrt::implements<CoreWindowStaticImpl, wuc::ICoreWindowStatic, wf::IActivationFactory> {
        wuc::CoreWindow GetForCurrentThread() const { return coreWindow(); }
        wf::IInspectable ActivateInstance() const { return coreWindow(); }
    };

    struct SystemNavigationManagerImpl : winrt::implements<SystemNavigationManagerImpl, wuc::ISystemNavigationManager> {
        winrt::event<wf::EventHandler<wuc::BackRequestedEventArgs>> m_backRequested;
        winrt::event_token BackRequested(wf::EventHandler<wuc::BackRequestedEventArgs> const& handler) { return m_backRequested.add(handler); }
        void BackRequested(winrt::event_token const& token) noexcept { m_backRequested.remove(token); }
    };

    struct SystemNavigationManagerStaticsImpl : winrt::implements<SystemNavigationManagerStaticsImpl, wuc::ISystemNavigationManagerStatics, wf::IActivationFactory> {
        wuc::SystemNavigationManager GetForCurrentView() const {
            auto abi = winrt::make<SystemNavigationManagerImpl>().as<wuc::ISystemNavigationManager>();
            return wuc::SystemNavigationManager{winrt::detach_abi(abi), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    struct MouseDeviceImpl : winrt::implements<MouseDeviceImpl, wdi::IMouseDevice> {
        winrt::event<wf::TypedEventHandler<wdi::MouseDevice, wdi::MouseEventArgs>> m_mouseMoved;
        winrt::event_token MouseMoved(wf::TypedEventHandler<wdi::MouseDevice, wdi::MouseEventArgs> const& handler) { return m_mouseMoved.add(handler); }
        void MouseMoved(winrt::event_token const& token) noexcept { m_mouseMoved.remove(token); }
    };

    struct MouseDeviceStaticsImpl : winrt::implements<MouseDeviceStaticsImpl, wdi::IMouseDeviceStatics, wf::IActivationFactory> {
        wdi::MouseDevice GetForCurrentView() const {
            static wdi::IMouseDevice device = winrt::make<MouseDeviceImpl>();
            wdi::IMouseDevice copy = device;
            return wdi::MouseDevice{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    struct DisplayInformationImpl : winrt::implements<DisplayInformationImpl, wgd::IDisplayInformation> {
        wgd::DisplayOrientations CurrentOrientation() const { return wgd::DisplayOrientations::Landscape; }
        wgd::DisplayOrientations NativeOrientation() const { return wgd::DisplayOrientations::Landscape; }
        wgd::ResolutionScale ResolutionScale() const { return wgd::ResolutionScale::Scale100Percent; }
        float LogicalDpi() const { return 96.0f; }
        float RawDpiX() const { return 96.0f; }
        float RawDpiY() const { return 96.0f; }
        bool StereoEnabled() const { return false; }
        wf::IAsyncOperation<wss::IRandomAccessStream> GetColorProfileAsync() const { return nullptr; }

        winrt::event<wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable>> m_orientationChanged;
        winrt::event<wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable>> m_dpiChanged;
        winrt::event<wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable>> m_stereoEnabledChanged;
        winrt::event<wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable>> m_colorProfileChanged;
        winrt::event_token OrientationChanged(wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable> const& handler) { return m_orientationChanged.add(handler); }
        void OrientationChanged(winrt::event_token const& token) noexcept { m_orientationChanged.remove(token); }
        winrt::event_token DpiChanged(wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable> const& handler) { return m_dpiChanged.add(handler); }
        void DpiChanged(winrt::event_token const& token) noexcept { m_dpiChanged.remove(token); }
        winrt::event_token StereoEnabledChanged(wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable> const& handler) { return m_stereoEnabledChanged.add(handler); }
        void StereoEnabledChanged(winrt::event_token const& token) noexcept { m_stereoEnabledChanged.remove(token); }
        winrt::event_token ColorProfileChanged(wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable> const& handler) { return m_colorProfileChanged.add(handler); }
        void ColorProfileChanged(winrt::event_token const& token) noexcept { m_colorProfileChanged.remove(token); }
    };

    struct DisplayInformationStaticsImpl : winrt::implements<DisplayInformationStaticsImpl, wgd::IDisplayInformationStatics, wf::IActivationFactory> {
        wgd::DisplayInformation GetForCurrentView() const {
            static wgd::IDisplayInformation information = winrt::make<DisplayInformationImpl>();
            wgd::IDisplayInformation copy = information;
            return wgd::DisplayInformation{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
        wgd::DisplayOrientations AutoRotationPreferences() const { return wgd::DisplayOrientations::Landscape; }
        void AutoRotationPreferences(wgd::DisplayOrientations const&) const {}
        winrt::event<wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable>> m_displayContentsInvalidated;
        winrt::event_token DisplayContentsInvalidated(wf::TypedEventHandler<wgd::DisplayInformation, wf::IInspectable> const& handler) { return m_displayContentsInvalidated.add(handler); }
        void DisplayContentsInvalidated(winrt::event_token const& token) noexcept { m_displayContentsInvalidated.remove(token); }
    };

    struct ApplicationViewImpl : winrt::implements<ApplicationViewImpl, wuv::IApplicationView, wuv::IApplicationView2, wuv::IApplicationView3> {
        wuv::ApplicationViewOrientation Orientation() const { return wuv::ApplicationViewOrientation::Landscape; }
        bool AdjacentToLeftDisplayEdge() const { return true; }
        bool AdjacentToRightDisplayEdge() const { return true; }
        bool IsFullScreen() const { return m_fullscreen; }
        bool IsOnLockScreen() const { return false; }
        bool IsScreenCaptureEnabled() const { return true; }
        void IsScreenCaptureEnabled(bool) const {}
        void Title(winrt::hstring const&) const {}
        winrt::hstring Title() const { return L"BSXbox"; }
        int32_t Id() const { return 1; }

        winrt::event<wf::TypedEventHandler<wuv::ApplicationView, wuv::ApplicationViewConsolidatedEventArgs>> m_consolidated;
        winrt::event_token Consolidated(wf::TypedEventHandler<wuv::ApplicationView, wuv::ApplicationViewConsolidatedEventArgs> const& handler) { return m_consolidated.add(handler); }
        void Consolidated(winrt::event_token const& token) noexcept { m_consolidated.remove(token); }

        bool SuppressSystemOverlays() const { return false; }
        void SuppressSystemOverlays(bool) const {}
        wf::Rect VisibleBounds() const {
            RECT rect{0, 0, members::m_defaultWindowWidth, members::m_defaultWindowHeight};
            if (members::m_hwnd) GetClientRect(members::m_hwnd, &rect);
            return {static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)};
        }
        winrt::event<wf::TypedEventHandler<wuv::ApplicationView, wf::IInspectable>> m_visibleBoundsChanged;
        winrt::event_token VisibleBoundsChanged(wf::TypedEventHandler<wuv::ApplicationView, wf::IInspectable> const& handler) { return m_visibleBoundsChanged.add(handler); }
        void VisibleBoundsChanged(winrt::event_token const& token) noexcept { m_visibleBoundsChanged.remove(token); }
        bool SetDesiredBoundsMode(wuv::ApplicationViewBoundsMode const&) const { return true; }
        wuv::ApplicationViewBoundsMode DesiredBoundsMode() const { return wuv::ApplicationViewBoundsMode::UseVisible; }

        wuv::ApplicationViewTitleBar TitleBar() const { return nullptr; }
        wuv::FullScreenSystemOverlayMode FullScreenSystemOverlayMode() const { return wuv::FullScreenSystemOverlayMode::Standard; }
        void FullScreenSystemOverlayMode(wuv::FullScreenSystemOverlayMode const&) const {}
        bool IsFullScreenMode() const { return m_fullscreen; }
        bool TryEnterFullScreenMode() {
            m_fullscreen = true;
            return true;
        }
        void ExitFullScreenMode() { m_fullscreen = false; }
        void ShowStandardSystemOverlays() {}
        bool TryResizeView(wf::Size const& value) {
            const int width = static_cast<int>(value.Width);
            const int height = static_cast<int>(value.Height);
            if (members::m_hwnd && width > 0 && height > 0) {
                SetWindowPos(members::m_hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return true;
        }
        void SetPreferredMinSize(wf::Size const&) const {}

        private:
        bool m_fullscreen = false;
    };

    struct ApplicationViewStaticsImpl : winrt::implements<ApplicationViewStaticsImpl, wuv::IApplicationViewStatics2, wuv::IApplicationViewStatics3, wf::IActivationFactory> {
        wuv::ApplicationView GetForCurrentView() const {
            static wuv::IApplicationView view = winrt::make<ApplicationViewImpl>().as<wuv::IApplicationView>();
            wuv::IApplicationView copy = view;
            return wuv::ApplicationView{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
        }
        bool TerminateAppOnFinalViewClose() const { return false; }
        void TerminateAppOnFinalViewClose(bool) const {}
        wuv::ApplicationViewWindowingMode PreferredLaunchWindowingMode() const { return wuv::ApplicationViewWindowingMode::Auto; }
        void PreferredLaunchWindowingMode(wuv::ApplicationViewWindowingMode const&) const {}
        wf::Size PreferredLaunchViewSize() const { return {static_cast<float>(members::m_defaultWindowWidth), static_cast<float>(members::m_defaultWindowHeight)}; }
        void PreferredLaunchViewSize(wf::Size const&) const {}
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    struct InputPaneImpl : winrt::implements<InputPaneImpl, wuv::IInputPane> {
        wf::Rect OccludedRect() const { return {}; }
        bool TryShow() const { return true; }
        bool TryHide() const { return true; }

        winrt::event<wf::TypedEventHandler<wuv::InputPane, wuv::InputPaneVisibilityEventArgs>> m_showing;
        winrt::event<wf::TypedEventHandler<wuv::InputPane, wuv::InputPaneVisibilityEventArgs>> m_hiding;
        winrt::event_token Showing(wf::TypedEventHandler<wuv::InputPane, wuv::InputPaneVisibilityEventArgs> const& handler) { return m_showing.add(handler); }
        void Showing(winrt::event_token const& token) noexcept { m_showing.remove(token); }
        winrt::event_token Hiding(wf::TypedEventHandler<wuv::InputPane, wuv::InputPaneVisibilityEventArgs> const& handler) { return m_hiding.add(handler); }
        void Hiding(winrt::event_token const& token) noexcept { m_hiding.remove(token); }
    };

    struct InputPaneStaticsImpl : winrt::implements<InputPaneStaticsImpl, wuv::IInputPaneStatics, wf::IActivationFactory> {
        wuv::InputPane GetForCurrentView() const {
            static wuv::IInputPane pane = winrt::make<InputPaneImpl>();
            wuv::IInputPane copy = pane;
            return wuv::InputPane{winrt::detach_abi(copy), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    struct CoreApplicationImpl : winrt::implements<CoreApplicationImpl, wamc::ICoreApplication, wamc::ICoreApplicationExit, wamc::ICoreImmersiveApplication, wf::IActivationFactory> {
        wf::IInspectable ActivateInstance() const { return nullptr; }
        wfc::IVectorView<wamc::CoreApplicationView> Views() const {
            return winrt::single_threaded_vector<wamc::CoreApplicationView>({coreApplicationView()}).GetView();
        }
        wamc::CoreApplicationView CreateNewView(winrt::hstring const&, winrt::hstring const&) const { return coreApplicationView(); }
        wamc::CoreApplicationView MainView() const { return coreApplicationView(); }
        winrt::event<wf::EventHandler<wam::SuspendingEventArgs>> m_suspending;
        winrt::event<wf::EventHandler<wf::IInspectable>> m_resuming;
        winrt::event<wf::EventHandler<wf::IInspectable>> m_exiting;
        winrt::hstring Id() const { return winrt::hstring(L"BSXbox"); }
        winrt::event_token Suspending(wf::EventHandler<wam::SuspendingEventArgs> const& handler) { return m_suspending.add(handler); }
        void Suspending(winrt::event_token const& token) noexcept { m_suspending.remove(token); }
        winrt::event_token Resuming(wf::EventHandler<wf::IInspectable> const& handler) { return m_resuming.add(handler); }
        void Resuming(winrt::event_token const& token) noexcept { m_resuming.remove(token); }
        wfc::IPropertySet Properties() const { return wfc::PropertySet(); }
        wamc::CoreApplicationView GetCurrentView() const { return coreApplicationView(); }

        void Run(wamc::IFrameworkViewSource const& viewSource) {
            auto view = viewSource.CreateView();
            members::log("CoreApplication.Run: view created");
            view.Initialize(coreApplicationView());
            view.SetWindow(coreWindow());
            view.Load(winrt::hstring{});
            members::log("CoreApplication.Run: view.Run");
            view.Run();
            view.Uninitialize();
            members::log("CoreApplication.Run: view finished");
        }

        void RunWithActivationFactories(wf::IGetActivationFactory const&) { throw winrt::hresult_not_implemented(); }
        void Exit() { members::log("CoreApplication.Exit"); }
        winrt::event_token Exiting(wf::EventHandler<wf::IInspectable> const& handler) { return m_exiting.add(handler); }
        void Exiting(winrt::event_token const& token) noexcept { m_exiting.remove(token); }
    };

    wst::StorageFolder folderFromPath(const std::wstring& path) {
        return wst::StorageFolder::GetFolderFromPathAsync(winrt::hstring(path)).get();
    }

    struct PackageImpl : winrt::implements<PackageImpl, wam::IPackage> {
        wam::PackageId Id() const { return nullptr; }
        wst::StorageFolder InstalledLocation() const { return folderFromPath(members::m_installedPath); }
        bool IsFramework() const { return false; }
        wfc::IVectorView<wam::Package> Dependencies() const { throw winrt::hresult_not_implemented(); }
    };

    struct PackageStaticsImpl : winrt::implements<PackageStaticsImpl, wam::IPackageStatics, wf::IActivationFactory> {
        wam::Package Current() const {
            auto abi = winrt::make<PackageImpl>().as<wam::IPackage>();
            return wam::Package{winrt::detach_abi(abi), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    struct ApplicationDataImpl : winrt::implements<ApplicationDataImpl, wst::IApplicationData> {
        uint32_t Version() const { return 0; }
        wf::IAsyncAction SetVersionAsync(uint32_t, wst::ApplicationDataSetVersionHandler const&) { return winrt::make<CompletedAsyncAction>(); }
        wf::IAsyncAction ClearAsync() { return winrt::make<CompletedAsyncAction>(); }
        wf::IAsyncAction ClearAsync(wst::ApplicationDataLocality const&) { return winrt::make<CompletedAsyncAction>(); }
        wst::ApplicationDataContainer LocalSettings() const { throw winrt::hresult_not_implemented(); }
        wst::ApplicationDataContainer RoamingSettings() const { throw winrt::hresult_not_implemented(); }
        wst::StorageFolder LocalFolder() const { return folderFromPath(members::m_localStatePath); }
        wst::StorageFolder RoamingFolder() const { throw winrt::hresult_not_implemented(); }
        wst::StorageFolder TemporaryFolder() const { throw winrt::hresult_not_implemented(); }
        winrt::event_token DataChanged(wf::TypedEventHandler<wst::ApplicationData, wf::IInspectable> const&) { return {0}; }
        void DataChanged(winrt::event_token const&) noexcept {}
        void SignalDataChanged() const {}
        uint64_t RoamingStorageQuota() const { return 0; }
    };

    struct ApplicationDataStaticsImpl : winrt::implements<ApplicationDataStaticsImpl, wst::IApplicationDataStatics, wf::IActivationFactory> {
        wst::ApplicationData Current() const {
            auto abi = winrt::make<ApplicationDataImpl>().as<wst::IApplicationData>();
            return wst::ApplicationData{winrt::detach_abi(abi), winrt::take_ownership_from_abi};
        }
        wf::IInspectable ActivateInstance() const { return nullptr; }
    };

    bool provide(const wf::IInspectable& factory, const winrt::guid& iid, void** out) {
        try {
            factory.as(iid, out);
            return true;
        } catch (...) {
            return false;
        }
    }

    void setPaths(const wchar_t* installedPath, const wchar_t* localStatePath) {
        members::m_installedPath = installedPath ? installedPath : L"";
        members::m_localStatePath = localStatePath ? localStatePath : L"";
        if (!members::m_localStatePath.empty()) CreateDirectoryW(members::m_localStatePath.c_str(), nullptr);
    }

    bool getActivationFactory(const wchar_t* classId, REFIID iid, void** out) {
        if (!out || !classId) return false;
        *out = nullptr;
        winrt::guid const target{iid};
        try {
            if (std::wstring_view(classId) == L"Windows.ApplicationModel.Core.CoreApplication") return provide(winrt::make<CoreApplicationImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.UI.Core.CoreWindow") return provide(winrt::make<CoreWindowStaticImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.UI.Core.SystemNavigationManager") return provide(winrt::make<SystemNavigationManagerStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.Devices.Input.MouseDevice") return provide(winrt::make<MouseDeviceStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.Graphics.Display.DisplayInformation") return provide(winrt::make<DisplayInformationStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.UI.ViewManagement.InputPane") return provide(winrt::make<InputPaneStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.UI.ViewManagement.ApplicationView") return provide(winrt::make<ApplicationViewStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.ApplicationModel.Package") return provide(winrt::make<PackageStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.Storage.ApplicationData") return provide(winrt::make<ApplicationDataStaticsImpl>(), target, out);
        } catch (...) {
            return false;
        }
        return false;
    }

    IUnknown* retainCoreWindow() {
        auto window = coreWindow();
        return reinterpret_cast<IUnknown*>(winrt::detach_abi(window));
    }

    void showHostWindow() {
        members::showWindow();
    }

    HWND hostWindow() {
        return members::m_hwnd.load();
    }
}
