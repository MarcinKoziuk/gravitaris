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

in highp vec2 uv;

out lowp vec4 fragmentColor;

const highp float REFERENCE_HEIGHT = 1080.0;
const highp float LINE_WIDTH_PX = 3.0;  // at the 1080p reference (~2px solid core after AA)
const highp float PERIOD_PX = 6.0;      // line + gap, at the 1080p reference (~50% duty)

// Analog instability: the image content itself stays put — only the CRT
// *effects* shimmer. Three subtle, temporally-uneven modulations:
//   - whole-frame brightness flicker (phosphor/refresh flicker),
//   - scanline strength jitter, varying per row (unstable beam current),
//   - sub-pixel scanline phase jitter (raster breathing).
// "Uneven" comes from value noise (hashed, interpolated) rather than a clean
// sine, so nothing reads as a rhythmic pulse.
const highp float FLICKER_RATE = 47.0;    // noise samples/sec (near-refresh)
const highp float FLICKER_AMPLITUDE = 0.035;
const highp float SCAN_JITTER_RATE = 61.0;
const highp float SCAN_JITTER_AMPLITUDE = 0.25;  // fraction of strength
const highp float PHASE_JITTER_PX = 0.5;         // at the 1080p reference

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
    highp float period = PERIOD_PX * dpiScale;
    highp float halfLineWidth = 0.5 * LINE_WIDTH_PX * dpiScale;
    highp float aa = max(0.5 * dpiScale, 0.5); // ~1px soft edge, DPI-scaled

    // Raster breathing: the scanline grid drifts by a fraction of a pixel.
    highp float phaseJitter = PHASE_JITTER_PX * dpiScale
            * (valueNoise(time * SCAN_JITTER_RATE * 0.37) - 0.5);

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
    highp float scanJitter = 1.0 + SCAN_JITTER_AMPLITUDE
            * (valueNoise(time * SCAN_JITTER_RATE + rowSeed) - 0.5);
    strength = clamp(strength * scanJitter, 0.0, 1.0);

    highp float scan = mix(1.0, 1.0 - dark, strength);

    // Compensate the average darkening a little so overall brightness holds up.
    color *= scan * (1.0 + 0.4 * strength);

    // Phosphor/refresh flicker: whole-frame brightness wanders subtly.
    color *= 1.0 + FLICKER_AMPLITUDE * (valueNoise(time * FLICKER_RATE) - 0.5);

    fragmentColor = vec4(color, 1.0);
}
