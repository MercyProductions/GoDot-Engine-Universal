#pragma once

#include <cstdint>

extern "C" int GodotSafe_ProbeGetProcAddress(
    void* candidate,
    std::uintptr_t mainStart,
    std::uintptr_t mainEnd,
    void** outMethodBind,
    void** outPtrcall,
    void** outGlobalGet);

extern "C" int GodotSafe_BuildStringName(
    void* stringNew,
    void* variantCtor,
    std::int32_t stringNameVariantType,
    const char* utf8,
    char* outStringName,
    int outStringNameBytes);

extern "C" int GodotSafe_CallGetInterfaceFunction(
    void* impl,
    const void* stringNameBuf,
    std::uintptr_t mainStart,
    std::uintptr_t mainEnd,
    void** outResolved);
