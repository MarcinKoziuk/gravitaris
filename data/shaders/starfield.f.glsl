// GLSL ES (WebGL) requires an explicit default float precision in fragment
// shaders -- desktop GLSL has no such requirement and silently accepts this
// as a no-op, so it is unconditional rather than platform-guarded.
precision highp float;

// Soft radial blob per star. corner spans [-1,1]^2 across the (stretched) quad,
// so a radial falloff on it gives a round dot when unstretched and an elongated
// smear when the streak stretches the quad. Additively blended onto black, the
// post-process bloom then makes the brightest stars glow.

in lowp vec3 vColor;
in lowp float vBrightness;
in highp vec2 vCorner;

out lowp vec4 fragmentColor;

void main() {
    float d = length(vCorner);
    float alpha = smoothstep(1.0, 0.0, d) * vBrightness;
    fragmentColor = vec4(vColor * alpha, alpha);
}
