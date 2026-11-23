#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/shims.hpp>
#include <shims/uwphost.hpp>

namespace bsx::shims::members {
    bool sdlTraceEnabled() {
        static const bool enabled = [] {
            char value[8] = {};
            return GetEnvironmentVariableA("BSXBOX_TRACE_SDL", value, sizeof(value)) > 0;
        }();
        return enabled;
    }

    const char* sdlError() {
        using GetErrorFn = const char* (*)();
        static GetErrorFn getError = reinterpret_cast<GetErrorFn>(bsx::findMappedExport("SDL2.dll", "SDL_GetError"));
        return getError ? getError() : "";
    }

    void sdlTrace(const char* format, ...) {
        if (!sdlTraceEnabled()) return;
        va_list args;
        va_start(args, format);
        char message[512] = {};
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        bsx::logf("sdl", "%s", message);
    }

    int shimSdlInit(uint32_t flags) {
        using InitFn = int (*)(uint32_t);
        static InitFn init = reinterpret_cast<InitFn>(bsx::findMappedExport("SDL2.dll", "SDL_Init"));
        const int result = init ? init(flags) : -1;
        sdlTrace("SDL_Init(0x%X) -> %d error='%s'", flags, result, sdlError());
        return result;
    }

    int shimSdlInitSubSystem(uint32_t flags) {
        using InitFn = int (*)(uint32_t);
        static InitFn init = reinterpret_cast<InitFn>(bsx::findMappedExport("SDL2.dll", "SDL_InitSubSystem"));
        const int result = init ? init(flags) : -1;
        sdlTrace("SDL_InitSubSystem(0x%X) -> %d error='%s'", flags, result, sdlError());
        return result;
    }

    void* shimSdlCreateWindow(const char* title, int x, int y, int width, int height, uint32_t flags) {
        using CreateWindowFn = void* (*)(const char*, int, int, int, int, uint32_t);
        static CreateWindowFn createWindow = reinterpret_cast<CreateWindowFn>(bsx::findMappedExport("SDL2.dll", "SDL_CreateWindow"));
        void* window = createWindow ? createWindow(title, x, y, width, height, flags) : nullptr;
        if (window) bsx::uwp::showHostWindow();
        sdlTrace("SDL_CreateWindow(%dx%d flags=0x%X) -> %p error='%s'", width, height, flags, window, sdlError());
        return window;
    }

    void* shimSdlGlCreateContext(void* window) {
        using CreateContextFn = void* (*)(void*);
        static CreateContextFn createContext = reinterpret_cast<CreateContextFn>(bsx::findMappedExport("SDL2.dll", "SDL_GL_CreateContext"));
        void* context = createContext ? createContext(window) : nullptr;
        sdlTrace("SDL_GL_CreateContext(%p) -> %p error='%s'", window, context, sdlError());
        return context;
    }

    void* shimSdlGlGetProcAddress(const char* name) {
        using GetProcFn = void* (*)(const char*);
        static GetProcFn getProc = reinterpret_cast<GetProcFn>(bsx::findMappedExport("SDL2.dll", "SDL_GL_GetProcAddress"));
        void* address = getProc ? getProc(name) : nullptr;
        sdlTrace("SDL_GL_GetProcAddress('%s') -> %p error='%s'", name ? name : "", address, sdlError());
        return address;
    }

    const char* shimSdlGetError() {
        using GetErrorFn = const char* (*)();
        static GetErrorFn getError = reinterpret_cast<GetErrorFn>(bsx::findMappedExport("SDL2.dll", "SDL_GetError"));
        const char* error = getError ? getError() : "";
        sdlTrace("SDL_GetError -> '%s'", error);
        return error;
    }
}

namespace bsx::shims {
    SHIMTABLE(sdlVideo,
        {"sdl2.dll", "SDL_Init", reinterpret_cast<void*>(&members::shimSdlInit)},
        {"sdl2.dll", "SDL_InitSubSystem", reinterpret_cast<void*>(&members::shimSdlInitSubSystem)},
        {"sdl2.dll", "SDL_CreateWindow", reinterpret_cast<void*>(&members::shimSdlCreateWindow)},
        {"sdl2.dll", "SDL_GL_CreateContext", reinterpret_cast<void*>(&members::shimSdlGlCreateContext)},
        {"sdl2.dll", "SDL_GL_GetProcAddress", reinterpret_cast<void*>(&members::shimSdlGlGetProcAddress)},
        {"sdl2.dll", "SDL_GetError", reinterpret_cast<void*>(&members::shimSdlGetError)}
    );
}
