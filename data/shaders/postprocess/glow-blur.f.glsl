// One pass of a separable 9-tap Gaussian blur. Called twice (horizontal then
// vertical) by GlowPostProcess to approximate a 2D blur cheaply. Sampling a
// full-res `image` into a lower-res destination (see GlowPostProcess) also
// does the downsample for free — no separate downsample pass needed.

uniform sampler2D image;
uniform highp vec2 direction; // (1,0)/srcSize or (0,1)/srcSize

in highp vec2 uv;

out lowp vec4 fragmentColor;

void main() {
    // Standard 9-tap Gaussian weights (sigma ~ 2), symmetric around center.
    const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 result = texture(image, uv).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = direction * float(i);
        result += texture(image, uv + offset).rgb * weights[i];
        result += texture(image, uv - offset).rgb * weights[i];
    }

    fragmentColor = vec4(result, 1.0);
}
