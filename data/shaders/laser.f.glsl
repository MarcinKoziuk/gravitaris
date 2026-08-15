// GLSL ES (WebGL) requires an explicit default float precision in fragment
// shaders; desktop GLSL accepts it as a no-op.
precision highp float;

// How solid a beam is where it leaves the mount. Short of opaque on purpose:
// it is light, and the hull behind it should still read through the wedge.
const float NEAR_ALPHA = 0.85;

in lowp vec3 interpolatedColor;
in mediump float fadeDistance;

out lowp vec4 fragmentColor;

void main() {
    // Light thinning out as it travels, not a ramp shared across the length of
    // the beam: what a laser burns reaches several times further than what it
    // lights, and a fade stretched over the former draws every shot as a line
    // across the whole sector.
    float alpha = NEAR_ALPHA * exp(-fadeDistance);

    // Premultiplied on the way out: the beam is drawn additively, so the alpha
    // computed here is what thins it toward the far end rather than anything
    // the blend equation reads.
    fragmentColor = vec4(interpolatedColor * alpha, alpha);
}
