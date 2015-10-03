#version 130

in vec2 position;
in vec2 texcoord;
out vec2 tc0;
//out vec2 tc1;
uniform vec2 foo_chroma_offset_0;
//uniform vec2 foo_chroma_offset_1;

void main()
{
	// The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is:
	//
	//   2.000  0.000  0.000 -1.000
	//   0.000  2.000  0.000 -1.000
	//   0.000  0.000 -2.000 -1.000
	//   0.000  0.000  0.000  1.000
	gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0);
	vec2 flipped_tc = texcoord;
//	flipped_tc.y = 1.0 - flipped_tc.y;
	tc0 = flipped_tc + foo_chroma_offset_0;
//	tc1 = flipped_tc + foo_chroma_offset_1;
}
