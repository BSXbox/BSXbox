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
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdarg>
#include <cstdio>
#include <string>

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
        std::wstring m_installedPath;
        std::wstring m_localStatePath;
        HWND m_hwnd = nullptr;
        CoreWindowImpl* m_coreWindow = nullptr;
        constexpr wchar_t m_windowClassName[] = L"BSXboxUWPHostWindow";
        constexpr int m_defaultWindowWidth = 1280;
        constexpr int m_defaultWindowHeight = 720;

        void log(const char* format, ...) {
            va_list args;
            va_start(args, format);
            std::fputs("[bsx] ", stderr);
            std::vfprintf(stderr, format, args);
            std::fputc('\n', stderr);
            va_end(args);
            std::fflush(stderr);
        }

        LRESULT CALLBACK coreWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
            switch (message) {
                case WM_CLOSE:
                    closeCoreWindow();
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                case WM_DESTROY:
                    if (m_hwnd == hwnd) m_hwnd = nullptr;
                    return 0;
                default:
                    return DefWindowProcW(hwnd, message, wparam, lparam);
            }
        }

        void ensureWindow() {
            if (m_hwnd) return;
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
            if (m_hwnd) log("host window created (%dx%d, hidden until CoreWindow.Activate)", width, height);
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
            if (std::wstring_view(classId) == L"Windows.ApplicationModel.Package") return provide(winrt::make<PackageStaticsImpl>(), target, out);
            if (std::wstring_view(classId) == L"Windows.Storage.ApplicationData") return provide(winrt::make<ApplicationDataStaticsImpl>(), target, out);
        } catch (...) {
            return false;
        }
        return false;
    }

    void swapChainProbe() {
        try {
            wuc::CoreWindow window = coreWindow();
            auto interop = window.try_as<ICoreWindowInterop>();
            HWND hwnd = nullptr;
            if (interop) interop->get_WindowHandle(&hwnd);
            members::log("probe: interop QI %s, hwnd=%p", interop ? "ok" : "FAIL", static_cast<void*>(hwnd));
            winrt::com_ptr<ID3D11Device> device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL featureLevel{};
            HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), &featureLevel, context.put());
            members::log("probe: D3D11CreateDevice 0x%08lX", static_cast<unsigned long>(result));
            if (FAILED(result)) return;
            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            result = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
            members::log("probe: QI(IDXGIDevice) 0x%08lX", static_cast<unsigned long>(result));
            if (FAILED(result)) return;
            winrt::com_ptr<IDXGIAdapter> adapter;
            result = dxgiDevice->GetAdapter(adapter.put());
            members::log("probe: GetAdapter 0x%08lX", static_cast<unsigned long>(result));
            if (FAILED(result)) return;
            winrt::com_ptr<IDXGIFactory2> factory;
            result = adapter->GetParent(IID_PPV_ARGS(factory.put()));
            members::log("probe: GetParent(IDXGIFactory2) 0x%08lX", static_cast<unsigned long>(result));
            if (FAILED(result)) return;
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 854;
            desc.Height = 480;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            winrt::com_ptr<IDXGISwapChain1> coreSwapChain;
            result = factory->CreateSwapChainForCoreWindow(device.get(), reinterpret_cast<IUnknown*>(winrt::get_abi(window)), &desc, nullptr, coreSwapChain.put());
            members::log("probe: CreateSwapChainForCoreWindow 0x%08lX", static_cast<unsigned long>(result));
            if (hwnd && FAILED(result)) {
                winrt::com_ptr<IDXGISwapChain1> hwndSwapChain;
                result = factory->CreateSwapChainForHwnd(device.get(), hwnd, &desc, nullptr, nullptr, hwndSwapChain.put());
                members::log("probe: CreateSwapChainForHwnd(same desc) 0x%08lX", static_cast<unsigned long>(result));
            }
        } catch (...) {
            members::log("probe: exception escaped");
        }
    }
}
