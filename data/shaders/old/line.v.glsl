#version 110

attribute vec2 position;

uniform mat4 mvp;

void main()
{
	gl_Position = (mvp * vec4(position, 1.0, 1.0));
}
