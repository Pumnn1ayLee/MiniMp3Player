#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "Mp3Player.h"

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static Mp3Player* g_player = nullptr;
static char g_fileInputBuf[512] = "";

// Win32 宽字符转 UTF-8（保证中文路径拖拽不乱码）
static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(len > 0 ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], len, nullptr, nullptr);
    return str;
}

static std::string formatTime(double seconds) {
    if (seconds < 0 || std::isnan(seconds)) seconds = 0;
    int totalSec = static_cast<int>(seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", totalSec / 60, totalSec % 60);
    return std::string(buf);
}

// 呼出 Windows 标准文件选择对话框
static std::string OpenMp3FileDialog(HWND owner) {
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    // 限制只能选择音频文件
    ofn.lpstrFilter = L"音频文件\0*.mp3;*.wav;*.flac\0所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    // 不改变当前工作目录，必须存在的文件
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return WideToUtf8(ofn.lpstrFile); // 转成 UTF-8 给底层 FFmpeg 识别
    }
    return "";
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) {
        return false;
    }
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DROPFILES: {
        // 支持将桌面的 MP3 文件直接拖进窗口播放
        HDROP hDrop = (HDROP)wParam;
        wchar_t filePathW[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, filePathW, MAX_PATH)) {
            std::string utf8Path = WideToUtf8(filePathW);
            std::snprintf(g_fileInputBuf, sizeof(g_fileInputBuf), "%s", utf8Path.c_str());
            if (g_player && g_player->load(utf8Path)) {
                g_player->play();
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void applyMinimalistTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding  = 6.0f;
    style.GrabRounding   = 6.0f;
    style.WindowPadding  = ImVec2(20.0f, 18.0f);
    style.FramePadding   = ImVec2(10.0f, 6.0f);
    style.ItemSpacing    = ImVec2(10.0f, 12.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]         = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.24f, 0.25f, 0.29f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.22f, 0.52f, 0.96f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.32f, 0.60f, 0.98f, 1.00f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.16f, 0.44f, 0.85f, 1.00f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.28f, 0.58f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.68f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLines]        = ImVec4(0.32f, 0.65f, 1.00f, 1.00f);
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"MiniMp3PlayerClass", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Mini MP3 Player",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 290, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // 开启文件拖拽
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // 加载 Windows 微软雅黑字体支持中文显示
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16.5f, nullptr, io.Fonts->GetGlyphRangesChineseFull());

    applyMinimalistTheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    Mp3Player player;
    g_player = &player;

    if (argc >= 2) {
        std::snprintf(g_fileInputBuf, sizeof(g_fileInputBuf), "%s", argv[1]);
        if (player.load(argv[1])) player.play();
    }

    bool done = false;
    bool isDraggingSeek = false;
    float seekPreviewSec = 0.0f;
    std::vector<float> waveform;

    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 快捷键：空格暂停/继续
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            player.togglePause();
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("PlayerPanel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // 1. 顶部：文件路径输入栏 + 加载按钮
        ImGui::PushItemWidth(-140.0f);
        ImGui::InputTextWithHint("##filepath", "输入 MP3 路径，或直接将文件拖入窗口...", g_fileInputBuf, sizeof(g_fileInputBuf));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("加载", ImVec2(65.0f, 0.0f))) {
            if (player.load(g_fileInputBuf)) player.play();
        }

        ImGui::SameLine();
        // ★ 这里是新加的导入按钮逻辑
        if (ImGui::Button("导入", ImVec2(55.0f, 0.0f))) {
            std::string selectedFile = OpenMp3FileDialog(hwnd);
            if (!selectedFile.empty()) {
                // 如果用户没有点取消，就把路径填进输入框，并直接开始播放
                std::snprintf(g_fileInputBuf, sizeof(g_fileInputBuf), "%s", selectedFile.c_str());
                if (player.load(g_fileInputBuf)) {
                    player.play();
                }
            }
        }
        // 2. 当前曲目与实时音频波形图
        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.92f, 1.0f), "正在播放: %s", player.getFileName().c_str());

        player.getWaveform(waveform, 128);
        ImGui::PlotLines("##Waveform", waveform.data(), static_cast<int>(waveform.size()),
                         0, nullptr, -1.0f, 1.0f, ImVec2(-1.0f, 52.0f));

        // 3. 播放进度条（支持拖拽 Seek）
        float duration = static_cast<float>(player.getDurationSec());
        float current  = isDraggingSeek ? seekPreviewSec : static_cast<float>(player.getCurrentTimeSec());

        std::string timeLabel = formatTime(current) + " / " + formatTime(duration);
        ImGui::PushItemWidth(-1.0f);
        if (ImGui::SliderFloat("##Seek", &current, 0.0f, std::max(duration, 0.1f), timeLabel.c_str())) {
            isDraggingSeek = true;
            seekPreviewSec = current;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            player.seek(seekPreviewSec);
            isDraggingSeek = false;
        }
        ImGui::PopItemWidth();

        // 4. 底部控制栏
        const char* playBtnText = player.isPlaying() ? " 暂停 (Space) " : " 播放 (Space) ";
        if (ImGui::Button(playBtnText, ImVec2(120.0f, 34.0f))) {
            if (!player.isLoaded() && g_fileInputBuf[0] != '\0') {
                if (player.load(g_fileInputBuf)) player.play();
            } else {
                player.togglePause();
            }
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.30f, 0.34f, 1.0f));
        if (ImGui::Button(" 停止 ", ImVec2(75.0f, 34.0f))) {
            player.stop();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, 25.0f);
        float vol = player.getVolume() * 100.0f;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##Volume", &vol, 0.0f, 100.0f, "音量 %.0f%%")) {
            player.setVolume(vol / 100.0f);
        }

        ImGui::End();

        ImGui::Render();
        const float clear_color[4] = { 0.11f, 0.12f, 0.14f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // 开启垂直同步
    }

    g_player = nullptr;
    player.stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}