uniform mat3 transformationProjectionMatrix;

in highp vec2 position;
in lowp vec4 color;

out lowp vec4 interpolatedColor;

void main() {
    gl_Position.xywz = vec4(transformationProjectionMatrix * vec3(position, 1.0), 0.0);
    interpolatedColor = color;
}
