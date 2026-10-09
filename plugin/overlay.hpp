// Direct3D 9 hooks (EndScene/Reset) and a small text/panel renderer for the RPG overlay.
#pragma once

#include <d3d9.h>

#include <string>
#include <vector>

#include "ui.hpp"

namespace tfuplugin {

struct OverlayCallbacks {
    void (*onFrame)(IDirect3DDevice9* dev) = nullptr;  // once per EndScene on the back buffer
    void (*onLost)() = nullptr;                        // before IDirect3DDevice9::Reset
    void (*onReset)(IDirect3DDevice9* dev) = nullptr;  // after a successful Reset
};

// Hooks the d3d9 device functions used by the game. Returns false (and explains in `log`) if neither the
// hot-patch nor the vtable route works; the plugin then runs without an overlay.
bool installD3D9Hooks(const OverlayCallbacks& cb, std::string& log);

class TextRenderer {
public:
    bool ensure(IDirect3DDevice9* dev, std::string* err);  // lazily creates the font for this device
    void lost();
    void reset();
    void release();
    // Draws lines top-down starting at (x, y). With `panelWidth` > 0 a translucent panel goes behind them.
    void draw(IDirect3DDevice9* dev, int x, int y, const std::vector<UiLine>& lines, int panelWidth = 0);
    int lineHeight() const { return lineHeight_; }

private:
    void fillRect(IDirect3DDevice9* dev, int x, int y, int w, int h, D3DCOLOR c);

    struct ID3DXFont* font_ = nullptr;
    IDirect3DStateBlock9* state_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    int lineHeight_ = 18;
    bool failed_ = false;
};

}  // namespace tfuplugin
