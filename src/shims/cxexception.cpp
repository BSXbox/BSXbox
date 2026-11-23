#include <windows.h>
#include <winrt/base.h>

#include <cstdio>

#include <log.hpp>
#include <pe/pe.hpp>
#include <shims/shims.hpp>

namespace bsx::cx {
    static void traceRaise(const char* name, long hr) {
        static const bool enabled = [] {
            char value[8] = {};
            return GetEnvironmentVariableA("BSXBOX_TRACE_CX", value, sizeof(value)) > 0;
        }();
        if (!enabled) return;
        bsx::logf("cx", "%s hr=0x%08lX", name, static_cast<unsigned long>(hr));
        void* frames[32] = {};
        const USHORT count = CaptureStackBackTrace(1, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) bsx::logf("cx", "  frame %02u %p", i, frames[i]);
    }

    [[noreturn]] void __cdecl raiseComException(long hr) {
        traceRaise("COMException", hr);
        throw winrt::hresult_error(static_cast<HRESULT>(hr));
    }

#define BSX_CX_RAISE(function, name, code) \
    [[noreturn]] void __cdecl function() { traceRaise(#name, code); throw winrt::hresult_error(code); }

    BSX_CX_RAISE(raiseDisconnected,          DisconnectedException,         RO_E_CLOSED)
    BSX_CX_RAISE(raiseWrongThread,           WrongThreadException,          0x8000000E) // RO_E_WRONG_THREAD
    BSX_CX_RAISE(raiseObjectDisposed,        ObjectDisposedException,       RO_E_CLOSED)
    BSX_CX_RAISE(raiseClassNotRegistered,    ClassNotRegisteredException,   REGDB_E_CLASSNOTREG)
    BSX_CX_RAISE(raiseChangedState,          ChangedStateException,         E_CHANGED_STATE)
    BSX_CX_RAISE(raiseOutOfBounds,           OutOfBoundsException,          E_BOUNDS)
    BSX_CX_RAISE(raiseInvalidArgument,       InvalidArgumentException,      E_INVALIDARG)
    BSX_CX_RAISE(raiseOutOfMemory,           OutOfMemoryException,          E_OUTOFMEMORY)
    BSX_CX_RAISE(raiseAccessDenied,          AccessDeniedException,         E_ACCESSDENIED)
    BSX_CX_RAISE(raiseFailure,               FailureException,              E_FAIL)
    BSX_CX_RAISE(raiseOperationCanceled,     OperationCanceledException,    E_ABORT)
    BSX_CX_RAISE(raiseNullReference,         NullReferenceException,        E_POINTER)
    BSX_CX_RAISE(raiseInvalidCast,           InvalidCastException,          E_NOINTERFACE)
    BSX_CX_RAISE(raiseNotImplemented,        NotImplementedException,      E_NOTIMPL)

#undef BSX_CX_RAISE
}

namespace bsx::shims {
#define BSX_CX_ENTRY(dll, name, fn) {dll, "?__abi_WinRTraise" name "@@YAXXZ", reinterpret_cast<void*>(&bsx::cx::fn)}
    SHIMTABLE(cxException,
        {"vccorlib140_app.dll", "?__abi_WinRTraiseCOMException@@YAXJ@Z", reinterpret_cast<void*>(&bsx::cx::raiseComException)},
        {"vccorlib140.dll", "?__abi_WinRTraiseCOMException@@YAXJ@Z", reinterpret_cast<void*>(&bsx::cx::raiseComException)},
        BSX_CX_ENTRY("vccorlib140_app.dll", "DisconnectedException", raiseDisconnected),
        BSX_CX_ENTRY("vccorlib140_app.dll", "WrongThreadException", raiseWrongThread),
        BSX_CX_ENTRY("vccorlib140_app.dll", "ObjectDisposedException", raiseObjectDisposed),
        BSX_CX_ENTRY("vccorlib140_app.dll", "ClassNotRegisteredException", raiseClassNotRegistered),
        BSX_CX_ENTRY("vccorlib140_app.dll", "ChangedStateException", raiseChangedState),
        BSX_CX_ENTRY("vccorlib140_app.dll", "OutOfBoundsException", raiseOutOfBounds),
        BSX_CX_ENTRY("vccorlib140_app.dll", "InvalidArgumentException", raiseInvalidArgument),
        BSX_CX_ENTRY("vccorlib140_app.dll", "OutOfMemoryException", raiseOutOfMemory),
        BSX_CX_ENTRY("vccorlib140_app.dll", "AccessDeniedException", raiseAccessDenied),
        BSX_CX_ENTRY("vccorlib140_app.dll", "FailureException", raiseFailure),
        BSX_CX_ENTRY("vccorlib140_app.dll", "OperationCanceledException", raiseOperationCanceled),
        BSX_CX_ENTRY("vccorlib140_app.dll", "NullReferenceException", raiseNullReference),
        BSX_CX_ENTRY("vccorlib140_app.dll", "InvalidCastException", raiseInvalidCast),
        BSX_CX_ENTRY("vccorlib140_app.dll", "NotImplementedException", raiseNotImplemented),
        BSX_CX_ENTRY("vccorlib140.dll", "DisconnectedException", raiseDisconnected),
        BSX_CX_ENTRY("vccorlib140.dll", "WrongThreadException", raiseWrongThread),
        BSX_CX_ENTRY("vccorlib140.dll", "ObjectDisposedException", raiseObjectDisposed),
        BSX_CX_ENTRY("vccorlib140.dll", "ClassNotRegisteredException", raiseClassNotRegistered),
        BSX_CX_ENTRY("vccorlib140.dll", "ChangedStateException", raiseChangedState),
        BSX_CX_ENTRY("vccorlib140.dll", "OutOfBoundsException", raiseOutOfBounds),
        BSX_CX_ENTRY("vccorlib140.dll", "InvalidArgumentException", raiseInvalidArgument),
        BSX_CX_ENTRY("vccorlib140.dll", "OutOfMemoryException", raiseOutOfMemory),
        BSX_CX_ENTRY("vccorlib140.dll", "AccessDeniedException", raiseAccessDenied),
        BSX_CX_ENTRY("vccorlib140.dll", "FailureException", raiseFailure),
        BSX_CX_ENTRY("vccorlib140.dll", "OperationCanceledException", raiseOperationCanceled),
        BSX_CX_ENTRY("vccorlib140.dll", "NullReferenceException", raiseNullReference),
        BSX_CX_ENTRY("vccorlib140.dll", "InvalidCastException", raiseInvalidCast),
        BSX_CX_ENTRY("vccorlib140.dll", "NotImplementedException", raiseNotImplemented)
    );
#undef BSX_CX_ENTRY
}
