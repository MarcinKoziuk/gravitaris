// Bright-pass extraction: subtracts `threshold` from each color channel
// before the scene is downsampled+blurred for bloom. Without this, blurring
// a large flat-filled area (e.g. a UI dialog's background) returns that same
// color almost unchanged — since the composite pass then ADDS the blurred
// result back on top of the sharp scene, any sufficiently large uniform
// region gets a flat brightness boost equal to intensity*itself, regardless
// of how far from an edge it is (and gets WORSE as the area grows, since more
// of it sits farther than the blur radius from any darker edge). Only pixels
// brighter than `threshold` (thin vector lines, not dim UI panel fills)
// should bloom at all.
//
// Also does a 4-tap 2x2 box downsample (matching what a linear-filtered blit
// would sample) so this doubles as the first half-res downsample step —
// thresholding each tap individually, before averaging, so a bright edge
// pixel can't "smuggle" a neighboring dim fill pixel through the average.

uniform sampler2D image;
uniform highp vec2 texelSize; // 1 / full-res source size
uniform highp float threshold;

in highp vec2 uv;

out lowp vec4 fragmentColor;

vec3 sampleThresholded(vec2 sampleUv) {
    vec3 color = texture(image, sampleUv).rgb;
    return max(color - vec3(threshold), vec3(0.0));
}

void main() {
    highp vec2 o = texelSize * 0.5;
    vec3 result = sampleThresholded(uv + vec2(-o.x, -o.y))
                + sampleThresholded(uv + vec2( o.x, -o.y))
                + sampleThresholded(uv + vec2(-o.x,  o.y))
                + sampleThresholded(uv + vec2( o.x,  o.y));

    fragmentColor = vec4(result * 0.25, 1.0);
}
