#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/controller.hpp>
#include <shims/shims.hpp>

namespace bsx::input::members {
    struct SdlJoystickGuid { uint8_t data[16]; };
    struct SdlKeyboardEvent { uint32_t type, timestamp, windowId; uint8_t state, repeat, padding[2]; int32_t scancode, sym; uint16_t mod; uint32_t unused; };
    struct SdlTextEvent { uint32_t type, timestamp, windowId; char text[32]; };
    struct SdlJoyAxisEvent { uint32_t type, timestamp; int32_t which; uint8_t axis, padding1, padding2, padding3; int16_t value; uint16_t padding4; };
    struct SdlJoyHatEvent { uint32_t type, timestamp; int32_t which; uint8_t hat, value, padding1, padding2; };
    union SdlEvent { uint32_t type; SdlKeyboardEvent key; SdlTextEvent text; SdlJoyAxisEvent axis; SdlJoyHatEvent hat; uint8_t padding[56]; };

    constexpr uint32_t sdlKeyDown = 0x300;
    constexpr uint32_t sdlKeyUp = 0x301;
    constexpr uint32_t sdlTextInput = 0x303;
    constexpr uint32_t sdlJoyAxisMotion = 0x600;
    constexpr uint32_t sdlJoyHatMotion = 0x602;
    constexpr uint32_t sdlJoyButtonDown = 0x603;
    constexpr uint32_t sdlJoyButtonUp = 0x604;
    constexpr uint32_t sdlWindowId = 1;
    constexpr uint32_t sdlScancodeMask = 0x40000000u;
    constexpr uint8_t sdlHatUp = 1;
    constexpr uint8_t sdlHatRight = 2;
    constexpr uint8_t sdlHatDown = 4;
    constexpr uint8_t sdlHatLeft = 8;
    constexpr int16_t sdlAxisMin = -32768;
    constexpr int16_t sdlAxisMax = 32767;
    constexpr uint8_t sdlDpadHorizontal = 1;
    constexpr uint8_t sdlDpadVertical = 2;
    constexpr const char* sdlGenericControllerName = "HID-compliant game controller";
    constexpr const char* sdlDualSenseName = "DualSense Wireless Controller (BSXbox)";
    std::vector<std::string> m_registeredControllerMappings;
    std::vector<std::string> m_tracedControllerGuids;
    std::map<int32_t, uint8_t> m_dpadAxisState;
    bool m_dualsenseMapped = false;
    int m_lastControllerCount = -1;

    bool inputTraceEnabled() {
        static const bool enabled = GetEnvironmentVariableA("BSXBOX_TRACE_SDL", nullptr, 0) > 0;
        return enabled;
    }

    void inputTrace(const char* format, ...) {
        if (!inputTraceEnabled()) return;
        va_list args;
        va_start(args, format);
        char message[512] = {};
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        bsx::logf("sdl", "%s", message);
    }

    bool pushEvent(const SdlEvent& event) {
        using PushEventFn = int (*)(const void*);
        static PushEventFn push = reinterpret_cast<PushEventFn>(bsx::findMappedExport("SDL2.dll", "SDL_PushEvent"));
        return push && push(&event) == 0;
    }

    uint32_t sdlKeycode(UINT key) {
        if (key >= 'A' && key <= 'Z') return key + 32;
        if (key >= '0' && key <= '9') return key;
        switch (key) {
            case VK_BACK: return 0x4000002A;
            case VK_TAB: return 0x4000002B;
            case VK_RETURN: return 0x40000028;
            case VK_ESCAPE: return 0x40000029;
            case VK_SPACE: return 0x20;
            case VK_LEFT: return sdlScancodeMask | 80;
            case VK_RIGHT: return sdlScancodeMask | 79;
            case VK_UP: return sdlScancodeMask | 82;
            case VK_DOWN: return sdlScancodeMask | 81;
            case VK_INSERT: return sdlScancodeMask | 73;
            case VK_HOME: return sdlScancodeMask | 74;
            case VK_PRIOR: return sdlScancodeMask | 75;
            case VK_DELETE: return sdlScancodeMask | 76;
            case VK_END: return sdlScancodeMask | 77;
            case VK_NEXT: return sdlScancodeMask | 78;
            case VK_LSHIFT: return sdlScancodeMask | 225;
            case VK_RSHIFT: return sdlScancodeMask | 229;
            case VK_LCONTROL: return sdlScancodeMask | 224;
            case VK_RCONTROL: return sdlScancodeMask | 228;
            case VK_LMENU: return sdlScancodeMask | 226;
            case VK_RMENU: return sdlScancodeMask | 230;
            case VK_CAPITAL: return sdlScancodeMask | 57;
            case VK_OEM_1: return ';';
            case VK_OEM_2: return '/';
            case VK_OEM_3: return '`';
            case VK_OEM_4: return '[';
            case VK_OEM_5: return '\\';
            case VK_OEM_6: return ']';
            case VK_OEM_7: return '\'';
            case VK_OEM_PLUS: return '=';
            case VK_OEM_COMMA: return ',';
            case VK_OEM_MINUS: return '-';
            case VK_OEM_PERIOD: return '.';
            default: break;
        }
        if (key >= VK_F1 && key <= VK_F12) return sdlScancodeMask | (58 + (key - VK_F1));
        return 0;
    }

    uint16_t sdlModifiers() {
        uint16_t mod = 0;
        if (GetKeyState(VK_SHIFT) < 0) mod |= 0x0003;
        if (GetKeyState(VK_CONTROL) < 0) mod |= 0x00C0;
        if (GetKeyState(VK_MENU) < 0) mod |= 0x0300;
        if (GetKeyState(VK_CAPITAL) & 1) mod |= 0x2000;
        if (GetKeyState(VK_NUMLOCK) & 1) mod |= 0x1000;
        return mod;
    }

    std::string sdlGuidString(const SdlJoystickGuid& guid) {
        static const char digits[] = "0123456789ABCDEF";
        std::string text;
        text.reserve(32);
        for (uint8_t byte : guid.data) {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 15]);
        }
        return text;
    }

    bool sdlIsDualSense(const SdlJoystickGuid& guid) {
        const uint16_t vendor = static_cast<uint16_t>(guid.data[4] | (guid.data[5] << 8));
        const uint16_t product = static_cast<uint16_t>(guid.data[8] | (guid.data[9] << 8));
        if (vendor != 0x054C) return false;
        return product == 0x0CE6 || product == 0x0DF2 || product == 0x05C4 || product == 0x09CC;
    }

    const char* mapJoystickName(const char* name) {
        if (m_dualsenseMapped && name && (std::strcmp(name, sdlGenericControllerName) == 0 || std::strstr(name, "DualSense") || std::strcmp(name, "Wireless Controller") == 0)) return sdlDualSenseName;
        return name;
    }

    void pushDpadAxisEvent(int32_t which, uint8_t axis, int16_t value, uint32_t timestamp) {
        SdlEvent event{};
        event.axis.type = sdlJoyAxisMotion;
        event.axis.timestamp = timestamp;
        event.axis.which = which;
        event.axis.axis = axis;
        event.axis.value = value;
        pushEvent(event);
    }

    void translateDpadToLeftStick(const SdlJoyHatEvent& hat) {
        uint8_t& active = m_dpadAxisState[hat.which];
        const int16_t horizontal = (hat.value & sdlHatLeft) ? sdlAxisMin : ((hat.value & sdlHatRight) ? sdlAxisMax : 0);
        const int16_t vertical = (hat.value & sdlHatUp) ? sdlAxisMin : ((hat.value & sdlHatDown) ? sdlAxisMax : 0);
        if (horizontal != 0 || (active & sdlDpadHorizontal)) pushDpadAxisEvent(hat.which, 0, horizontal, hat.timestamp);
        if (vertical != 0 || (active & sdlDpadVertical)) pushDpadAxisEvent(hat.which, 1, vertical, hat.timestamp);
        active = static_cast<uint8_t>((horizontal ? sdlDpadHorizontal : 0) | (vertical ? sdlDpadVertical : 0));
        if (!active) m_dpadAxisState.erase(hat.which);
    }

    int shimSdlPollEvent(void* event) {
        using PollEventFn = int (*)(void*);
        static PollEventFn pollEvent = reinterpret_cast<PollEventFn>(bsx::findMappedExport("SDL2.dll", "SDL_PollEvent"));
        bsx::input::initializeControllers();
        if (!pollEvent) return 0;
        const int result = pollEvent(event);
        if (result && event) {
            const SdlEvent* sdlEvent = static_cast<const SdlEvent*>(event);
            if (sdlEvent->type == sdlJoyHatMotion) {
                inputTrace("joystick hat instance=%d hat=%u value=%u", sdlEvent->hat.which, sdlEvent->hat.hat, sdlEvent->hat.value);
                translateDpadToLeftStick(sdlEvent->hat);
            } else if (sdlEvent->type == sdlJoyButtonDown || sdlEvent->type == sdlJoyButtonUp) {
                inputTrace("joystick button instance=%d button=%u pressed=%u", sdlEvent->hat.which, sdlEvent->hat.hat, sdlEvent->hat.value);
            }
        }
        return result;
    }

    const char* shimSdlJoystickName(void* joystick) {
        using NameFn = const char* (*)(void*);
        static NameFn name = reinterpret_cast<NameFn>(bsx::findMappedExport("SDL2.dll", "SDL_JoystickName"));
        return mapJoystickName(name ? name(joystick) : nullptr);
    }

    const char* shimSdlJoystickNameForIndex(int index) {
        using NameFn = const char* (*)(int);
        static NameFn name = reinterpret_cast<NameFn>(bsx::findMappedExport("SDL2.dll", "SDL_JoystickNameForIndex"));
        return mapJoystickName(name ? name(index) : nullptr);
    }
}

namespace bsx::input {
    void initializeControllers() {
        using NumJoysticksFn = int (*)();
        using NameForIndexFn = const char* (*)(int);
        using GetDeviceGuidFn = members::SdlJoystickGuid (*)(int);
        using AddMappingFn = int (*)(const char*);
        static NumJoysticksFn numJoysticks = reinterpret_cast<NumJoysticksFn>(bsx::findMappedExport("SDL2.dll", "SDL_NumJoysticks"));
        static NameForIndexFn nameForIndex = reinterpret_cast<NameForIndexFn>(bsx::findMappedExport("SDL2.dll", "SDL_JoystickNameForIndex"));
        static GetDeviceGuidFn getDeviceGuid = reinterpret_cast<GetDeviceGuidFn>(bsx::findMappedExport("SDL2.dll", "SDL_JoystickGetDeviceGUID"));
        static AddMappingFn addMapping = reinterpret_cast<AddMappingFn>(bsx::findMappedExport("SDL2.dll", "SDL_GameControllerAddMapping"));
        if (!numJoysticks || !getDeviceGuid || !addMapping) return;
        const int count = numJoysticks();
        if (count != members::m_lastControllerCount) {
            members::inputTrace("controller scan found %d joystick(s)", count);
            members::m_lastControllerCount = count;
        }
        for (int index = 0; index < count; ++index) {
            const members::SdlJoystickGuid guid = getDeviceGuid(index);
            const std::string guidString = members::sdlGuidString(guid);
            bool traced = false;
            for (const std::string& known : members::m_tracedControllerGuids) if (known == guidString) traced = true;
            if (!traced) {
                const char* traceName = nameForIndex ? nameForIndex(index) : nullptr;
                members::inputTrace("joystick %d name='%s' guid=%s", index, traceName ? traceName : "", guidString.c_str());
                members::m_tracedControllerGuids.push_back(guidString);
            }
            if (!members::sdlIsDualSense(guid)) continue;
            bool registered = false;
            for (const std::string& known : members::m_registeredControllerMappings) if (known == guidString) registered = true;
            if (registered) continue;
            const char* deviceName = nameForIndex ? nameForIndex(index) : nullptr;
            if (!deviceName) deviceName = members::sdlGenericControllerName;
            const std::string mapping = guidString + "," + deviceName + ",a:b1,b:b2,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,leftstick:b10,rightstick:b11,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a5,lefttrigger:a3,righttrigger:a4";
            if (addMapping(mapping.c_str()) >= 0) {
                members::m_registeredControllerMappings.push_back(guidString);
                members::m_dualsenseMapped = true;
                bsx::logf("sdl", "registered game controller mapping for '%s' (%s)", deviceName, guidString.c_str());
            }
        }
    }

    void pushKeyboardKey(UINT key, bool down) {
        const uint32_t sym = members::sdlKeycode(key);
        if (!sym) return;
        members::SdlEvent event{};
        event.key.type = down ? members::sdlKeyDown : members::sdlKeyUp;
        event.key.windowId = members::sdlWindowId;
        event.key.state = down ? 1 : 0;
        event.key.repeat = 0;
        event.key.sym = static_cast<int32_t>(sym);
        event.key.mod = members::sdlModifiers();
        members::pushEvent(event);
    }

    void pushKeyboardText(wchar_t character) {
        if (character < 0x20) return;
        members::SdlEvent event{};
        event.text.type = members::sdlTextInput;
        event.text.windowId = members::sdlWindowId;
        char utf8[8] = {};
        const int length = WideCharToMultiByte(CP_UTF8, 0, &character, 1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        if (length <= 0) return;
        std::memcpy(event.text.text, utf8, static_cast<size_t>(length));
        members::pushEvent(event);
    }
}

namespace bsx::shims {
    SHIMTABLE(controllerInput,
        {"sdl2.dll", "SDL_PollEvent", reinterpret_cast<void*>(&bsx::input::members::shimSdlPollEvent)},
        {"sdl2.dll", "SDL_JoystickName", reinterpret_cast<void*>(&bsx::input::members::shimSdlJoystickName)},
        {"sdl2.dll", "SDL_JoystickNameForIndex", reinterpret_cast<void*>(&bsx::input::members::shimSdlJoystickNameForIndex)}
    );
}
