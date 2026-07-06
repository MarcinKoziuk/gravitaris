#version 110

varying vec2 vTexCoord;

uniform sampler2D fboTexture;
uniform float time;
uniform vec2 resolution;

uniform vec2 blurDirection;
uniform float blurRadius;

#ifndef FXAA_REDUCE_MIN
    #define FXAA_REDUCE_MIN   (1.0/ 128.0)
#endif
#ifndef FXAA_REDUCE_MUL
    #define FXAA_REDUCE_MUL   (1.0 / 8.0)
#endif
#ifndef FXAA_SPAN_MAX
    #define FXAA_SPAN_MAX     8.0
#endif

//optimized version for mobile, where dependent 
//texture reads can be a bottleneck
vec4 fxaa(sampler2D tex, vec2 fragCoord, vec2 resolution,
            vec2 v_rgbNW, vec2 v_rgbNE, 
            vec2 v_rgbSW, vec2 v_rgbSE, 
            vec2 v_rgbM) {
    vec4 color;
    vec2 inverseVP = vec2(1.0 / resolution.x, 1.0 / resolution.y);
    vec3 rgbNW = texture2D(tex, v_rgbNW).xyz;
    vec3 rgbNE = texture2D(tex, v_rgbNE).xyz;
    vec3 rgbSW = texture2D(tex, v_rgbSW).xyz;
    vec3 rgbSE = texture2D(tex, v_rgbSE).xyz;
    vec4 texColor = texture2D(tex, v_rgbM);
    vec3 rgbM  = texColor.xyz;
    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                          (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
    
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
              max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
              dir * rcpDirMin)) * inverseVP;
    
    vec3 rgbA = 0.5 * (
        texture2D(tex, fragCoord * inverseVP + dir * (1.0 / 3.0 - 0.5)).xyz +
        texture2D(tex, fragCoord * inverseVP + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture2D(tex, fragCoord * inverseVP + dir * -0.5).xyz +
        texture2D(tex, fragCoord * inverseVP + dir * 0.5).xyz);

    float lumaB = dot(rgbB, luma);
    if ((lumaB < lumaMin) || (lumaB > lumaMax))
        color = vec4(rgbA, texColor.a);
    else
        color = vec4(rgbB, texColor.a);
    return color;
}

void texcoords(vec2 fragCoord, vec2 resolution,
			out vec2 v_rgbNW, out vec2 v_rgbNE,
			out vec2 v_rgbSW, out vec2 v_rgbSE,
			out vec2 v_rgbM) {
	vec2 inverseVP = 1.0 / resolution.xy;
	v_rgbNW = (fragCoord + vec2(-1.0, -1.0)) * inverseVP;
	v_rgbNE = (fragCoord + vec2(1.0, -1.0)) * inverseVP;
	v_rgbSW = (fragCoord + vec2(-1.0, 1.0)) * inverseVP;
	v_rgbSE = (fragCoord + vec2(1.0, 1.0)) * inverseVP;
	v_rgbM = vec2(fragCoord * inverseVP);
}

vec4 apply(sampler2D tex, vec2 fragCoord, vec2 resolution) {
	vec2 v_rgbNW;
	vec2 v_rgbNE;
	vec2 v_rgbSW;
	vec2 v_rgbSE;
	vec2 v_rgbM;

	//compute the texture coords
	texcoords(fragCoord, resolution, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM);
	
	//compute FXAA
	return fxaa(tex, fragCoord, resolution, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM);
}

void main(void)
{
	vec2 texcoord = vTexCoord;
	//texcoord.x += sin(texcoord.y * 4.0*2.0*3.14159 + (time/2.4)) / 100.0;

	//texcoord.x += sin(texcoord.y * 4.0*2.0*3.14159 + (time/100.0)) / 1000.0;
	//texcoord.y += cos(texcoord.x * 4.0*2.0*3.14159 + (time/100.0)) / 1000.0;

	//gl_FragColor = texture2D(fboTexture, texcoord);

	//this will be our RGBA sum
    vec4 sum = vec4(0.0);

    //our original texcoord for this fragment
    vec2 tc = texcoord;

    //the amount to blur, i.e. how far off center to sample from 
    //1.0 -> blur by one pixel
    //2.0 -> blur by two pixels, etc.
    float blur = 0.0;
	if (blurDirection.x > 0.0)
		blur = blurRadius / resolution.x;
	else
		blur = blurRadius / resolution.y;

    //the direction of our blur
    //(1.0, 0.0) -> x-axis blur
    //(0.0, 1.0) -> y-axis blur
    float hstep = blurDirection.x;
    float vstep = blurDirection.y;

    //apply blurring, using a 9-tap filter with predefined gaussian weights

	//0	0.000003	0.000229	0.005977	0.060598	0.24173	0.382925

	float mul = 1.25;
	
	sum += texture2D(fboTexture, vec2(tc.x - 6.0*blur*hstep, tc.y - 6.0*blur*vstep)) * mul * 0.0;
	sum += texture2D(fboTexture, vec2(tc.x - 5.0*blur*hstep, tc.y - 5.0*blur*vstep)) * mul * 0.000003;
    sum += texture2D(fboTexture, vec2(tc.x - 4.0*blur*hstep, tc.y - 4.0*blur*vstep)) * mul * 0.000229;
    sum += texture2D(fboTexture, vec2(tc.x - 3.0*blur*hstep, tc.y - 3.0*blur*vstep)) * mul * 0.005977;
    sum += texture2D(fboTexture, vec2(tc.x - 2.0*blur*hstep, tc.y - 2.0*blur*vstep)) * mul * 0.060598;
    sum += texture2D(fboTexture, vec2(tc.x - 1.0*blur*hstep, tc.y - 1.0*blur*vstep)) * mul * 0.24173;

    sum += texture2D(fboTexture, vec2(tc.x, tc.y)) * 0.382925;

    sum += texture2D(fboTexture, vec2(tc.x + 1.0*blur*hstep, tc.y + 1.0*blur*vstep)) * mul * 0.24173;
    sum += texture2D(fboTexture, vec2(tc.x + 2.0*blur*hstep, tc.y + 2.0*blur*vstep)) * mul * 0.060598;
    sum += texture2D(fboTexture, vec2(tc.x + 3.0*blur*hstep, tc.y + 3.0*blur*vstep)) * mul * 0.005977;
    sum += texture2D(fboTexture, vec2(tc.x + 4.0*blur*hstep, tc.y + 4.0*blur*vstep)) * mul * 0.000229;
	sum += texture2D(fboTexture, vec2(tc.x + 5.0*blur*hstep, tc.y + 5.0*blur*vstep)) * mul * 0.000003;
	sum += texture2D(fboTexture, vec2(tc.x + 6.0*blur*hstep, tc.y + 6.0*blur*vstep)) * mul * 0.0;
	
	/*
    sum += texture2D(fboTexture, vec2(tc.x - 4.0*blur*hstep, tc.y - 4.0*blur*vstep)) * 0.0162162162;
    sum += texture2D(fboTexture, vec2(tc.x - 3.0*blur*hstep, tc.y - 3.0*blur*vstep)) * 0.0540540541;
    sum += texture2D(fboTexture, vec2(tc.x - 2.0*blur*hstep, tc.y - 2.0*blur*vstep)) * 0.1216216216;
    sum += texture2D(fboTexture, vec2(tc.x - 1.0*blur*hstep, tc.y - 1.0*blur*vstep)) * 0.1945945946;

    sum += texture2D(fboTexture, vec2(tc.x, tc.y)) * 0.2270270270;

    sum += texture2D(fboTexture, vec2(tc.x + 1.0*blur*hstep, tc.y + 1.0*blur*vstep)) * 0.1945945946;
    sum += texture2D(fboTexture, vec2(tc.x + 2.0*blur*hstep, tc.y + 2.0*blur*vstep)) * 0.1216216216;
    sum += texture2D(fboTexture, vec2(tc.x + 3.0*blur*hstep, tc.y + 3.0*blur*vstep)) * 0.0540540541;
    sum += texture2D(fboTexture, vec2(tc.x + 4.0*blur*hstep, tc.y + 4.0*blur*vstep)) * 0.0162162162;
	*/


	gl_FragColor = texture2D(fboTexture, texcoord) + vec4(sum.rgb, 1.0);

	//gl_FragColor = texture2D(fboTexture, texcoord);

	//deze tewee
	//vec4 color = apply(fboTexture, texcoord * resolution, resolution);
    //gl_FragColor = color + vec4(sum.rgb, 1.0);
}