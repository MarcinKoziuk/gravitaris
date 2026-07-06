#version 110

uniform vec3 color;

void main()
{
	gl_FragColor.xyz = color.xyz;
}
