#include <windows.h>
#include <dxgi1_2.h>

#include <cstdio>

#include <shims/uwphost.hpp>

namespace bsx::uwp {
    namespace members {
        using CreateSwapChainForCoreWindowFn = HRESULT(__stdcall*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);

        CreateSwapChainForCoreWindowFn m_originalCoreWindow = nullptr;
        void** m_coreWindowSlot = nullptr;

        HRESULT __stdcall createSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
            ICoreWindowInterop* interop = nullptr;
            HWND hwnd = nullptr;
            if (window && SUCCEEDED(window->QueryInterface(IID_PPV_ARGS(&interop))) && interop) {
                const HRESULT windowResult = interop->get_WindowHandle(&hwnd);
                interop->Release();
                if (SUCCEEDED(windowResult) && hwnd) {
                    const HRESULT result = factory->CreateSwapChainForHwnd(device, hwnd, desc, nullptr, restrictToOutput, swapChain);
                    std::fprintf(stderr, "[bsx] CoreWindow -> HWND %p swap chain -> 0x%08lX\n", static_cast<void*>(hwnd), static_cast<unsigned long>(result));
                    return result;
                }
            }
            return m_originalCoreWindow(factory, device, window, desc, restrictToOutput, swapChain);
        }
    }

    void installDxgiSwapChainAdapter() {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi) {
            std::fprintf(stderr, "[bsx] dxgi.dll not loaded; swap-chain adapter skipped\n");
            return;
        }
        using CreateDxgiFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = reinterpret_cast<CreateDxgiFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (!create) {
            std::fprintf(stderr, "[bsx] CreateDXGIFactory1 not found; adapter skipped\n");
            return;
        }
        IDXGIFactory2* factory = nullptr;
        const HRESULT result = create(IID_PPV_ARGS(&factory));
        if (FAILED(result) || !factory) {
            std::fprintf(stderr, "[bsx] temporary factory failed 0x%08lX; adapter skipped\n", static_cast<unsigned long>(result));
            return;
        }
        auto vtable = *reinterpret_cast<void***>(factory);
        void** slot = vtable + 16;
        members::m_originalCoreWindow = reinterpret_cast<members::CreateSwapChainForCoreWindowFn>(*slot);
        members::m_coreWindowSlot = slot;
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            std::fprintf(stderr, "[bsx] vtable is not writable; adapter skipped\n");
            factory->Release();
            members::m_originalCoreWindow = nullptr;
            members::m_coreWindowSlot = nullptr;
            return;
        }
        *slot = reinterpret_cast<void*>(&members::createSwapChainForCoreWindow);
        VirtualProtect(slot, sizeof(void*), old, &old);
        factory->Release();
        std::fprintf(stderr, "[bsx] CoreWindow swap-chain adapter installed at %p\n", static_cast<void*>(slot));
    }
}
