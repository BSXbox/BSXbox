#include <windows.h>

__declspec(thread) unsigned char g_tlshostanchor = 0;
extern "C" __declspec(dllexport) void tlshostkeep() {g_tlshostanchor = 1;}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {return TRUE;}