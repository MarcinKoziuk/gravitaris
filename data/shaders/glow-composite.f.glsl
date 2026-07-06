// Additively blends the blurred glow layer on top of the sharp scene —
// mimicking phosphor bloom on a real vector CRT.

uniform sampler2D sceneTex;
uniform sampler2D glowTex;
uniform highp float intensity;

in highp vec2 uv;

out lowp vec4 fragmentColor;

void main() {
    vec3 sharp = texture(sceneTex, uv).rgb;
    vec3 glow = texture(glowTex, uv).rgb;
    fragmentColor = vec4(sharp + glow * intensity, 1.0);
}
