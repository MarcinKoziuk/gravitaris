// GLSL ES (WebGL) requires an explicit default float precision in fragment
// shaders; desktop GLSL accepts it as a no-op.
precision highp float;

in lowp vec4 interpolatedColor;
out lowp vec4 fragmentColor;

void main() {
    // Premultiplied on the way out: the beam is drawn additively, so the alpha
    // carried across the wedge is what thins it toward the far end rather than
    // anything the blend equation reads.
    fragmentColor = vec4(interpolatedColor.rgb * interpolatedColor.a, interpolatedColor.a);
}
