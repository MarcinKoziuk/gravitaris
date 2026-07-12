# Selective postprocess for RmlUi (glow/CRT on chrome, not on text)

Problem: `GlowPostProcess` (bloom + CRT scanlines) currently applies to the
whole scene, including RmlUi when `m_uiInWorld` is true. Want backgrounds/
panels affected but text (and possibly images/video) to stay crisp.

Not yet decided; options below. Preference order depends on whether the UI
will have overlapping floating windows/dialogs — see the challenge section,
which changes the ranking from a first pass that only considered a single
flat document.

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

## Option 4: mask-based partial postprocess

Render RmlUi once, normally, into the scene color target exactly as today —
no split, no second pass. Alongside it (same draw calls, same document paint
order, just an extra write target/shader swap), rasterize a mask buffer where
each `RenderGeometry` call writes crisp=1 or crisp=0. The CRT/bloom passes
read this mask to blend between "postprocessed" and "as-is" per pixel.

Originally noted as not working for bloom (crisp glyphs would still pick up a
self-halo from having already contributed to the blur source). That's
avoidable: multiply the *bright-pass/threshold* shader's input by `(1 -
mask)` before it enters the blur chain, so crisp pixels never seed bloom in
the first place — not merely unblurred afterward. (Bloom from a *different*,
non-crisp bright pixel spilling onto nearby crisp text is unaffected by this
and is arguably desirable — that's the light source doing the blooming, not
the text.)

## Challenge: two overlapping dialogs, each with crisp + non-crisp content

Concretely: Dialog A (front) and Dialog B (behind) both have a background
panel (non-crisp) and text (crisp), and they visually overlap. Correct
behavior, matching what RmlUi already does today in a single pass: wherever
A's opaque background covers B, it should hide *both* B's background *and*
B's text — front-to-back occlusion doesn't care which element is "crisp."

**Options 1 & 2** both fail this. Each turns the document's single
depth-ordered draw list into exactly two buckets rendered as two sequential
raster passes — deferred draws (option 1) or two full render passes (option
2) — and within each bucket, relative order between B and A is preserved, but
between buckets it is not: the crisp bucket is composited entirely on top of
the non-crisp bucket, full stop. Result: B's crisp text renders on top of A's
opaque background in the overlap region, even though A is supposed to be in
front. The dialog behind visually punches its text through the dialog in
front. Option 2 is slightly harder to patch than option 1 here, since it's
two independent full-screen render passes rather than one pass with a
deferred list you could still intercept.

A partial fix for option 1: also rasterize a coverage mask of all non-crisp
opaque draws in true document order, and use it to discard/clip the deferred
crisp layer wherever later opaque non-crisp content should have covered it.
This works, but at that point you've reimplemented half of option 4's mask
underneath option 1's classification — worth asking whether to just do
option 4 outright instead.

**Option 3 fails structurally, not just incidentally.** Background chrome
and foreground content are different documents rendered at fixed points in
the frame — a background-document element can *never* occlude a
foreground-document element, by construction, regardless of which dialog
"owns" which piece. Every window's foreground content will always punch
through every window's background chrome, independent of intended stacking
order. Fixing this needs a background/foreground document pair *per dialog*
plus manual z-interleave logic between pairs — at which point you're
reimplementing RmlUi's own z-ordering by hand.

**Option 4 handles this correctly, close to for free.** The color buffer is
rendered in one normal pass, so A's opaque background already occludes B
exactly as it does today — nothing about that changes. The mask buffer is
rasterized with the *same* draw calls in the *same* order, so wherever A's
background (mask=0) painted over B's text (mask=1) in the color buffer, it
also painted over it in the mask buffer. The final mask value at each pixel
reflects "what's actually visible there after occlusion," not "does a crisp
element exist somewhere in this region" — so the CRT/bloom blend is correct
across overlapping dialogs without any extra bookkeeping. This is the
structural reason option 4 doesn't inherit the options 1–3 problem: it never
reorders draws relative to each other into buckets; it only asks, per
already-correctly-occluded pixel, "was the thing that ended up here crisp?"

## Revised recommendation

If overlapping/floating/draggable dialogs are ever in scope, **option 4** is
the only one that handles it correctly without extra machinery, and the
bloom limitation that originally ranked it last is fixable (pre-multiply the
bright-pass input by `(1 - mask)`). If the UI stays a single non-overlapping
document (one full-screen HUD, no floating windows) — not yet decided, see
`IDEAS.md` — the z-order problem above never triggers in practice, and
options 1/2 remain simpler to implement for that case.

## Cheap thing to try first

If the actual complaint is bloom on text specifically (not scanlines
chopping small glyphs), dimming UI text color below `GlowPostProcess`'s
`m_threshold` (currently `Defaults::threshold = 0.35`, see
`glow-post-process.hpp`) exempts it from bloom with zero code changes. Won't
help with CRT scanlines on small text, or with the overlapping-dialogs case
above, which needs one of the real options.
