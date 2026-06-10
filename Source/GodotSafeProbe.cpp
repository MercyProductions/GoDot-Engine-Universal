#include "GodotSafeProbe.h"

#include <Windows.h>

#include <cstring>

namespace
{
    bool IsInImageRange(void* ptr, std::uintptr_t mainStart, std::uintptr_t mainEnd)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        return address >= mainStart && address < mainEnd;
    }

    using StringNewFn = void(__stdcall*)(void* rDest, const char* utf8);
    using VariantGetCtorFn = void*(__stdcall*)(std::int32_t type, std::int32_t index);
    using PtrConstructorFn = void(__stdcall*)(void* self, const void** args);
    using GetInterfaceFunctionFn = void*(__stdcall*)(const void* stringName);
}

extern "C" int GodotSafe_ProbeGetProcAddress(
    void* candidate,
    std::uintptr_t mainStart,
    std::uintptr_t mainEnd,
    void** outMethodBind,
    void** outPtrcall,
    void** outGlobalGet)
{
    if (!candidate || !outMethodBind || !outPtrcall || !outGlobalGet)
        return 0;

    *outMethodBind = nullptr;
    *outPtrcall = nullptr;
    *outGlobalGet = nullptr;

    __try
    {
        using GetProcAddressFn = void*(__stdcall*)(const char*);
        GetProcAddressFn probe = reinterpret_cast<GetProcAddressFn>(candidate);

        void* methodBind = probe("classdb_get_method_bind");
        void* ptrcall = probe("object_method_bind_ptrcall");
        void* globalGet = probe("global_get_singleton");

        if (!IsInImageRange(methodBind, mainStart, mainEnd) ||
            !IsInImageRange(ptrcall, mainStart, mainEnd) ||
            !IsInImageRange(globalGet, mainStart, mainEnd))
        {
            return 0;
        }

        *outMethodBind = methodBind;
        *outPtrcall = ptrcall;
        *outGlobalGet = globalGet;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

extern "C" int GodotSafe_BuildStringName(
    void* stringNew,
    void* variantCtor,
    std::int32_t stringNameVariantType,
    const char* utf8,
    char* outStringName,
    int outStringNameBytes)
{
    if (!stringNew || !variantCtor || !utf8 || !utf8[0] || !outStringName || outStringNameBytes < 64)
        return 0;

    std::memset(outStringName, 0, static_cast<std::size_t>(outStringNameBytes));

    __try
    {
        alignas(8) char stringBuf[64] = {};
        reinterpret_cast<StringNewFn>(stringNew)(stringBuf, utf8);
        if (!*reinterpret_cast<void* const*>(stringBuf))
            return 0;

        auto lookupCtor = reinterpret_cast<VariantGetCtorFn>(variantCtor);
        for (std::int32_t ctorIndex = 0; ctorIndex < 4; ++ctorIndex)
        {
            auto ctor = reinterpret_cast<PtrConstructorFn>(lookupCtor(stringNameVariantType, ctorIndex));
            if (!ctor)
                continue;

            std::memset(outStringName, 0, static_cast<std::size_t>(outStringNameBytes));
            const void* args[1] = { stringBuf };
            ctor(outStringName, args);

            if (outStringName[0] || outStringName[1] || outStringName[outStringNameBytes - 1])
                return 1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return 0;
}

extern "C" int GodotSafe_CallGetInterfaceFunction(
    void* impl,
    const void* stringNameBuf,
    std::uintptr_t mainStart,
    std::uintptr_t mainEnd,
    void** outResolved)
{
    if (!impl || !stringNameBuf || !outResolved)
        return 0;

    *outResolved = nullptr;

    __try
    {
        void* resolved = reinterpret_cast<GetInterfaceFunctionFn>(impl)(stringNameBuf);
        if (!resolved || !IsInImageRange(resolved, mainStart, mainEnd))
            return 0;

        *outResolved = resolved;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
