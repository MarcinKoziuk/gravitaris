// Analytic edge AA for the baked line renderer, using screen-space partial
// derivatives (fwidth) rather than a fixed 1px falloff — the standard
// technique for SDF/distance-based antialiasing (see Chris Green's "Improved
// Alpha-Tested Magnification for Vector Textures and Special Effects", Valve
// 2007, and Inigo Quilez's SDF articles). A fixed-width falloff assumes 1
// distance-unit always maps to ~1 screen pixel, which isn't reliably true;
// dividing by fwidth(dist) instead adapts the falloff to however many pixels
// that unit actually covers locally, which is the "true" fix for the
// shallow-tangent-angle moiré a fixed falloff can show on large curves.
//
// Segments: edgeDistance is the fragment's SIGNED distance from the
// centerline in pixels (see line2.v.glsl).
//
// Circles: distance is computed from gl_FragCoord directly (see comment in
// line2.v.glsl on why — avoids precision loss from interpolating a position
// varying across a billboard quad that can span most of the screen).
//
// Miter joins carry primType == 1 and get neither treatment: they stay fully
// opaque/hard-edged (MSAA covers their silhouette).

uniform highp float width;
uniform highp vec2 viewportSize;

in lowp vec4 interpolatedColor;
in highp float primType;
in highp float edgeDistance;
in highp vec2 circleCenterPix;
in highp float circleRadiusPix;

out lowp vec4 fragmentColor;

// Normalized signed-distance-to-edge antialiasing: 1 well inside the shape,
// 0 well outside, with a ~1-derivative-wide ramp straddling the true edge.
float coverageFromSignedDistance(float insideDistance) {
    highp float aa = max(fwidth(insideDistance), 1e-5);
    return clamp(insideDistance / aa + 0.5, 0.0, 1.0);
}

void main() {
    highp float halfWidth = width * 0.5;
    highp float coverage;

    if (primType < 0.5) {
        // Segment: positive while inside the stroke, negative outside.
        coverage = coverageFromSignedDistance(halfWidth - abs(edgeDistance));
    } else if (primType < 1.5) {
        // Miter join
        coverage = 1.0;
    } else if (primType < 2.5) {
        // Circle ring: radial distance from the ring's true edge, using the
        // rasterizer-provided fragment position instead of an interpolated one.
        highp vec2 fragPix = gl_FragCoord.xy - 0.5 * viewportSize;
        highp float dist = distance(fragPix, circleCenterPix) - circleRadiusPix;
        coverage = coverageFromSignedDistance(halfWidth - abs(dist));
    } else if (primType < 3.5) {
        // Polygon fill triangle: flat, fully opaque interior.
        coverage = 1.0;
    } else {
        // Filled disc: opaque inside the radius, AA-fading across the edge.
        highp vec2 fragPix = gl_FragCoord.xy - 0.5 * viewportSize;
        highp float insideDistance = circleRadiusPix - distance(fragPix, circleCenterPix);
        coverage = coverageFromSignedDistance(insideDistance);
    }

    fragmentColor = vec4(interpolatedColor.rgb, interpolatedColor.a * coverage);
}
