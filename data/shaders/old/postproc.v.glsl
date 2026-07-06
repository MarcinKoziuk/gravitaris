#version 110

attribute vec2 texCoord;

varying vec2 vTexCoord;

uniform vec2 resolution;

void main(void)
{
	gl_Position = vec4(texCoord, 0.0, 1.0);
	vTexCoord = (texCoord + 1.0) / 2.0;
}