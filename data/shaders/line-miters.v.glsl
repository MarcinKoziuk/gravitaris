uniform mat3 transformationProjectionMatrix;
uniform float width;
uniform vec3 color;
in highp vec3 position;
in highp vec2 instancePointA;
in highp vec2 instancePointB;
in highp vec2 instancePointC;
out lowp vec4 interpolatedColor;

void main() {
    vec2 tangent = normalize(normalize(instancePointC - instancePointB) + normalize(instancePointB - instancePointA));
    vec2 miter = vec2(-tangent.y, tangent.x);

    vec2 ab = instancePointB - instancePointA;
    vec2 cb = instancePointB - instancePointC;
    vec2 abNorm = normalize(vec2(-ab.y, ab.x));
    vec2 cbNorm = normalize(-vec2(-cb.y, cb.x));

    float sigma = sign(dot(ab + cb, miter));

    vec2 p0 = 0.5 * width * sigma * (sigma < 0.0 ? abNorm : cbNorm);
    vec2 p1 = 0.5 * miter * sigma * width / dot(miter, abNorm);
    vec2 p2 = 0.5 * width * sigma * (sigma < 0.0 ? cbNorm : abNorm);

    vec2 point = instancePointB + position.x * p0 + position.y * p1 + position.z * p2;

    gl_Position.xywz = vec4(transformationProjectionMatrix * vec3(point, 1.0), 0.0);
    interpolatedColor = vec4(color, 1.0);
}
