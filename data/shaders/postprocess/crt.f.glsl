// GLSL ES (WebGL) requires an explicit default float precision in fragment
// shaders -- desktop GLSL has no such requirement and silently accepts this
// as a no-op, so it is unconditional rather than platform-guarded.
precision highp float;

// Simple CRT line effect: horizontal scanlines + a gentle brightness lift so
// the darkening doesn't crush the image. Runs as the final present pass over
// the (already composited) scene.
//
// Line width/spacing are defined at a 1080p reference and scaled by the
// actual window height, so a "2px line" looks the same relative size on a
// 4K display as on 1080p instead of becoming imperceptibly thin.

uniform sampler2D image;
uniform highp vec2 viewportSize;
uniform highp float scanlineStrength; // 0 = off, 1 = fully dark troughs
uniform highp float time;             // seconds, small-magnitude (see CrtShader::setTime)

// Scanline geometry, expressed at the 1080p reference and scaled by the actual
// window height (see dpiScale below). Runtime-tweakable via the debug UI.
//   lineWidthPx: dark-line thickness (~2px solid core after AA at the default 3)
//   periodPx:    line + gap (a "2px line" default of 6 => ~50% duty cycle)
uniform highp float lineWidthPx;
uniform highp float periodPx;

in highp vec2 uv;

out lowp vec4 fragmentColor;

const highp float REFERENCE_HEIGHT = 1080.0;

// Analog instability: the image content itself stays put — only the CRT
// *effects* shimmer. Three subtle, temporally-uneven modulations, all
// runtime-tweakable via the debug UI:
//   - whole-frame brightness flicker (phosphor/refresh flicker),
//   - scanline strength jitter, varying per row (unstable beam current),
//   - sub-pixel scanline phase jitter (raster breathing).
// "Uneven" comes from value noise (hashed, interpolated) rather than a clean
// sine, so nothing reads as a rhythmic pulse.
uniform highp float flickerRate;       // noise samples/sec (near-refresh)
uniform highp float flickerAmplitude;
uniform highp float scanJitterRate;
uniform highp float scanJitterAmplitude; // fraction of strength
uniform highp float phaseJitterPx;       // at the 1080p reference

highp float hash(highp float x) {
    return fract(sin(x * 12.9898) * 43758.5453);
}

// 1D value noise: smooth-but-uneven wander in [0,1].
highp float valueNoise(highp float x) {
    highp float i = floor(x);
    highp float f = fract(x);
    return mix(hash(i), hash(i + 1.0), f * f * (3.0 - 2.0 * f));
}

void main() {
    vec3 color = texture(image, uv).rgb;

    highp float dpiScale = viewportSize.y / REFERENCE_HEIGHT;
    highp float period = periodPx * dpiScale;
    highp float halfLineWidth = 0.5 * lineWidthPx * dpiScale;
    highp float aa = max(0.5 * dpiScale, 0.5); // ~1px soft edge, DPI-scaled

    // Raster breathing: the scanline grid drifts by a fraction of a pixel.
    highp float phaseJitter = phaseJitterPx * dpiScale
            * (valueNoise(time * scanJitterRate * 0.37) - 0.5);

    highp float pos = mod(gl_FragCoord.y + phaseJitter, period);
    highp float distToCenter = min(pos, period - pos);
    highp float dark = 1.0 - smoothstep(halfLineWidth - aa, halfLineWidth + aa, distToCenter);

    // Beam-width variation (Lottes-style): on a real CRT a bright beam widens
    // and bleeds into the gap between scanlines, so bright content is dimmed
    // far less than dark content. Without this, thin bright lines that run
    // nearly parallel to the scanlines get visibly chopped into dashes
    // wherever they drift through a dark trough.
    highp float luma = dot(color, vec3(0.299, 0.587, 0.114));
    highp float strength = scanlineStrength * (1.0 - 0.85 * clamp(luma, 0.0, 1.0));

    // Unstable beam current: scanline darkness wavers over time, slightly
    // differently per row so the shimmer doesn't move in lockstep.
    highp float rowSeed = gl_FragCoord.y * 0.013;
    highp float scanJitter = 1.0 + scanJitterAmplitude
            * (valueNoise(time * scanJitterRate + rowSeed) - 0.5);
    strength = clamp(strength * scanJitter, 0.0, 1.0);

    highp float scan = mix(1.0, 1.0 - dark, strength);

    // Compensate the average darkening a little so overall brightness holds up.
    color *= scan * (1.0 + 0.4 * strength);

    // Phosphor/refresh flicker: whole-frame brightness wanders subtly.
    color *= 1.0 + flickerAmplitude * (valueNoise(time * flickerRate) - 0.5);

    fragmentColor = vec4(color, 1.0);
}
