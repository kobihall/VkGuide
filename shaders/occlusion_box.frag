#version 450

//the occlusion boxes write no color (the pipeline's write mask is empty) and no depth; the query counts the samples
//that pass the depth test
layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vec4(0.0);
}
