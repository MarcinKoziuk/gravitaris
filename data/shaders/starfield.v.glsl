// Background parallax starfield. Each star is a billboard quad: `center` is the
// star's layer-space position, `corner` the [-1,1]^2 quad corner. The quad is
// built in pixel space so on-screen size is stable across zoom.

uniform mat3 viewProjection;   // layer-space -> NDC (camera scaled by parallax factor)
uniform vec2 viewportSize;     // framebuffer size in pixels

in highp vec2 center;
in highp vec2 corner;
in highp vec2 params;          // x = size in pixels, y = brightness
in highp vec3 starColor;

out lowp vec3 vColor;
out lowp float vBrightness;
out highp vec2 vCorner;

vec2 toPixel(vec2 p) {
    vec3 clip = viewProjection * vec3(p, 1.0);
    return (clip.xy / clip.z) * 0.5 * viewportSize;
}

void main() {
    float size = params.x;
    vBrightness = params.y;
    vColor = starColor;
    vCorner = corner;

    vec2 centerPix = toPixel(center);
    vec2 pix = centerPix + corner * size;

    vec2 ndc = pix / (0.5 * viewportSize);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
