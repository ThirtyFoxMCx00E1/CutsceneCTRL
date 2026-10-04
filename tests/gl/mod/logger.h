#pragma once
#include <cstdio>
struct FakeLogger { template<class... A> void Info(const char* f, A... a) { printf("    [log] "); printf(f, a...); printf("\n"); } };
extern FakeLogger* logger;
