#version 110

uniform vec3 color;

varying vec2 iCornerVect;

void main()
{
	vec4 c;
	c.xyz = (color.xyz / 1.25) + vec3(0.18, 0.18, 0.18);
	//c.a = (1.0 - ((length(iCornerVect)*2.0) - 0.50)); //was
	gl_FragColor = c;
	//gl_FragColor = vec4(c.rgb, 0.6);
	//gl_FragColor = vec4(color, 1.0);

	gl_FragColor.xyz = (color.xyz / 1.25) + vec3(0.18, 0.18, 0.18);
	gl_FragColor.w = 0.5;
}
