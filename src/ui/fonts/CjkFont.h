#pragma once

struct ImFont;

// CjkFont -- a private ImFontAtlas carrying a CJK-capable face.
//
// Why this exists: all UI text is drawn with Nexus's shared ImGui font (NexusLink->Font) unless the
// Menomonia ladder is loaded, and both atlases are baked over Latin ranges only. Text outside that
// range (Chinese item / zone / achievement names) therefore renders as the fallback '?' glyph.
// Nexus's Fonts_AddFromFile takes no glyph-range argument, so the only way to draw CJK is to own an
// atlas: build one, upload it to GW2's D3D11 device, and hand the ImFont to Gw2Ui. ImDrawList::AddText
// pushes font->ContainerAtlas->TexID on its own, so the draw path needs no changes.
namespace CjkFont
{
    // Builds the atlas on first call (render thread; needs APIDefs->SwapChain). Returns nullptr when no
    // CJK face is installed on the machine -- callers then keep the stock behaviour.
    ImFont* Get();

    // Releases the atlas + texture. Call from the addon's unload path.
    void Shutdown();
}
