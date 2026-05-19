#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AegisUniversalOverlay.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "kiero.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern "C" const char* AegisUniversalOverlay_GetEngineOverlayName();
extern "C" void AegisUniversalOverlay_PollEngineProviders();
extern "C" void AegisUniversalOverlay_DrawEngineMenu();
extern "C" void AegisUniversalOverlay_DrawEngineOverlay();

namespace
{
    constexpr UINT kD3D11PresentIndex = 8;
    constexpr UINT kD3D11ResizeBuffersIndex = 13;
    constexpr UINT kD3D9ResetIndex = 16;
    constexpr UINT kD3D9EndSceneIndex = 42;

    using D3D11PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using D3D11ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using D3D9EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
    using D3D9ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using SwapBuffersFn = BOOL(WINAPI*)(HDC);

    enum class ActiveBackend
    {
        None,
        D3D9,
        D3D11,
        OpenGL
    };

    std::mutex g_stateMutex;
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_hooksInstalled{false};
    std::atomic<bool> g_menuVisible{true};
    std::atomic<bool> g_f4WasDown{false};
    std::wstring g_status = L"not started";
    std::wstring g_detectedBackends = L"none";
    std::wstring g_selectedBackend = L"None";
    ActiveBackend g_activeBackend = ActiveBackend::None;

    HWND g_hwnd = nullptr;
    WNDPROC g_originalWndProc = nullptr;
    bool g_win32Initialized = false;
    bool g_imguiContextReady = false;
    bool g_imguiRendererReady = false;
    std::uint64_t g_presentCount = 0;
    std::uint64_t g_resizeCount = 0;
    std::uint32_t g_backbufferWidth = 0;
    std::uint32_t g_backbufferHeight = 0;
    std::uint32_t g_backbufferFormat = 0;

    D3D11PresentFn g_originalD3D11Present = nullptr;
    D3D11ResizeBuffersFn g_originalD3D11ResizeBuffers = nullptr;
    IDXGISwapChain* g_d3d11SwapChain = nullptr;
    ID3D11Device* g_d3d11Device = nullptr;
    ID3D11DeviceContext* g_d3d11Context = nullptr;
    ID3D11RenderTargetView* g_d3d11RenderTarget = nullptr;

    D3D9EndSceneFn g_originalD3D9EndScene = nullptr;
    D3D9ResetFn g_originalD3D9Reset = nullptr;
    IDirect3DDevice9* g_d3d9Device = nullptr;

    SwapBuffersFn g_originalSwapBuffers = nullptr;
    void* g_swapBuffersTarget = nullptr;

    void SetStatus(const std::wstring& value)
    {
        std::lock_guard lock(g_stateMutex);
        g_status = value;
    }

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const std::wstring& value)
    {
        wcsncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    std::wstring ModuleList()
    {
        std::wstring result;
        auto append = [&result](const wchar_t* name) {
            if (::GetModuleHandleW(name))
            {
                if (!result.empty())
                    result += L", ";
                result += name;
            }
        };

        append(L"d3d11.dll");
        append(L"d3d9.dll");
        append(L"opengl32.dll");
        append(L"d3d12.dll");
        append(L"vulkan-1.dll");
        append(L"dxgi.dll");
        return result.empty() ? L"none" : result;
    }

    void RefreshDetectedBackends()
    {
        std::lock_guard lock(g_stateMutex);
        g_detectedBackends = ModuleList();
    }

    const wchar_t* BackendName(ActiveBackend backend)
    {
        switch (backend)
        {
        case ActiveBackend::D3D9:
            return L"Direct3D9";
        case ActiveBackend::D3D11:
            return L"Direct3D11";
        case ActiveBackend::OpenGL:
            return L"OpenGL";
        default:
            return L"None";
        }
    }

    void SetSelectedBackend(ActiveBackend backend)
    {
        std::lock_guard lock(g_stateMutex);
        g_activeBackend = backend;
        g_selectedBackend = BackendName(backend);
    }

    bool EnsureImGuiContext()
    {
        if (g_imguiContextReady)
            return true;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 3.0f;
        style.FrameRounding = 2.0f;
        style.WindowBorderSize = 1.0f;
        g_imguiContextReady = true;
        return true;
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_KEYUP && wParam == VK_F4)
        {
            AegisUniversalOverlay_ToggleMenu();
            return 0;
        }

        if (g_menuVisible.load() && g_imguiContextReady)
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
                return 1;
        }

        return g_originalWndProc ? ::CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam)
                                 : ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureWin32(HWND hwnd)
    {
        if (!hwnd)
            return false;

        if (g_win32Initialized && g_hwnd == hwnd)
            return true;

        EnsureImGuiContext();
        g_hwnd = hwnd;
        ImGui_ImplWin32_Init(hwnd);
        g_win32Initialized = true;

#ifdef _WIN64
        g_originalWndProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWndProc)));
#else
        g_originalWndProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OverlayWndProc)));
#endif
        return true;
    }

    void PollHotkey()
    {
        const bool f4Down = (::GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        const bool wasDown = g_f4WasDown.exchange(f4Down);
        if (f4Down && !wasDown)
            AegisUniversalOverlay_ToggleMenu();
    }

    void DrawCoreMenu()
    {
        if (!g_menuVisible.load())
            return;

        bool open = true;
        ImGui::SetNextWindowSize(ImVec2(780.0f, 560.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Aegis Universal", &open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            g_menuVisible.store(open);
            return;
        }

        ImGui::TextUnformatted(AegisUniversal_GetBrandAsciiArt());
        ImGui::Separator();
        ImGui::Text("Hotkey: F4");
        ImGui::Text("Engine: %ls", AegisUniversal_GetEngineName());

        AegisUniversalRuntimeInfo runtime = {};
        if (AegisUniversal_GetRuntimeInfo(&runtime))
        {
            ImGui::Text("Process: %ls", runtime.processName);
            ImGui::Text("Modules: %u | matched modules: %u | exports: %u | flags: 0x%08x",
                runtime.moduleCount,
                runtime.matchedModuleCount,
                runtime.matchedExportCount,
                runtime.flags);
        }

        std::wstring status;
        std::wstring selected;
        std::wstring detected;
        {
            std::lock_guard lock(g_stateMutex);
            status = g_status;
            selected = g_selectedBackend;
            detected = g_detectedBackends;
        }

        ImGui::Text("Selected backend: %ls", selected.c_str());
        ImGui::Text("Detected render modules: %ls", detected.c_str());
        ImGui::TextWrapped("Bridge status: %ls", status.c_str());
        ImGui::Text("HWND: 0x%p | frames: %llu | resizes: %llu | backbuffer: %ux%u",
            static_cast<void*>(g_hwnd),
            static_cast<unsigned long long>(g_presentCount),
            static_cast<unsigned long long>(g_resizeCount),
            g_backbufferWidth,
            g_backbufferHeight);

        if (ImGui::BeginTabBar("AegisUniversalTabs"))
        {
            if (ImGui::BeginTabItem("Runtime"))
            {
                ImGui::Text("Overlay running: %s", g_running.load() ? "yes" : "no");
                ImGui::Text("Hooks installed: %s", g_hooksInstalled.load() ? "yes" : "no");
                ImGui::Text("Win32 backend: %s", g_win32Initialized ? "ready" : "waiting");
                ImGui::Text("Renderer backend: %s", g_imguiRendererReady ? "ready" : "waiting");
                ImGui::Text("Loaded SDK resolver exports: %u", AegisUniversal_GetLoadedSdkExportCount());
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Renderer"))
            {
                ImGui::Text("Direct3D11: %s", ::GetModuleHandleW(L"d3d11.dll") ? "detected" : "not loaded");
                ImGui::Text("Direct3D9: %s", ::GetModuleHandleW(L"d3d9.dll") ? "detected" : "not loaded");
                ImGui::Text("OpenGL: %s", ::GetModuleHandleW(L"opengl32.dll") ? "detected" : "not loaded");
                ImGui::Text("Direct3D12: %s", ::GetModuleHandleW(L"d3d12.dll") ? "detected, bridge not enabled" : "not loaded");
                ImGui::Text("Vulkan: %s", ::GetModuleHandleW(L"vulkan-1.dll") ? "detected, bridge not enabled" : "not loaded");
                ImGui::EndTabItem();
            }

            AegisUniversalOverlay_DrawEngineMenu();
            ImGui::EndTabBar();
        }

        ImGui::End();
        g_menuVisible.store(open);
    }

    void DrawFrame()
    {
        PollHotkey();
        AegisUniversalOverlay_PollEngineProviders();
        AegisUniversalOverlay_DrawEngineOverlay();
        DrawCoreMenu();
    }

    void CleanupD3D11RenderTarget()
    {
        if (g_d3d11RenderTarget)
        {
            g_d3d11RenderTarget->Release();
            g_d3d11RenderTarget = nullptr;
        }
    }

    bool CreateD3D11RenderTarget(IDXGISwapChain* swapChain)
    {
        if (g_d3d11RenderTarget || !swapChain || !g_d3d11Device)
            return g_d3d11RenderTarget != nullptr;

        ID3D11Texture2D* backbuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer))) || !backbuffer)
            return false;

        D3D11_TEXTURE2D_DESC desc = {};
        backbuffer->GetDesc(&desc);
        g_backbufferWidth = desc.Width;
        g_backbufferHeight = desc.Height;
        g_backbufferFormat = static_cast<std::uint32_t>(desc.Format);

        const HRESULT hr = g_d3d11Device->CreateRenderTargetView(backbuffer, nullptr, &g_d3d11RenderTarget);
        backbuffer->Release();
        return SUCCEEDED(hr) && g_d3d11RenderTarget;
    }

    bool InitializeD3D11(IDXGISwapChain* swapChain)
    {
        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D11)
            return true;

        DXGI_SWAP_CHAIN_DESC swapDesc = {};
        if (FAILED(swapChain->GetDesc(&swapDesc)) || !swapDesc.OutputWindow)
            return false;

        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_d3d11Device))) || !g_d3d11Device)
            return false;

        g_d3d11Device->GetImmediateContext(&g_d3d11Context);
        if (!g_d3d11Context)
            return false;

        g_d3d11SwapChain = swapChain;
        if (!EnsureWin32(swapDesc.OutputWindow))
            return false;

        if (!ImGui_ImplDX11_Init(g_d3d11Device, g_d3d11Context))
            return false;

        CreateD3D11RenderTarget(swapChain);
        g_imguiRendererReady = true;
        SetSelectedBackend(ActiveBackend::D3D11);
        SetStatus(L"D3D11 Present captured; internal ImGui renderer is active");
        return true;
    }

    HRESULT __stdcall HookD3D11Present(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
    {
        ++g_presentCount;
        if (InitializeD3D11(swapChain) && CreateD3D11RenderTarget(swapChain))
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawFrame();
            ImGui::Render();
            g_d3d11Context->OMSetRenderTargets(1, &g_d3d11RenderTarget, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return g_originalD3D11Present ? g_originalD3D11Present(swapChain, syncInterval, flags) : S_OK;
    }

    HRESULT __stdcall HookD3D11ResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        ++g_resizeCount;
        CleanupD3D11RenderTarget();
        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D11)
            ImGui_ImplDX11_InvalidateDeviceObjects();

        const HRESULT hr = g_originalD3D11ResizeBuffers ?
            g_originalD3D11ResizeBuffers(swapChain, bufferCount, width, height, format, flags) :
            S_OK;

        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D11)
            ImGui_ImplDX11_CreateDeviceObjects();
        return hr;
    }

    bool InitializeD3D9(IDirect3DDevice9* device)
    {
        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D9)
            return true;

        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(device->GetCreationParameters(&params)) || !params.hFocusWindow)
            return false;

        if (!EnsureWin32(params.hFocusWindow))
            return false;

        g_d3d9Device = device;
        g_d3d9Device->AddRef();
        if (!ImGui_ImplDX9_Init(device))
            return false;

        g_imguiRendererReady = true;
        SetSelectedBackend(ActiveBackend::D3D9);
        SetStatus(L"D3D9 EndScene captured; internal ImGui renderer is active");
        return true;
    }

    HRESULT __stdcall HookD3D9EndScene(IDirect3DDevice9* device)
    {
        ++g_presentCount;
        if (InitializeD3D9(device))
        {
            D3DVIEWPORT9 vp = {};
            if (SUCCEEDED(device->GetViewport(&vp)))
            {
                g_backbufferWidth = vp.Width;
                g_backbufferHeight = vp.Height;
            }

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawFrame();
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }
        return g_originalD3D9EndScene ? g_originalD3D9EndScene(device) : S_OK;
    }

    HRESULT __stdcall HookD3D9Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
    {
        ++g_resizeCount;
        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D9)
            ImGui_ImplDX9_InvalidateDeviceObjects();

        const HRESULT hr = g_originalD3D9Reset ? g_originalD3D9Reset(device, params) : S_OK;
        if (SUCCEEDED(hr) && g_imguiRendererReady && g_activeBackend == ActiveBackend::D3D9)
            ImGui_ImplDX9_CreateDeviceObjects();
        return hr;
    }

    bool InitializeOpenGL(HDC hdc)
    {
        if (g_imguiRendererReady && g_activeBackend == ActiveBackend::OpenGL)
            return true;

        HWND hwnd = ::WindowFromDC(hdc);
        if (!hwnd)
            return false;

        if (!EnsureWin32(hwnd))
            return false;

        if (!ImGui_ImplOpenGL3_Init("#version 130"))
            return false;

        g_imguiRendererReady = true;
        SetSelectedBackend(ActiveBackend::OpenGL);
        SetStatus(L"OpenGL SwapBuffers captured; internal ImGui renderer is active");
        return true;
    }

    BOOL WINAPI HookSwapBuffers(HDC hdc)
    {
        ++g_presentCount;
        if (InitializeOpenGL(hdc))
        {
            RECT rect = {};
            if (g_hwnd && ::GetClientRect(g_hwnd, &rect))
            {
                g_backbufferWidth = static_cast<std::uint32_t>(std::max<LONG>(0, rect.right - rect.left));
                g_backbufferHeight = static_cast<std::uint32_t>(std::max<LONG>(0, rect.bottom - rect.top));
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawFrame();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        return g_originalSwapBuffers ? g_originalSwapBuffers(hdc) : FALSE;
    }

    bool TryInstallD3D11()
    {
        if (!::GetModuleHandleW(L"d3d11.dll"))
            return false;

        if (kiero::init(kiero::RenderType::D3D11) != kiero::Status::Success)
            return false;

        if (kiero::bind(kD3D11PresentIndex, reinterpret_cast<void**>(&g_originalD3D11Present), reinterpret_cast<void*>(HookD3D11Present)) != kiero::Status::Success)
        {
            kiero::shutdown();
            return false;
        }
        if (kiero::bind(kD3D11ResizeBuffersIndex, reinterpret_cast<void**>(&g_originalD3D11ResizeBuffers), reinterpret_cast<void*>(HookD3D11ResizeBuffers)) != kiero::Status::Success)
        {
            kiero::shutdown();
            return false;
        }

        g_hooksInstalled.store(true);
        SetSelectedBackend(ActiveBackend::D3D11);
        SetStatus(L"D3D11 hooks installed; waiting for first Present");
        return true;
    }

    bool TryInstallD3D9()
    {
        if (!::GetModuleHandleW(L"d3d9.dll"))
            return false;

        if (kiero::init(kiero::RenderType::D3D9) != kiero::Status::Success)
            return false;

        if (kiero::bind(kD3D9EndSceneIndex, reinterpret_cast<void**>(&g_originalD3D9EndScene), reinterpret_cast<void*>(HookD3D9EndScene)) != kiero::Status::Success)
        {
            kiero::shutdown();
            return false;
        }
        if (kiero::bind(kD3D9ResetIndex, reinterpret_cast<void**>(&g_originalD3D9Reset), reinterpret_cast<void*>(HookD3D9Reset)) != kiero::Status::Success)
        {
            kiero::shutdown();
            return false;
        }

        g_hooksInstalled.store(true);
        SetSelectedBackend(ActiveBackend::D3D9);
        SetStatus(L"D3D9 hooks installed; waiting for first EndScene");
        return true;
    }

    bool TryInstallOpenGL()
    {
        if (!::GetModuleHandleW(L"opengl32.dll"))
            return false;

        HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
        if (!gdi)
            return false;

        g_swapBuffersTarget = reinterpret_cast<void*>(::GetProcAddress(gdi, "SwapBuffers"));
        if (!g_swapBuffersTarget)
            return false;

        const MH_STATUS initStatus = MH_Initialize();
        if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        if (MH_CreateHook(g_swapBuffersTarget, reinterpret_cast<void*>(HookSwapBuffers), reinterpret_cast<void**>(&g_originalSwapBuffers)) != MH_OK)
            return false;
        if (MH_EnableHook(g_swapBuffersTarget) != MH_OK)
            return false;

        g_hooksInstalled.store(true);
        SetSelectedBackend(ActiveBackend::OpenGL);
        SetStatus(L"OpenGL SwapBuffers hook installed; waiting for first frame");
        return true;
    }

    DWORD WINAPI InstallThread(void*)
    {
        RefreshDetectedBackends();
        g_running.store(true);

        for (int attempt = 0; attempt < 180 && g_running.load(); ++attempt)
        {
            RefreshDetectedBackends();

            if (TryInstallD3D11() || TryInstallD3D9() || TryInstallOpenGL())
                return 0;

            if (::GetModuleHandleW(L"d3d12.dll") || ::GetModuleHandleW(L"vulkan-1.dll"))
                SetStatus(L"Vulkan/D3D12 detected; this build reports those backends but renders ImGui on D3D11, D3D9, or OpenGL");
            else
                SetStatus(L"waiting for D3D11, D3D9, or OpenGL runtime module");

            ::Sleep(500);
        }

        if (!g_hooksInstalled.load())
            SetStatus(L"no supported render bridge installed; detected modules: " + ModuleList());
        return 0;
    }

    void ShutdownRenderer()
    {
        if (g_imguiRendererReady)
        {
            if (g_activeBackend == ActiveBackend::D3D11)
                ImGui_ImplDX11_Shutdown();
            else if (g_activeBackend == ActiveBackend::D3D9)
                ImGui_ImplDX9_Shutdown();
            else if (g_activeBackend == ActiveBackend::OpenGL)
                ImGui_ImplOpenGL3_Shutdown();
        }
        g_imguiRendererReady = false;

        if (g_win32Initialized)
            ImGui_ImplWin32_Shutdown();
        g_win32Initialized = false;

        if (g_imguiContextReady)
            ImGui::DestroyContext();
        g_imguiContextReady = false;
    }
}

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_Start()
{
    if (g_running.exchange(true))
        return 1;

    if (HANDLE thread = ::CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr))
    {
        ::CloseHandle(thread);
        return 1;
    }

    g_running.store(false);
    SetStatus(L"failed to create overlay install thread");
    return 0;
}

AEGIS_UNIVERSAL_API void AegisUniversalOverlay_Stop()
{
    g_running.store(false);

    if (g_swapBuffersTarget)
        MH_DisableHook(g_swapBuffersTarget);

    kiero::shutdown();
    CleanupD3D11RenderTarget();

    if (g_d3d11Context)
    {
        g_d3d11Context->Release();
        g_d3d11Context = nullptr;
    }
    if (g_d3d11Device)
    {
        g_d3d11Device->Release();
        g_d3d11Device = nullptr;
    }
    if (g_d3d9Device)
    {
        g_d3d9Device->Release();
        g_d3d9Device = nullptr;
    }

    if (g_hwnd && g_originalWndProc)
        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
    g_originalWndProc = nullptr;

    ShutdownRenderer();
    g_hooksInstalled.store(false);
    g_d3d11SwapChain = nullptr;
    g_hwnd = nullptr;
    SetSelectedBackend(ActiveBackend::None);
    SetStatus(L"stopped");
}

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsRunning()
{
    return g_running.load() ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsInstalled()
{
    return g_hooksInstalled.load() ? 1 : 0;
}

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_IsMenuVisible()
{
    return g_menuVisible.load() ? 1 : 0;
}

AEGIS_UNIVERSAL_API void AegisUniversalOverlay_SetMenuVisible(int visible)
{
    g_menuVisible.store(visible != 0);
}

AEGIS_UNIVERSAL_API void AegisUniversalOverlay_ToggleMenu()
{
    g_menuVisible.store(!g_menuVisible.load());
}

AEGIS_UNIVERSAL_API const wchar_t* AegisUniversalOverlay_GetStatus()
{
    std::lock_guard lock(g_stateMutex);
    return g_status.c_str();
}

AEGIS_UNIVERSAL_API int AegisUniversalOverlay_GetInfo(AegisUniversalOverlayBridgeInfo* outInfo)
{
    if (!outInfo)
        return 0;

    *outInfo = {};
    outInfo->size = sizeof(AegisUniversalOverlayBridgeInfo);
    outInfo->running = g_running.load() ? 1 : 0;
    outInfo->hooksInstalled = g_hooksInstalled.load() ? 1 : 0;
    outInfo->swapchainCaptured = (g_d3d11SwapChain || g_d3d9Device || g_activeBackend == ActiveBackend::OpenGL) ? 1 : 0;
    outInfo->hwndFound = g_hwnd ? 1 : 0;
    outInfo->imguiInitialized = (g_imguiContextReady && g_win32Initialized && g_imguiRendererReady) ? 1 : 0;
    outInfo->renderTargetReady = (g_d3d11RenderTarget || g_activeBackend == ActiveBackend::D3D9 || g_activeBackend == ActiveBackend::OpenGL) ? 1 : 0;
    outInfo->menuVisible = g_menuVisible.load() ? 1 : 0;
    outInfo->presentCount = g_presentCount;
    outInfo->resizeCount = g_resizeCount;
    outInfo->hwnd = g_hwnd;
    outInfo->backbufferWidth = g_backbufferWidth;
    outInfo->backbufferHeight = g_backbufferHeight;
    outInfo->backbufferFormat = g_backbufferFormat;

    std::lock_guard lock(g_stateMutex);
    CopyWide(outInfo->selectedBackend, g_selectedBackend);
    CopyWide(outInfo->detectedBackends, g_detectedBackends);
    CopyWide(outInfo->status, g_status);
    return 1;
}
