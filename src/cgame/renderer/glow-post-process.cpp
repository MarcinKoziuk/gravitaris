#include <cmath>

#include <Magnum/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureFormat.h>
#include <Magnum/GL/PixelFormat.h>
#include <Magnum/GL/Sampler.h>
#include <Magnum/Math/Range.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Functions.h>

#include <gravitaris/cgame/renderer/glow-post-process.hpp>

namespace Gravitaris {

using namespace Magnum;

namespace {

GL::Texture2D MakeColorTexture(const Vector2i& size)
{
    GL::Texture2D texture;
    texture.setStorage(1, GL::TextureFormat::RGBA8, size)
            .setMinificationFilter(GL::SamplerFilter::Linear)
            .setMagnificationFilter(GL::SamplerFilter::Linear)
            .setWrapping(GL::SamplerWrapping::ClampToEdge);
    return texture;
}

GL::Framebuffer MakeColorFramebuffer(const Vector2i& size, GL::Texture2D& colorTexture)
{
    GL::Framebuffer fbo{Range2Di{{}, size}};
    fbo.attachTexture(GL::Framebuffer::ColorAttachment{0}, colorTexture, 0)
       .mapForDraw(GL::Framebuffer::ColorAttachment{0});
    return fbo;
}

} // namespace

GlowPostProcess::GlowPostProcess(IFilesystem& filesystem)
    : m_thresholdShader(filesystem)
    , m_blurShader(filesystem)
    , m_compositeShader(filesystem)
    , m_crtShader(filesystem)
{
    // No vertex buffers at all — the vertex shader derives positions purely
    // from gl_VertexID (the "big triangle" trick).
    m_fullscreenTri.setPrimitive(MeshPrimitive::Triangles).setCount(3);
}

void GlowPostProcess::Resize(const Vector2i& framebufferSize, const Vector2i& logicalSize)
{
    if ((framebufferSize == m_fullSize && logicalSize == m_logicalSize)
            || framebufferSize.x() <= 0 || framebufferSize.y() <= 0) {
        return;
    }

    m_fullSize = framebufferSize;
    m_logicalSize = logicalSize;
    m_halfSize = Math::max(framebufferSize / 2, Vector2i{1, 1});
    m_quarterSize = Math::max(framebufferSize / 4, Vector2i{1, 1});
    // Blur resolution follows the logical size, so the halo is DPI-independent.
    m_blurSize = Math::max(logicalSize / 4, Vector2i{1, 1});

    m_sceneColor = MakeColorTexture(m_fullSize);
    m_sceneFbo = MakeColorFramebuffer(m_fullSize, m_sceneColor);

    m_half = MakeColorTexture(m_halfSize);
    m_halfFbo = MakeColorFramebuffer(m_halfSize, m_half);

    m_quarter = MakeColorTexture(m_quarterSize);
    m_quarterFbo = MakeColorFramebuffer(m_quarterSize, m_quarter);

    m_blurA = MakeColorTexture(m_blurSize);
    m_blurB = MakeColorTexture(m_blurSize);
    m_blurFboA = MakeColorFramebuffer(m_blurSize, m_blurA);
    m_blurFboB = MakeColorFramebuffer(m_blurSize, m_blurB);

    m_outputColor = MakeColorTexture(m_fullSize);
    m_outputFbo = MakeColorFramebuffer(m_fullSize, m_outputColor);
}

void GlowPostProcess::BeginScene(const Vector2i& framebufferSize, const Vector2i& logicalSize)
{
    Resize(framebufferSize, logicalSize);

    m_sceneFbo.bind();
    m_sceneFbo.setViewport(Range2Di{{}, m_fullSize});
    m_sceneFbo.clearColor(0, Color4{0.f, 0.f, 0.f, 1.f});
}

void GlowPostProcess::Present(GL::Texture2D& sourceTex, GL::Framebuffer& sourceFbo,
                             GL::AbstractFramebuffer& target, const Vector2i& windowSize, float time)
{
    if (m_crtEnabled) {
        GL::Renderer::disable(GL::Renderer::Feature::Blending);
        target.bind();
        target.setViewport(Range2Di{{}, windowSize});
        m_crtShader.setViewportSize(Vector2{windowSize})
                .setScanlineStrength(m_scanlineStrength)
                .setLineWidthPx(m_scanlineWidthPx)
                .setPeriodPx(m_scanlinePeriodPx)
                .setFlickerRate(m_flickerRate)
                .setFlickerAmplitude(m_flickerAmplitude)
                .setScanJitterRate(m_scanJitterRate)
                .setScanJitterAmplitude(m_scanJitterAmplitude)
                .setPhaseJitterPx(m_phaseJitterPx)
                .setTime(time)
                .bindImage(sourceTex)
                .draw(m_fullscreenTri);
    } else {
        GL::AbstractFramebuffer::blit(sourceFbo, target, Range2Di{{}, m_fullSize}, Range2Di{{}, windowSize},
                                       GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);
    }
}

void GlowPostProcess::EndSceneAndComposite(GL::AbstractFramebuffer& target, const Vector2i& windowSize, float time)
{
    if (!m_enabled) {
        // No glow: present the sharp scene directly (CRT pass still applies).
        Present(m_sceneColor, m_sceneFbo, target, windowSize, time);
        return;
    }

    GL::Renderer::disable(GL::Renderer::Feature::Blending);

    // Bright-pass + first 2x downsample in one pass: extracts only pixels
    // above m_threshold (so large dim UI fills don't bloom, see m_threshold's
    // comment) while box-filtering full -> 1/2.
    m_halfFbo.bind();
    m_halfFbo.setViewport(Range2Di{{}, m_halfSize});
    m_thresholdShader.setTexelSize(Vector2{1.f / static_cast<float>(m_fullSize.x()), 1.f / static_cast<float>(m_fullSize.y())})
            .setThreshold(m_threshold)
            .bindImage(m_sceneColor)
            .draw(m_fullscreenTri);

    // Halve again (1/2 -> 1/4 of the framebuffer), then reach the blur
    // resolution. On HiDPI m_blurSize < m_quarterSize so this is a real 2x
    // box step; on 1x it is a 1:1 copy. Each hop stays <=2x so thin lines
    // never alias against a coarse grid.
    GL::AbstractFramebuffer::blit(m_halfFbo, m_quarterFbo, Range2Di{{}, m_halfSize}, Range2Di{{}, m_quarterSize},
                                   GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);
    GL::AbstractFramebuffer::blit(m_quarterFbo, m_blurFboA, Range2Di{{}, m_quarterSize}, Range2Di{{}, m_blurSize},
                                   GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);

    // Separable Gaussian at 1/4 res, repeated to widen the glow. Each pass:
    // horizontal A -> B, vertical B -> A, leaving the result back in blurA.
    for (int i = 0; i < m_blurPasses; ++i) {
        m_blurFboB.bind();
        m_blurFboB.setViewport(Range2Di{{}, m_blurSize});
        m_blurShader.setDirection(Vector2{1.f / static_cast<float>(m_blurSize.x()), 0.f})
                .bindImage(m_blurA)
                .draw(m_fullscreenTri);

        m_blurFboA.bind();
        m_blurFboA.setViewport(Range2Di{{}, m_blurSize});
        m_blurShader.setDirection(Vector2{0.f, 1.f / static_cast<float>(m_blurSize.y())})
                .bindImage(m_blurB)
                .draw(m_fullscreenTri);
    }

    // Glow "breathing": modulate bloom intensity by a few percent with three
    // incommensurate high-frequency sines (~9-22 Hz), so the halo shimmers
    // unevenly like unstable phosphor drive — while the sharp scene underneath
    // stays perfectly still.
    const float breathe = 0.55f * std::sin(time * 57.f)
                        + 0.30f * std::sin(time * 91.3f + 1.7f)
                        + 0.15f * std::sin(time * 139.7f + 4.2f);
    const float jitteredIntensity = m_intensity * (1.f + m_breatheAmplitude * breathe);

    // Composite sharp scene + intensity * blurred glow into the full-res
    // output target (so the CRT present pass can sample it).
    m_outputFbo.bind();
    m_outputFbo.setViewport(Range2Di{{}, m_fullSize});
    m_compositeShader.setIntensity(jitteredIntensity)
            .bindScene(m_sceneColor)
            .bindGlow(m_blurA)
            .draw(m_fullscreenTri);

    Present(m_outputColor, m_outputFbo, target, windowSize, time);
}

} // namespace Gravitaris
