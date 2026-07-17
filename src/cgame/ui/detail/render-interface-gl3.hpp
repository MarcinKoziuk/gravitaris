#pragma once

#include <unordered_map>
#include <unordered_set>

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>

namespace Gravitaris {

namespace Gfx {
struct ShadersData;
}

class RenderInterfaceGL3 : public Rml::RenderInterface {
public:
    RenderInterfaceGL3();

    ~RenderInterfaceGL3() override;

    // Returns true if the renderer was successfully constructed.
    explicit operator bool() const
    { return static_cast<bool>(shaders); }

    // The viewport should be updated whenever the window size changes.
    void SetViewport(int viewport_width, int viewport_height);

    // Sets up OpenGL states for taking rendering commands from RmlUi.
    void BeginFrame();

    void EndFrame();

    // Optional, can be used to clear the framebuffer.
    void Clear();

    // -- Inherited from Rml::RenderInterface --

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;

    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void EnableScissorRegion(bool enable) override;

    void SetScissorRegion(Rml::Rectanglei region) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;

    void ReleaseTexture(Rml::TextureHandle texture_handle) override;

    void SetTransform(const Rml::Matrix4f *transform) override;

    // Can be passed to RenderGeometry() to enable texture rendering without changing the bound texture.
    static const Rml::TextureHandle TextureEnableWithoutBinding = Rml::TextureHandle(-1);

    // Live-texture bridge: registers an engine-owned GL texture under `name`
    // so RCSS/RML can reference it as src="live://name" (the minimap FBO
    // does). LoadTexture resolves the name to this id instead of reading a
    // file, and ReleaseTexture will never delete it -- the engine side owns
    // the texture's lifetime and may re-render its contents every frame.
    void RegisterLiveTexture(const Rml::String& name, unsigned glTextureId, Rml::Vector2i dimensions);

private:
    enum class ProgramId {
        None, Texture = 1, Color = 2, All = (Texture | Color)
    };

    void SubmitTransformUniform(ProgramId program_id, int uniform_location);

    Rml::Matrix4f transform, projection;
    ProgramId transform_dirty_state = ProgramId::All;
    bool transform_active = false;

    enum class ScissoringState {
        Disable, Scissor, Stencil
    };
    ScissoringState scissoring_state = ScissoringState::Disable;

    int viewport_width = 0;
    int viewport_height = 0;

    struct LiveTexture {
        unsigned glTextureId;
        Rml::Vector2i dimensions;
    };
    std::unordered_map<Rml::String, LiveTexture> m_liveTextures;
    // Handles LoadTexture returned for live textures; ReleaseTexture must not
    // glDeleteTextures these (the engine owns them).
    std::unordered_set<Rml::TextureHandle> m_liveHandles;

    Rml::UniquePtr<Gfx::ShadersData> shaders;
};

} // namespace Gravitaris
