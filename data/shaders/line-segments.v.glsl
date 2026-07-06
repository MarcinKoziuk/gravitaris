uniform mat3 transformationProjectionMatrix;
uniform float width;
uniform vec3 color;
in highp vec2 position;
in highp vec2 instancePointA;
in highp vec2 instancePointB;
out lowp vec4 interpolatedColor;

void main() {
    vec2 xBasis = instancePointB - instancePointA;
    vec2 yBasis = normalize(vec2(-xBasis.y, xBasis.x));
    vec2 point = instancePointA + xBasis * position.x + yBasis * width * position.y;

    gl_Position.xywz = vec4(transformationProjectionMatrix * vec3(point, 1.0), 0.0);
    interpolatedColor = vec4(color, 1.0);
}
