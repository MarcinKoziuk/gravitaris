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
    : m_blurShader(filesystem)
    , m_compositeShader(filesystem)
{
    // No vertex buffers at all — the vertex shader derives positions purely
    // from gl_VertexID (the "big triangle" trick).
    m_fullscreenTri.setPrimitive(MeshPrimitive::Triangles).setCount(3);
}

void GlowPostProcess::Resize(const Vector2i& windowSize)
{
    if (windowSize == m_fullSize || windowSize.x() <= 0 || windowSize.y() <= 0) {
        return;
    }

    m_fullSize = windowSize;
    m_halfSize = Math::max(windowSize / 2, Vector2i{1, 1});
    m_blurSize = Math::max(windowSize / 4, Vector2i{1, 1});

    m_sceneColor = MakeColorTexture(m_fullSize);
    m_sceneFbo = MakeColorFramebuffer(m_fullSize, m_sceneColor);

    m_half = MakeColorTexture(m_halfSize);
    m_halfFbo = MakeColorFramebuffer(m_halfSize, m_half);

    m_blurA = MakeColorTexture(m_blurSize);
    m_blurB = MakeColorTexture(m_blurSize);
    m_blurFboA = MakeColorFramebuffer(m_blurSize, m_blurA);
    m_blurFboB = MakeColorFramebuffer(m_blurSize, m_blurB);
}

void GlowPostProcess::BeginScene(const Vector2i& windowSize)
{
    Resize(windowSize);

    m_sceneFbo.bind();
    m_sceneFbo.setViewport(Range2Di{{}, m_fullSize});
    m_sceneFbo.clearColor(0, Color4{0.f, 0.f, 0.f, 1.f});
}

void GlowPostProcess::EndSceneAndComposite(GL::AbstractFramebuffer& target, const Vector2i& windowSize)
{
    if (!m_enabled) {
        // Cheap path: just copy the sharp scene through untouched.
        GL::AbstractFramebuffer::blit(m_sceneFbo, target, Range2Di{{}, m_fullSize}, Range2Di{{}, windowSize},
                                       GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);
        return;
    }

    GL::Renderer::disable(GL::Renderer::Feature::Blending);

    // Prefiltered downsample: two chained 2x linear blits give an exact 4x4
    // box filter, so thin lines can't alias against the 1/4-res grid (a
    // single 4x decimation drops rows entirely, which showed up as periodic
    // glow blobs along shallow-angle curves).
    GL::AbstractFramebuffer::blit(m_sceneFbo, m_halfFbo, Range2Di{{}, m_fullSize}, Range2Di{{}, m_halfSize},
                                   GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);
    GL::AbstractFramebuffer::blit(m_halfFbo, m_blurFboA, Range2Di{{}, m_halfSize}, Range2Di{{}, m_blurSize},
                                   GL::FramebufferBlit::Color, GL::FramebufferBlitFilter::Linear);

    // Separable Gaussian at 1/4 res: horizontal A -> B, vertical B -> A.
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

    // Composite: sharp scene + intensity * blurred glow -> target, at window res.
    target.bind();
    target.setViewport(Range2Di{{}, windowSize});
    m_compositeShader.setIntensity(m_intensity)
            .bindScene(m_sceneColor)
            .bindGlow(m_blurA)
            .draw(m_fullscreenTri);
}

} // namespace Gravitaris
