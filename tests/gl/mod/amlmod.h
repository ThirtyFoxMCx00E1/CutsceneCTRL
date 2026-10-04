#pragma once
#include <cstdint>
#define BYBIT(a32, a64) (a64)
struct FakeAML { void* GetLibHandle(const char*) { return nullptr; } uintptr_t GetSym(void*, const char*) { return 0; } void GetDisplaySize(int* w, int* h) { *w = 1600; *h = 720; } };
extern FakeAML* aml;
