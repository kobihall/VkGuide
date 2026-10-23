#version 450

//one world-space box for an occlusion query (VulkanEngine::drawGeometry()): 36 vertices, its 12 triangles, from
//the box's corners in push constants and no vertex buffer

layout(set = 0, binding = 0) uniform SceneData {
	mat4 view;
	mat4 proj;
	mat4 viewproj;
	vec4 ambientColor;
	vec4 sunlightDirection;
	vec4 sunlightColor;
} sceneData;

layout(push_constant) uniform Box {
	vec4 lo;
	vec4 hi;
} box;

//each face's two triangles, by corner number: bit 0 is x, bit 1 y and bit 2 z (0 for lo, 1 for hi)
const int CORNERS[36] = int[](
	0, 2, 1, 1, 2, 3,
	4, 5, 6, 5, 7, 6,
	0, 1, 4, 1, 5, 4,
	2, 6, 3, 3, 6, 7,
	0, 4, 2, 2, 4, 6,
	1, 3, 5, 3, 7, 5);

void main()
{
	const int corner = CORNERS[gl_VertexIndex];
	const vec3 t = vec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
	gl_Position = sceneData.viewproj * vec4(mix(box.lo.xyz, box.hi.xyz, t), 1.0);
}
