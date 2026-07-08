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

in highp vec2 uv;

out lowp vec4 fragmentColor;

const highp float REFERENCE_HEIGHT = 1080.0;
const highp float LINE_WIDTH_PX = 2.0;  // at the 1080p reference
const highp float PERIOD_PX = 4.0;      // line + gap, at the 1080p reference

void main() {
    vec3 color = texture(image, uv).rgb;

    highp float dpiScale = viewportSize.y / REFERENCE_HEIGHT;
    highp float period = PERIOD_PX * dpiScale;
    highp float halfLineWidth = 0.5 * LINE_WIDTH_PX * dpiScale;
    highp float aa = max(0.5 * dpiScale, 0.5); // ~1px soft edge, DPI-scaled

    highp float pos = mod(gl_FragCoord.y, period);
    highp float distToCenter = min(pos, period - pos);
    highp float dark = 1.0 - smoothstep(halfLineWidth - aa, halfLineWidth + aa, distToCenter);

    // Beam-width variation (Lottes-style): on a real CRT a bright beam widens
    // and bleeds into the gap between scanlines, so bright content is dimmed
    // far less than dark content. Without this, thin bright lines that run
    // nearly parallel to the scanlines get visibly chopped into dashes
    // wherever they drift through a dark trough.
    highp float luma = dot(color, vec3(0.299, 0.587, 0.114));
    highp float strength = scanlineStrength * (1.0 - 0.85 * clamp(luma, 0.0, 1.0));

    highp float scan = mix(1.0, 1.0 - dark, strength);

    // Compensate the average darkening a little so overall brightness holds up.
    color *= scan * (1.0 + 0.4 * strength);

    fragmentColor = vec4(color, 1.0);
}
