# Selective postprocess for RmlUi (glow/CRT on chrome, not on text)

Problem: `GlowPostProcess` (bloom + CRT scanlines) currently applies to the
whole scene, including RmlUi when `m_uiInWorld` is true. Want backgrounds/
panels affected but text (and possibly images/video) to stay crisp.

Not yet decided; options below, roughly in order of preference.

## Option 1: classify draw calls in RenderInterfaceGL3, two targets

RmlUi's render interface already sees every draw call and distinguishes
texture creation paths:

- Font glyph atlases arrive via `GenerateTexture()` (called by the font
  engine).
- Images/video arrive via `LoadTexture()`.
- Untextured geometry is backgrounds/borders/decorator fills.

Tag each `Rml::TextureHandle` as "crisp" or "affected" at creation time. In
`RenderGeometry()`, route crisp-tagged draws to a separate deferred list (or a
second FBO bound after `EndSceneAndComposite`); everything else draws into
the glow scene target as it does now.

Pros: single document, positions always in sync, per-texture-class control,
degrades gracefully (unrecognized textures default to one bucket). Con:
touches the render interface; need to decide draw order between the two
layers (crisp-on-top is the natural choice for text).

## Option 2: render the document twice with a class swap

Add a root class per pass (e.g. `.glow-pass`, `.crisp-pass`) and stylesheet
rules that blank out the other pass's content (`.glow-pass .text { color:
transparent }` / `.crisp-pass .panel { background: transparent }`). Render
once into the glow scene, swap the class, render again after
`EndSceneAndComposite`.

Pros: pure RML/CSS, no render-interface changes, authors control crispness
via classes. Con: two `Update()` + `Render()` passes per frame (class change
dirties layout/style), discipline required so every element belongs to
exactly one pass.

## Option 3: two documents/contexts

Background chrome and foreground content as separate `Rml::ElementDocument`s
(possibly separate `Rml::Context`s), rendered at the two different points in
the frame.

Simplest mentally, no interface hacks — but anything dynamic (moving/resizing
windows, scrolling, layout that spans both) needs manual sync between
documents. Reasonable for a mostly-static HUD, painful for real windows.
Least preferred for anything beyond a static background.

## Option 4: mask-based partial postprocess

Render text into a stencil/alpha mask; composite/CRT shader lerps the effect
by mask instead of applying uniformly.

Works for scanlines. Does NOT work for bloom: by the time you'd mask, the
text's brightness has already contributed to the blurred halo texture, so
crisp glyphs still end up wearing a glow halo of themselves. (Might be a
desirable retro look on its own, just not "unaffected".)

## Cheap thing to try first

If the actual complaint is bloom on text specifically (not scanlines
chopping small glyphs), dimming UI text color below `GlowPostProcess`'s
`m_threshold` (currently `Defaults::threshold = 0.35`, see
`glow-post-process.hpp`) exempts it from bloom with zero code changes. Won't
help with CRT scanlines on small text, which is the more likely actual
irritant — that needs option 1 or 2.
