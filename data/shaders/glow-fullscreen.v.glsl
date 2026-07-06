// Draws a single triangle that covers the whole viewport, using the classic
// "big triangle" trick — no vertex buffer needed, positions are derived
// purely from gl_VertexID. Used by every post-process pass (blur, composite).

out highp vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
