// GLSL ES (WebGL) requires an explicit default float precision in fragment
// shaders -- desktop GLSL has no such requirement and silently accepts this
// as a no-op, so it is unconditional rather than platform-guarded.
precision highp float;

in lowp vec4 interpolatedColor;
out lowp vec4 fragmentColor;

void main() {
    fragmentColor = interpolatedColor;
}
