uniform mat3 transformationProjectionMatrix;
uniform vec3 color;
in highp vec2 position;
out lowp vec4 interpolatedColor;

void main() {
    gl_Position.xywz = vec4(transformationProjectionMatrix * vec3(position, 1.0), 0.0);
    interpolatedColor = vec4(color, 0.6);
}
