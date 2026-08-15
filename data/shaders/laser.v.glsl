uniform mat3 transformationProjectionMatrix;

in highp vec2 position;
// rgb is the beam's colour; a is how far this corner sits from the mount,
// measured in fade lengths. Not an opacity, and it runs well past lowp's
// guaranteed range, so it cannot be declared alongside the colour as one.
in mediump vec4 color;

out lowp vec3 interpolatedColor;
out mediump float fadeDistance;

void main() {
    gl_Position.xywz = vec4(transformationProjectionMatrix * vec3(position, 1.0), 0.0);
    interpolatedColor = color.rgb;
    fadeDistance = color.a;
}
