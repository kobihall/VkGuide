#version 450

#extension GL_GOOGLE_include_directive : require

#include "input_structures.glsl"

//the vertex is the 48-byte Vertex of vk_types.h, read through the pipeline's vertex input rather than a
//buffer device address: MoltenVK makes every device-addressable buffer resident again on each draw of a
//pipeline that declares the capability, which cost a 57,000-draw scene two seconds a frame
layout (location = 0) in vec3 inPosition;
layout (location = 1) in float inUvX;
layout (location = 2) in vec3 inNormal;
layout (location = 3) in float inUvY;
layout (location = 4) in vec4 inColor;
//per instance: the object's world matrix, one column per location (5 to 8)
layout (location = 5) in mat4 inWorldMatrix;

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;

void main()
{
	vec4 position = vec4(inPosition, 1.0f);

	gl_Position =  sceneData.viewproj * inWorldMatrix * position;

	outNormal = (inWorldMatrix * vec4(inNormal, 0.f)).xyz;
	outColor = inColor.xyz * materialData.colorFactors.xyz;
	outUV.x = inUvX;
	outUV.y = inUvY;
}
