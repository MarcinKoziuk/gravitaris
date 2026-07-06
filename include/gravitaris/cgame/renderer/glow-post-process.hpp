#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/GL/Framebuffer.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

#include <gravitaris/cgame/renderer/shader/glow-blur-shader.hpp>
#include <gravitaris/cgame/renderer/shader/glow-composite-shader.hpp>

namespace Gravitaris {

using Magnum::Vector2i;

// Toggleable CRT-phosphor-style bloom/glow, applied as a post-process pass
// over the whole rendered scene (game world only — UI is drawn afterwards,
// on top of the composited result, and stays crisp).
//
// Pipeline per frame:
//   1. BeginScene()  — bind an offscreen full-res color target; caller
//      renders the normal scene into it exactly as before (no renderer
//      changes needed).
//   2. EndSceneAndComposite(target) — if enabled: downsample+blur the scene
//      (two-pass separable Gaussian, done at 1/4 res so a wide soft glow is
//      cheap) and additively composite it back over the sharp scene into
//      `target`. If disabled: just blits the sharp scene into `target`
//      unchanged, so toggling off costs almost nothing.
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

    GlowBlurShader m_blurShader;
    GlowCompositeShader m_compositeShader;

    // Fullscreen triangle: no vertex buffer, positions come from gl_VertexID.
    Magnum::GL::Mesh m_fullscreenTri;

    Vector2i m_fullSize{0, 0};
    Vector2i m_halfSize{0, 0};
    Vector2i m_blurSize{0, 0};

    bool m_enabled = false;
    float m_intensity = 1.2f;

    void Resize(const Vector2i& windowSize);

public:
    explicit GlowPostProcess(IFilesystem& filesystem);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    void SetIntensity(float intensity) { m_intensity = intensity; }
    [[nodiscard]] float GetIntensity() const { return m_intensity; }

    // Bind the offscreen scene target and clear it; call before rendering
    // the normal game scene.
    void BeginScene(const Vector2i& windowSize);

    // Blur+composite (or plain blit, if disabled) the scene into `target`.
    void EndSceneAndComposite(Magnum::GL::AbstractFramebuffer& target, const Vector2i& windowSize);
};

} // namespace Gravitaris
