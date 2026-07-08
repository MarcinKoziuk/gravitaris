#pragma once

#include <algorithm>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/renderer/shader/glow-blur-shader.hpp>
#include <gravitaris/cgame/renderer/shader/glow-composite-shader.hpp>
#include <gravitaris/cgame/renderer/shader/crt-shader.hpp>

namespace Gravitaris {

using Magnum::Vector2i;

// Toggleable CRT-phosphor-style bloom/glow plus an optional CRT scanline
// pass, applied as post-process over the whole rendered scene (game world
// only — UI is drawn afterwards, on top, and stays crisp).
//
// Pipeline per frame:
//   1. BeginScene()  — bind an offscreen full-res color target; caller
//      renders the normal scene into it exactly as before (no renderer
//      changes needed).
//   2. EndSceneAndComposite(target) — if glow enabled: downsample+blur the
//      scene (separable Gaussian, N passes at 1/4 res so a wide soft glow is
//      cheap) and additively composite it over the sharp scene. Then, if the
//      CRT pass is enabled, present through the scanline shader; otherwise
//      copy straight to `target`.
class GlowPostProcess {
private:
    Magnum::GL::Texture2D m_sceneColor{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_sceneFbo{Magnum::NoCreate};

    // Intermediate 1/2-res target: downsampling full->1/2->1/4 via two 2x
    // linear blits is an exact 4x4 box prefilter, which prevents thin lines
    // from aliasing against the coarse 1/4-res grid (visible as periodic
    // glow "blobs" along shallow-angle curves if decimating in one 4x jump).
    Magnum::GL::Texture2D m_half{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_halfFbo{Magnum::NoCreate};

    // Ping-pong pair at 1/4 resolution for the separable blur.
    Magnum::GL::Texture2D m_blurA{Magnum::NoCreate};
    Magnum::GL::Texture2D m_blurB{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_blurFboA{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_blurFboB{Magnum::NoCreate};

    // Full-res target the glow composite writes into, so a following CRT pass
    // has something to read (the composite can't sample the framebuffer it
    // draws to). Unused on the plain-copy path.
    Magnum::GL::Texture2D m_outputColor{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_outputFbo{Magnum::NoCreate};

    GlowBlurShader m_blurShader;
    GlowCompositeShader m_compositeShader;
    CrtShader m_crtShader;

    // Fullscreen triangle: no vertex buffer, positions come from gl_VertexID.
    Magnum::GL::Mesh m_fullscreenTri;

    Vector2i m_fullSize{0, 0};
    Vector2i m_halfSize{0, 0};
    Vector2i m_blurSize{0, 0};

    bool m_enabled = false;
    float m_intensity = 2.2f;
    int m_blurPasses = 3; // more passes = wider, softer, more present glow

    bool m_crtEnabled = false;
    float m_scanlineStrength = 0.525f; // 50% stronger than the initial 0.35 default

    void Resize(const Vector2i& windowSize);

    // Present `sourceTex` (in `sourceFbo`) to `target`, through the CRT
    // scanline shader if enabled, else a plain blit.
    void Present(Magnum::GL::Texture2D& sourceTex, Magnum::GL::Framebuffer& sourceFbo,
                 Magnum::GL::AbstractFramebuffer& target, const Vector2i& windowSize);

public:
    explicit GlowPostProcess(IFilesystem& filesystem);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    void SetIntensity(float intensity) { m_intensity = std::max(0.f, intensity); }
    [[nodiscard]] float GetIntensity() const { return m_intensity; }
    void AddIntensity(float delta) { SetIntensity(m_intensity + delta); }

    void SetCrtEnabled(bool enabled) { m_crtEnabled = enabled; }
    [[nodiscard]] bool IsCrtEnabled() const { return m_crtEnabled; }

    void SetScanlineStrength(float strength) { m_scanlineStrength = strength; }
    [[nodiscard]] float GetScanlineStrength() const { return m_scanlineStrength; }

    // Bind the offscreen scene target and clear it; call before rendering
    // the normal game scene.
    void BeginScene(const Vector2i& windowSize);

    // Blur+composite (or plain blit, if disabled) the scene into `target`.
    void EndSceneAndComposite(Magnum::GL::AbstractFramebuffer& target, const Vector2i& windowSize);
};

} // namespace Gravitaris
