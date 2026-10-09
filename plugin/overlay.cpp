#include "overlay.hpp"

#include <windows.h>
// d3dx9 is used only through interfaces and a GetProcAddress'd factory, so no d3dx9 import library is
// needed and the plugin still loads (without text) on systems missing the DirectX 9 redistributable.
#include <d3dx9core.h>

#include <cstdint>
#include <cstring>

namespace tfuplugin {

namespace {

OverlayCallbacks g_cb;

using EndSceneFn = HRESULT(WINAPI*)(IDirect3DDevice9*);
using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
EndSceneFn g_endScene = nullptr;
ResetFn g_reset = nullptr;

constexpr int kVtReset = 16;
constexpr int kVtEndScene = 42;

bool isBackBuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* bb = nullptr;
    bool same = false;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        same = rt == bb;
    if (rt) rt->Release();
    if (bb) bb->Release();
    return same;
}

HRESULT WINAPI hkEndScene(IDirect3DDevice9* dev) {
    // Skip render-to-texture passes so the menu isn't drawn into mirrors or shadow maps.
    if (g_cb.onFrame && isBackBuffer(dev)) g_cb.onFrame(dev);
    return g_endScene(dev);
}

HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_cb.onLost) g_cb.onLost();
    HRESULT hr = g_reset(dev, pp);
    if (SUCCEEDED(hr) && g_cb.onReset) g_cb.onReset(dev);
    return hr;
}

// Windows' hot-patch convention: "mov edi, edi" at the entry and 5 bytes of padding before it.
// We put a long jump in the padding and a 2-byte jump back to it at the entry; the original continues at +2.
bool hotPatch(void* fn, void* detour, void** original) {
    auto* p = static_cast<uint8_t*>(fn);
    if (p[0] != 0x8B || p[1] != 0xFF) return false;
    for (int i = 1; i <= 5; ++i)
        if (p[-i] != 0x90 && p[-i] != 0xCC) return false;
    DWORD old = 0;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[-5] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(detour) - reinterpret_cast<intptr_t>(p));
    std::memcpy(p - 4, &rel, 4);
    *original = p + 2;
    // Single aligned 16-bit store: "jmp short -7".
    *reinterpret_cast<volatile uint16_t*>(p) = 0xF9EB;
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return true;
}

bool vtablePatch(void** vtable, int index, void* detour, void** original) {
    DWORD old = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = vtable[index];
    vtable[index] = detour;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return true;
}

}  // namespace

bool installD3D9Hooks(const OverlayCallbacks& cb, std::string& log) {
    g_cb = cb;
    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    if (!create) {
        log = "d3d9.dll / Direct3DCreate9 не найдены";
        return false;
    }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) {
        log = "Direct3DCreate9 вернул NULL";
        return false;
    }
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "tfu_rpg_dummy";
    RegisterClassExA(&wc);
    HWND wnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev);
    if (FAILED(hr) || !dev) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    bool ok = false;
    if (SUCCEEDED(hr) && dev) {
        void** vt = *reinterpret_cast<void***>(dev);
        void* endScene = vt[kVtEndScene];
        void* reset = vt[kVtReset];
        bool hpEnd = hotPatch(endScene, reinterpret_cast<void*>(&hkEndScene), reinterpret_cast<void**>(&g_endScene));
        bool hpReset = hotPatch(reset, reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&g_reset));
        // Fallback (DXVK, Wine, wrappers without hot-patch padding): patch the shared device vtable.
        if (!hpEnd) hpEnd = vtablePatch(vt, kVtEndScene, reinterpret_cast<void*>(&hkEndScene), reinterpret_cast<void**>(&g_endScene));
        if (!hpReset) hpReset = vtablePatch(vt, kVtReset, reinterpret_cast<void*>(&hkReset), reinterpret_cast<void**>(&g_reset));
        ok = hpEnd && hpReset;
        log = ok ? "хуки D3D9 установлены" : "не удалось установить хуки D3D9";
        dev->Release();
    } else {
        log = "не удалось создать временное устройство D3D9";
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    return ok;
}

// ---------------------------------------------------------------- text renderer

namespace {

using CreateFontWFn = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCWSTR,
                                        LPD3DXFONT*);

CreateFontWFn findCreateFont() {
    static CreateFontWFn fn = nullptr;
    static bool searched = false;
    if (searched) return fn;
    searched = true;
    // The game ships an older d3dx9; any version exports the same function.
    for (int v = 43; v >= 24 && !fn; --v) {
        char name[32];
        wsprintfA(name, "d3dx9_%d.dll", v);
        if (HMODULE m = LoadLibraryA(name)) fn = reinterpret_cast<CreateFontWFn>(GetProcAddress(m, "D3DXCreateFontW"));
    }
    return fn;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR c;
};

}  // namespace

bool TextRenderer::ensure(IDirect3DDevice9* dev, std::string* err) {
    if (device_ != dev) {
        release();
        device_ = dev;
        failed_ = false;
    }
    if (font_ || failed_) return font_ != nullptr;
    CreateFontWFn create = findCreateFont();
    if (!create) {
        failed_ = true;
        if (err) *err = "D3DXCreateFontW не найден (установите DirectX End-User Runtime): меню недоступно, HUD — только в логе";
        return false;
    }
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    lineHeight_ = vp.Height > 0 ? static_cast<int>(vp.Height / 42) : 18;
    if (lineHeight_ < 14) lineHeight_ = 14;
    if (FAILED(create(dev, lineHeight_ - 2, 0, FW_SEMIBOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI", &font_))) {
        failed_ = true;
        if (err) *err = "D3DXCreateFontW завершился ошибкой";
        return false;
    }
    dev->CreateStateBlock(D3DSBT_ALL, &state_);
    return true;
}

void TextRenderer::lost() {
    if (font_) font_->OnLostDevice();
    if (state_) {
        state_->Release();
        state_ = nullptr;
    }
}

void TextRenderer::reset() {
    if (font_) font_->OnResetDevice();
    if (device_ && font_) device_->CreateStateBlock(D3DSBT_ALL, &state_);
}

void TextRenderer::release() {
    if (font_) font_->Release();
    if (state_) state_->Release();
    font_ = nullptr;
    state_ = nullptr;
    device_ = nullptr;
}

void TextRenderer::fillRect(IDirect3DDevice9* dev, int x, int y, int w, int h, D3DCOLOR c) {
    Vertex v[4] = {
        {float(x), float(y), 0, 1, c},
        {float(x + w), float(y), 0, 1, c},
        {float(x), float(y + h), 0, 1, c},
        {float(x + w), float(y + h), 0, 1, c},
    };
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetTexture(0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}

void TextRenderer::draw(IDirect3DDevice9* dev, int x, int y, const std::vector<UiLine>& lines, int panelWidth) {
    if (!font_ || lines.empty()) return;
    if (state_) state_->Capture();
    if (panelWidth > 0) fillRect(dev, x - 12, y - 10, panelWidth + 24, static_cast<int>(lines.size()) * lineHeight_ + 20, D3DCOLOR_ARGB(200, 8, 10, 18));
    int cy = y;
    for (const auto& l : lines) {
        std::wstring w = widen(l.text);
        RECT shadow{x + 1, cy + 1, x + 4000, cy + lineHeight_ + 1};
        font_->DrawTextW(nullptr, w.c_str(), -1, &shadow, DT_NOCLIP | DT_LEFT | DT_TOP, D3DCOLOR_ARGB(220, 0, 0, 0));
        RECT r{x, cy, x + 4000, cy + lineHeight_};
        font_->DrawTextW(nullptr, w.c_str(), -1, &r, DT_NOCLIP | DT_LEFT | DT_TOP, l.color);
        cy += lineHeight_;
    }
    if (state_) state_->Apply();
}

}  // namespace tfuplugin
