#pragma once
#include <cstdint>
typedef void* jobject; typedef void* jclass; typedef void* jmethodID; typedef int jboolean; enum { JNI_TRUE = 1 };
struct JNIEnv { jclass GetObjectClass(jobject){return nullptr;} jmethodID GetMethodID(jclass,const char*,const char*){return nullptr;}
  jboolean CallBooleanMethod(jobject, jmethodID){return 1;} bool ExceptionCheck(){return false;} void ExceptionClear(){} void DeleteLocalRef(void*){} };
struct FakeAML { void* GetLibHandle(const char*){return nullptr;} uintptr_t GetSym(void*,const char*){return 0;} JNIEnv* GetJNIEnvironment(){return nullptr;} jobject GetCurrentActivity(){return nullptr;} void GetDisplaySize(int*w,int*h){*w=1600;*h=720;} };
extern FakeAML* aml;
#define DECL_HOOKv(_name, ...) void (*_name)(__VA_ARGS__); void HookOf_##_name(__VA_ARGS__)
#define DECL_HOOKb(_name, ...) bool (*_name)(__VA_ARGS__); bool HookOf_##_name(__VA_ARGS__)
#define HOOK(_name, _addr) do{}while(0)
// resolver shim: GameSymbols::ResolveAll is never called in the harness, but must compile
struct FakeAML2 {};
