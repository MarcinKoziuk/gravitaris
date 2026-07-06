#version 110

attribute vec2 position;
attribute vec2 cornerVect;

varying vec2 iCornerVect;

uniform mat4 mvp;
uniform vec2 scale;
uniform float thickness;
uniform float zoom;

void main()
{
	vec2 dpos = position + (cornerVect/scale * max((thickness * 3.15)/zoom + 0.2, 1.5/zoom));

	//vec2 dpos = position + cornerVect*1.0;
	vec4 fragCoord = (mvp * vec4(dpos, 1.0, 1.0));



	iCornerVect = cornerVect;

	gl_Position = fragCoord;
}
