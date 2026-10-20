#include "ui/fonts/CjkFont.h"
#include "Shared.h"

#include <imgui.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

namespace
{
    ImFontAtlas*              g_atlas = nullptr;
    ImFont*                   g_font  = nullptr;
    ID3D11ShaderResourceView* g_srv   = nullptr;
    Texture_t                 g_view{};   // Nexus's texture view -- this, not the raw SRV, is the ImTextureID
    bool                      g_givenUp = false;
    int                       g_attempts = 0;

    constexpr float kPx = 32.0f;   // baked size: crisp for menu text, downscales cleanly to the 14-16 px body

    // The addon's own debug log. Nexus's Log() only feeds its in-game console window, not Nexus.log, so a
    // one-line-per-step file next to the data is the only reliable way to diagnose this at a distance.
    void Dbg(const char* fmt, ...)
    {
        static std::string path;
        if (path.empty())
        {
            const char* root = AddonDir();
            if (!root || !*root) return;
            path = std::string(root) + "\\data\\cjk_debug.log";
        }
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
        std::fprintf(f, "[tid %08lx] ", (unsigned long)GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

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

    // Every candidate, in preference order: faces we can ship next to the addon first, then the CJK faces
    // Chinese Windows always has. BuildAtlas() walks this list until one actually yields CJK glyphs.
    std::vector<std::string> Candidates()
    {
        std::vector<std::string> out;
        const std::string dir = AddonFontDir();
        for (const char* n : { "\\NotoSansSC-Regular.otf", "\\NotoSansCJKsc-Regular.otf",
                               "\\SourceHanSansSC-Regular.otf", "\\SourceHanSansCN-Regular.otf" })
            out.push_back(dir + n);
        for (const char* p : { "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msyh.ttf",
                               "C:\\Windows\\Fonts\\Deng.ttf", "C:\\Windows\\Fonts\\simhei.ttf",
                               "C:\\Windows\\Fonts\\simsun.ttc" })
            out.push_back(p);
        return out;
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
        if (g_givenUp) return false;
        if (!APIDefs || !APIDefs->SwapChain)
        {
            if (g_attempts == 0) Dbg("wait: no APIDefs/SwapChain yet");
            return false;
        }

        ++g_attempts;
        if (g_attempts == 1) Dbg("build: first attempt, tid %08lx", (unsigned long)GetCurrentThreadId());

        IDXGISwapChain* swap = static_cast<IDXGISwapChain*>(APIDefs->SwapChain);
        ID3D11Device* device = nullptr;
        const HRESULT dh = swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
        if (FAILED(dh) || !device)
        {
            Dbg("fail: GetDevice hr=0x%08lx", (unsigned long)dh);
            if (g_attempts >= 5) { g_givenUp = true; LogWarn("CJK font: cannot reach the D3D11 device."); }
            return false;
        }

        bool sawFace = false;
        for (const std::string& face : Candidates())
        {
            if (!FileExists(face)) continue;
            sawFace = true;

            ImFontAtlas* atlas = IM_NEW(ImFontAtlas)();
            atlas->TexDesiredWidth = 2048;

            ImFontConfig cfg;
            cfg.FontNo      = 0;    // .ttc collections: face 0 (regular)
            cfg.OversampleH = 1;    // CJK glyphs are complex; 1 is the sane default for them
            cfg.OversampleV = 1;
            cfg.PixelSnapH  = false;
            std::snprintf(cfg.Name, IM_ARRAYSIZE(cfg.Name), "TC CJK UI");

            ImFont* font = atlas->AddFontFromFileTTF(face.c_str(), kPx, &cfg, atlas->GetGlyphRangesChineseSimplifiedCommon());
            if (!font)
            {
                Dbg("face %s: AddFontFromFileTTF failed", face.c_str());
                IM_DELETE(atlas);
                continue;
            }
            unsigned char* pixels = nullptr;
            int width = 0, height = 0, bpp = 0;
            atlas->GetTexDataAsRGBA32(&pixels, &width, &height, &bpp);   // this is what BUILDS the atlas
            if (!pixels || width <= 0 || height <= 0 || bpp != 4)
            {
                Dbg("face %s: rasterize failed (%dx%d bpp%d)", face.c_str(), width, height, bpp);
                IM_DELETE(atlas);
                continue;
            }

            // The glyph table (and therefore FindGlyphNoFallback) only exists once the atlas has been built,
            // so this check MUST come after GetTexDataAsRGBA32. Doing it earlier always reported "no U+4E2D"
            // and rejected every face.
            if (!font->FindGlyphNoFallback((ImWchar)0x4E2D))   // '中'
            {
                Dbg("face %s: no U+4E2D in atlas", face.c_str());
                IM_DELETE(atlas);
                continue;
            }
            Dbg("face %s: atlas %dx%d ok", face.c_str(), width, height);

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width            = static_cast<UINT>(width);
            desc.Height           = static_cast<UINT>(height);
            desc.MipLevels        = 1;
            desc.ArraySize        = 1;
            desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage            = D3D11_USAGE_DEFAULT;
            desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA sub = {};
            sub.pSysMem     = pixels;
            sub.SysMemPitch = static_cast<UINT>(width * 4);

            ID3D11Texture2D* texture = nullptr;
            HRESULT hr = device->CreateTexture2D(&desc, &sub, &texture);
            if (FAILED(hr) || !texture)
            {
                Dbg("face %s: CreateTexture2D hr=0x%08lx (wrong thread? device is single-threaded)", face.c_str(), (unsigned long)hr);
                IM_DELETE(atlas);
                device->Release();
                if (g_attempts >= 5) { g_givenUp = true; LogWarn("CJK font: CreateTexture2D keeps failing."); }
                return false;
            }

            ID3D11ShaderResourceView* srv = nullptr;
            hr = device->CreateShaderResourceView(texture, nullptr, &srv);
            texture->Release();
            if (FAILED(hr) || !srv)
            {
                Dbg("face %s: CreateShaderResourceView hr=0x%08lx", face.c_str(), (unsigned long)hr);
                IM_DELETE(atlas);
                device->Release();
                if (g_attempts >= 5) { g_givenUp = true; LogWarn("CJK font: CreateShaderResourceView keeps failing."); }
                return false;
            }

            device->Release();

            // Nexus's ImTextureID is a Texture_t* (Width/Height/Resource), not the bare ID3D11ShaderResourceView*.
            // Handing it the raw SRV makes its renderer read {Width,Height,Resource} out of the SRV's vtable and
            // bind garbage, which draws nothing at all.
            g_view.Width    = static_cast<uint32_t>(width);
            g_view.Height   = static_cast<uint32_t>(height);
            g_view.Resource = srv;

            atlas->TexID = static_cast<ImTextureID>(&g_view);
            g_atlas = atlas;
            g_font  = font;
            g_srv   = srv;
            Dbg("OK: %s active", face.c_str());

            char msg[192];
            std::snprintf(msg, sizeof msg, "CJK UI font active: %s, atlas %dx%d.", face.c_str(), width, height);
            LogInfo(msg);
            return true;
        }

        device->Release();
        if (!sawFace)
        {
            Dbg("fail: no candidate face exists on this machine");
            g_givenUp = true;
            LogWarn("CJK font: no Chinese face found (Noto/Source Han in data\\fonts, or msyh.ttc in C:\\Windows\\Fonts).");
            return false;
        }
        if (g_attempts >= 5) { g_givenUp = true; Dbg("fail: every candidate face failed"); }
        return false;
    }
}

ImFont* CjkFont::Get()
{
    return Build() ? g_font : nullptr;
}

void CjkFont::DrawDiagnostic()
{
    if (!g_font) return;
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        Dbg("diag: font=%p atlas=%p texid=%p view.Resource=%p %ux%u fontsize=%.1f",
            (void*)g_font, (void*)g_atlas, (void*)g_atlas->TexID, (void*)g_view.Resource,
            g_view.Width, g_view.Height, g_font->FontSize);
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(30, 30), ImVec2(560, 120), IM_COL32(0, 0, 0, 230));
    dl->AddText(g_font, 28.0f, ImVec2(42, 40), IM_COL32(255, 255, 0, 255), "CJK diag: 中文测试 ABC 0123");
    dl->AddText(g_font, 16.0f, ImVec2(42, 78), IM_COL32(255, 255, 255, 255), "16px: 精金守卫装置 / Adamant Guard");
}

void CjkFont::Shutdown()
{
    ReleaseAll();
    g_givenUp = false;
    g_attempts = 0;
}
