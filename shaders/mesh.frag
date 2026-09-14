#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
	//the vertex shader transforms the normal by the full model matrix, so any object scale ends
	//up in its length - a radius-100 preview sphere would otherwise be lit 100x too brightly
	float lightValue = max(dot(normalize(inNormal), sceneData.sunlightDirection.xyz), 0.1f);

	vec3 color = inColor * texture(colorTex,inUV).xyz;
	vec3 ambient = color *  sceneData.ambientColor.xyz;

	outFragColor = vec4(color * lightValue *  sceneData.sunlightColor.w + ambient ,1.0f);
}
