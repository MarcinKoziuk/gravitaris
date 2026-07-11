#pragma once

#include <algorithm>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/renderer/shader/glow-threshold-shader.hpp>
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

    // Downsample intermediates, halving each step (full->1/2->1/4->...) down
    // to the blur resolution. Each 2x step is a box prefilter, preventing thin
    // lines from aliasing (periodic glow "blobs" along shallow-angle curves if
    // decimating in one big jump). On HiDPI the scene is >1/4 the blur size, so
    // an extra halving runs; on 1x the last step is a 1:1 copy.
    Magnum::GL::Texture2D m_half{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_halfFbo{Magnum::NoCreate};
    Magnum::GL::Texture2D m_quarter{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_quarterFbo{Magnum::NoCreate};

    // Ping-pong pair at the blur resolution for the separable blur.
    Magnum::GL::Texture2D m_blurA{Magnum::NoCreate};
    Magnum::GL::Texture2D m_blurB{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_blurFboA{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_blurFboB{Magnum::NoCreate};

    // Full-res target the glow composite writes into, so a following CRT pass
    // has something to read (the composite can't sample the framebuffer it
    // draws to). Unused on the plain-copy path.
    Magnum::GL::Texture2D m_outputColor{Magnum::NoCreate};
    Magnum::GL::Framebuffer m_outputFbo{Magnum::NoCreate};

    GlowThresholdShader m_thresholdShader;
    GlowBlurShader m_blurShader;
    GlowCompositeShader m_compositeShader;
    CrtShader m_crtShader;

    // Fullscreen triangle: no vertex buffer, positions come from gl_VertexID.
    Magnum::GL::Mesh m_fullscreenTri;

    // Sharp passes (scene, composite, present) run at m_fullSize (framebuffer
    // pixels). The blur runs at m_blurSize, derived from the *logical* window
    // size, so the halo is the same fraction of the screen regardless of HiDPI
    // backing scale (and cheaper on Retina, where m_fullSize is much larger).
    Vector2i m_fullSize{0, 0};
    Vector2i m_logicalSize{0, 0};
    Vector2i m_halfSize{0, 0};
    Vector2i m_quarterSize{0, 0};
    Vector2i m_blurSize{0, 0};

    bool m_enabled = true;
    float m_intensity = 2.2f;
    int m_blurPasses = 3; // more passes = wider, softer, more present glow

    // Bright-pass cutoff before the blur (0-1, in linear-ish 8-bit color
    // units): pixels at/below this don't bloom at all. Without it, blurring a
    // large flat-filled area (a UI dialog's background) returns nearly that
    // same color, which the composite then ADDS back on top — inflating
    // brightness across the whole interior regardless of size. Vector lines
    // (near-white/cyan, ~1.0) stay well above this; dim UI panel fills don't.
    float m_threshold = 0.35f;

    bool m_crtEnabled = true;
    float m_scanlineStrength = 0.725f; // 50% stronger than the initial 0.35 default

    void Resize(const Vector2i& framebufferSize, const Vector2i& logicalSize);

    // Present `sourceTex` (in `sourceFbo`) to `target`, through the CRT
    // scanline shader if enabled, else a plain blit.
    void Present(Magnum::GL::Texture2D& sourceTex, Magnum::GL::Framebuffer& sourceFbo,
                 Magnum::GL::AbstractFramebuffer& target, const Vector2i& windowSize, float time);

public:
    explicit GlowPostProcess(IFilesystem& filesystem);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    void SetIntensity(float intensity) { m_intensity = std::max(0.f, intensity); }
    [[nodiscard]] float GetIntensity() const { return m_intensity; }
    void AddIntensity(float delta) { SetIntensity(m_intensity + delta); }

    void SetThreshold(float threshold) { m_threshold = std::max(0.f, threshold); }
    [[nodiscard]] float GetThreshold() const { return m_threshold; }

    void SetCrtEnabled(bool enabled) { m_crtEnabled = enabled; }
    [[nodiscard]] bool IsCrtEnabled() const { return m_crtEnabled; }

    void SetScanlineStrength(float strength) { m_scanlineStrength = strength; }
    [[nodiscard]] float GetScanlineStrength() const { return m_scanlineStrength; }

    // Bind the offscreen scene target and clear it; call before rendering
    // the normal game scene. framebufferSize is the sharp render resolution;
    // logicalSize (window points) sets the DPI-independent blur resolution.
    void BeginScene(const Vector2i& framebufferSize, const Vector2i& logicalSize);

    // Blur+composite (or plain blit, if disabled) the scene into `target`.
    // `time` (seconds, any monotonically-increasing small-magnitude clock —
    // NOT wall-clock epoch time, which would lose float precision in the
    // shader's sin()) drives the CRT wiggle when the CRT pass is enabled.
    void EndSceneAndComposite(Magnum::GL::AbstractFramebuffer& target, const Vector2i& windowSize, float time);
};

} // namespace Gravitaris
