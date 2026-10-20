#include "ui/fonts/CjkFont.h"
#include "Shared.h"

#include <imgui.h>
#include <d3d11.h>
#include <cstdio>
#include <string>

namespace
{
    ImFontAtlas*              g_atlas = nullptr;
    ImFont*                   g_font  = nullptr;
    ID3D11ShaderResourceView* g_srv   = nullptr;
    bool                      g_failed = false;

    constexpr float kPx = 32.0f;   // baked size: crisp for menu text, downscales cleanly to the 14-16 px body

    void LogWarn(const char* msg)
    {
        if (APIDefs && APIDefs->Log) APIDefs->Log(LOGL_WARNING, "Tyrian Codex", msg);
    }

    void LogInfo(const char* msg)
    {
        if (APIDefs && APIDefs->Log) APIDefs->Log(LOGL_INFO, "Tyrian Codex", msg);
    }

    bool FileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

    std::string AddonFontDir()
    {
        const char* root = AddonDir();
        return (root && *root) ? std::string(root) + "\\data\\fonts" : std::string("data\\fonts");
    }

    // Preference: a face dropped in next to the addon (redistributable, e.g. Noto Sans SC), then the CJK
    // faces every Chinese Windows ships. The .ttc ones are collections; index 0 is the regular weight.
    bool FindFace(std::string& path)
    {
        const std::string dir = AddonFontDir();
        const char* bundled[] = {
            "\\NotoSansSC-Regular.otf",
            "\\NotoSansCJKsc-Regular.otf",
            "\\SourceHanSansSC-Regular.otf",
            "\\SourceHanSansCN-Regular.otf",
        };
        for (const char* n : bundled)
        {
            const std::string p = dir + n;
            if (FileExists(p)) { path = p; return true; }
        }

        const char* system[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",    // Microsoft YaHei
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\Deng.ttf",    // DengXian
            "C:\\Windows\\Fonts\\simhei.ttf",  // SimHei
            "C:\\Windows\\Fonts\\simsun.ttc",  // SimSun
        };
        for (const char* p : system)
            if (FileExists(p)) { path = p; return true; }
        return false;
    }

    void ReleaseAll()
    {
        g_font = nullptr;
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        if (g_atlas) { IM_DELETE(g_atlas); g_atlas = nullptr; }
    }

    bool Build()
    {
        if (g_font) return true;
        if (g_failed || !APIDefs || !APIDefs->SwapChain) return false;

        std::string face;
        if (!FindFace(face))
        {
            g_failed = true;
            LogWarn("CJK font: no Chinese face found (looked for Noto/Source Han in data\\fonts and msyh.ttc in C:\\Windows\\Fonts).");
            return false;
        }

        IDXGISwapChain* swap = static_cast<IDXGISwapChain*>(APIDefs->SwapChain);
        ID3D11Device* device = nullptr;
        if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
        {
            g_failed = true;
            LogWarn("CJK font: failed to get the D3D11 device.");
            return false;
        }

        ImFontAtlas* atlas = IM_NEW(ImFontAtlas)();
        atlas->TexDesiredWidth = 2048;

        ImFontConfig cfg;
        cfg.FontNo       = 0;      // .ttc collections: face 0
        cfg.OversampleH  = 1;      // CJK glyphs are complex; 1 is the sane default for them
        cfg.OversampleV  = 1;
        cfg.PixelSnapH   = false;
        std::snprintf(cfg.Name, IM_ARRAYSIZE(cfg.Name), "TC CJK UI");

        // Common simplified Chinese (~2500 glyphs) + Basic Latin + CJK punctuation. Deliberately not the
        // full 20k block: that would need a 4096-wide atlas and seconds of CPU at startup.
        ImFont* font = atlas->AddFontFromFileTTF(face.c_str(), kPx, &cfg, atlas->GetGlyphRangesChineseSimplifiedCommon());
        if (!font)
        {
            IM_DELETE(atlas);
            device->Release();
            g_failed = true;
            LogWarn("CJK font: AddFontFromFileTTF failed for the chosen face.");
            return false;
        }

        if (!font->FindGlyphNoFallback((ImWchar)0x4E2D))   // '中'
        {
            IM_DELETE(atlas);
            device->Release();
            g_failed = true;
            LogWarn("CJK font: atlas has no U+4E2D -- the face carries no Chinese glyphs.");
            return false;
        }

        unsigned char* pixels = nullptr;
        int width = 0, height = 0, bpp = 0;
        atlas->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);
        if (!pixels || width <= 0 || height <= 0 || bpp != 4)
        {
            IM_DELETE(atlas);
            device->Release();
            g_failed = true;
            LogWarn("CJK font: failed to rasterize the atlas.");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width              = static_cast<UINT>(width);
        desc.Height             = static_cast<UINT>(height);
        desc.MipLevels          = 1;
        desc.ArraySize          = 1;
        desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count   = 1;
        desc.Usage              = D3D11_USAGE_DEFAULT;
        desc.BindFlags          = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sub = {};
        sub.pSysMem      = pixels;
        sub.SysMemPitch  = static_cast<UINT>(width * 4);

        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, &sub, &texture);
        if (FAILED(hr) || !texture)
        {
            IM_DELETE(atlas);
            device->Release();
            g_failed = true;
            LogWarn("CJK font: CreateTexture2D failed.");
            return false;
        }

        ID3D11ShaderResourceView* srv = nullptr;
        hr = device->CreateShaderResourceView(texture, nullptr, &srv);
        texture->Release();
        device->Release();
        if (FAILED(hr) || !srv)
        {
            IM_DELETE(atlas);
            g_failed = true;
            LogWarn("CJK font: CreateShaderResourceView failed.");
            return false;
        }

        atlas->TexID = static_cast<ImTextureID>(srv);
        g_atlas = atlas;
        g_font  = font;
        g_srv   = srv;

        char msg[192];
        std::snprintf(msg, sizeof msg, "CJK UI font active: %s, atlas %dx%d.", face.c_str(), width, height);
        LogInfo(msg);
        return true;
    }
}

ImFont* CjkFont::Get()
{
    return Build() ? g_font : nullptr;
}

void CjkFont::Shutdown()
{
    ReleaseAll();
    g_failed = false;
}
