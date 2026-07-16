// model-renderer2: line expansion is baked into per-vertex adjacency at load
// time (pointA/B/C + param), so instancing is free for per-entity transforms.
// The width offset is applied in pixel space, which keeps on-screen thickness
// constant at any zoom level without re-baking the mesh.

uniform mat3 viewProjection;   // world -> NDC (includes camera zoom + pan)
uniform vec2 viewportSize;     // framebuffer size in pixels
uniform float width;           // desired line thickness in pixels

in highp vec2 pointA;
in highp vec2 pointB;
in highp vec2 pointC;
in highp vec4 param;           // xyz: primitive weights, w: type (0 = segment, 1 = join, 2 = circle)
in highp vec3 color;
in highp float teamWeight;       // 1 = stroke authored in team placeholder color

in highp mat3 instanceTransform; // per-entity model matrix (locations 6,7,8)
in highp vec3 instanceTeamColor; // per-entity team color (location 9)
in highp float instanceFlash;    // per-entity hit-flash amount, 0..1 (location 10)

out lowp vec4 interpolatedColor;
// Which analytic-AA path the fragment shader should take; mirrors param.w.
out highp float primType;
// Signed distance from the centerline in pixels, for analytic edge AA in the
// fragment shader. For segment quads it is -extent at one edge and +extent at
// the other, so it interpolates through 0 at the centerline (the fragment
// shader takes abs()). Left at 0 for miter joins, which keep hard edges and
// rely on MSAA instead — their triangle-fan geometry doesn't have a single
// "distance from center" the way a straight quad does.
out highp float edgeDistance;
// Pixel-space circle center/radius, for the analytic circle path. The
// fragment shader reconstructs its own position from gl_FragCoord (computed
// directly by the rasterizer, full precision) rather than interpolating a
// vertex-shader position across the quad — on a circle as large as an orbit
// ring the billboard quad can cover most of the screen, and GPUs commonly
// interpolate varyings at reduced precision over long spans, which shows up
// as visible banding/moiré. gl_FragCoord avoids that interpolation entirely.
out highp vec2 circleCenterPix;
out highp float circleRadiusPix;

// Extra half-pixels added beyond the nominal half-width so the alpha falloff
// has room to fade outside the line without thinning it.
const float AA_FEATHER = 1.0;

vec2 toPixel(mat3 m, vec2 p) {
    vec3 clip = m * vec3(p, 1.0);
    return (clip.xy / clip.z) * 0.5 * viewportSize;
}

void main() {
    mat3 m = viewProjection * instanceTransform;
    vec2 a = toPixel(m, pointA);
    vec2 b = toPixel(m, pointB);

    vec2 pix;
    primType = param.w;
    edgeDistance = 0.0;
    circleCenterPix = vec2(0.0);
    circleRadiusPix = 0.0;

    if (param.w < 0.5) {
        // Segment quad from A to B. param.x = t along A->B, param.y = side (+-0.5).
        // Push the edge out by AA_FEATHER px beyond the nominal half-width so the
        // fragment shader can fade to transparent without thinning the line.
        vec2 xBasis = b - a;
        vec2 yBasis = normalize(vec2(-xBasis.y, xBasis.x));
        float sideSign = sign(param.y);
        float extent = width * 0.5 + AA_FEATHER;
        pix = mix(a, b, param.x) + yBasis * sideSign * extent;
        edgeDistance = sideSign * extent;
    } else if (param.w < 1.5) {
        // Miter join centred at B. Same math as the instanced renderer, but in
        // pixel space. param.xyz are the p0/p1/p2 corner weights.
        vec2 c = toPixel(m, pointC);
        vec2 tangent = normalize(normalize(c - b) + normalize(b - a));
        vec2 miter = vec2(-tangent.y, tangent.x);

        vec2 ab = b - a;
        vec2 cb = b - c;
        vec2 abNorm = normalize(vec2(-ab.y, ab.x));
        vec2 cbNorm = normalize(-vec2(-cb.y, cb.x));

        float sigma = sign(dot(ab + cb, miter));

        vec2 p0 = 0.5 * width * sigma * (sigma < 0.0 ? abNorm : cbNorm);
        vec2 p1 = 0.5 * miter * sigma * width / dot(miter, abNorm);
        vec2 p2 = 0.5 * width * sigma * (sigma < 0.0 ? cbNorm : abNorm);

        pix = b + param.x * p0 + param.y * p1 + param.z * p2;
    } else if (param.w < 2.5) {
        // Circle ring billboard. pointA = center (a), pointB.x = radius in
        // local units (not a real point — measure it in pixel space by
        // comparing the transformed center to a point `radius` away along
        // local X, so rotation/zoom/instance scale are all accounted for).
        // param.xy are the quad corner directions in [-1,1].
        vec2 edgePix = toPixel(m, pointA + vec2(pointB.x, 0.0));
        circleCenterPix = a;
        circleRadiusPix = length(edgePix - a);

        float extent = circleRadiusPix + width * 0.5 + AA_FEATHER;
        pix = a + param.xy * extent;
    } else if (param.w < 3.5) {
        // Polygon fill triangle: pointA is the vertex position, transformed
        // straight to pixel space with no expansion. Flat opaque coverage.
        pix = a;
    } else {
        // Filled disc: same radius measurement as the ring, but the quad only
        // needs to cover the disc itself (+AA), and the fragment shader fills
        // the interior instead of the ring.
        vec2 edgePix = toPixel(m, pointA + vec2(pointB.x, 0.0));
        circleCenterPix = a;
        circleRadiusPix = length(edgePix - a);

        float extent = circleRadiusPix + AA_FEATHER;
        pix = a + param.xy * extent;
    }

    vec2 ndc = pix / (0.5 * viewportSize);
    gl_Position = vec4(ndc, 0.0, 1.0);
    // Placeholder-authored strokes take the instance's team color; all other
    // strokes keep their baked SVG color (StarCraft-style team mask). Hit
    // flash then whitens the whole result on top, team color and all.
    vec3 teamMixed = mix(color, instanceTeamColor, teamWeight);
    interpolatedColor = vec4(mix(teamMixed, vec3(1.0), instanceFlash), 1.0);
}
