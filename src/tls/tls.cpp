#include <windows.h>

#ifndef BSX_TLS_SIZE
#define BSX_TLS_SIZE 32768
#endif

extern "C" {
    __declspec(thread) unsigned char g_bsxTlsReserve[BSX_TLS_SIZE];

    __declspec(dllexport) unsigned char* bsxTlsReserveBase() { return g_bsxTlsReserve; }
    __declspec(dllexport) unsigned bsxTlsReserveSize() { return BSX_TLS_SIZE; }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
