// Background parallax starfield. Each star is a billboard quad: `center` is the
// star's *absolute world-space* position (not camera-relative -- see below),
// `corner` the [-1,1]^2 quad corner. The quad is built in pixel space so
// on-screen size is stable across zoom.
//
// Parallax is applied here, per-vertex, via `parallax` + the `cameraPos`
// uniform, rather than baked into `center` on the CPU side the way it used
// to be. That mattered: baking camera position into vertex data meant any
// camera movement invalidated the whole vertex buffer, forcing a full
// re-upload every single frame (60/s) -- under WASM specifically, profiling
// a real session found Emscripten's WebGL bindings wrap each such upload in
// a fresh JS typed-array view, and that was frequent/large enough to cause
// periodic 100ms+ "Major GC" pauses on the browser's main thread. Moving
// parallax into a uniform (cameraPos, cheap to update every frame) plus a
// per-vertex attribute (parallax, fixed at generation time, never needs
// updating) means camera movement alone no longer requires touching the
// vertex buffer at all -- see StarfieldRenderer::NeedsRebuild for when it
// still does (only once the camera has drifted far enough that the
// generously over-padded generated region no longer covers what's visible).
uniform mat3 viewProjection;   // projection only (no camera translation -- see above)
uniform vec2 viewportSize;     // framebuffer size in pixels
uniform vec2 cameraPos;        // world-space camera position, updated every frame

in highp vec2 center;
in highp float parallax;       // 0 = infinitely far (still), 1 = moves with the world
in highp vec2 corner;
in highp vec2 params;          // x = size in pixels, y = brightness
in highp vec3 starColor;

out lowp vec3 vColor;
out lowp float vBrightness;
out highp vec2 vCorner;

vec2 toPixel(vec2 worldPos) {
    vec3 clip = viewProjection * vec3(worldPos, 1.0);
    return (clip.xy / clip.z) * 0.5 * viewportSize;
}

void main() {
    float size = params.x;
    vBrightness = params.y;
    vColor = starColor;
    vCorner = corner;

    vec2 effectiveWorldPos = center - cameraPos * parallax;
    vec2 centerPix = toPixel(effectiveWorldPos);
    vec2 pix = centerPix + corner * size;

    vec2 ndc = pix / (0.5 * viewportSize);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
