// AirborneNX: Xenos rectangle lists for the native renderer. Each input triangle is three corners of a
// rectangle; the fourth is the mirror of the vertex opposite the longest edge (the diagonal), and the four go
// out as a strip of two triangles. Same construction as the emulation's geometry shader
// (graphics/vulkan/pipeline_cache.cpp, PipelineGeometryShader::kRectangleList). The interface is the one every
// vertex shader of tools/shaders_native has: the position, TEXCOORD0-15 at locations 0-15 and COLOR0-5 at
// 16-21. Interpolated values are mirrored like the position.
//
// Built with: dxc -spirv -T gs_6_6 -E main -fspv-target-env=vulkan1.2 -Fh rect_list_gs.h -Vn kRectListGs
//             rect_list_gs.hlsl   (the header is then converted to uint32 words, see the commit)
struct Vertex {
  float4 pos : SV_Position;
  [[vk::location(0)]] float4 v0 : TEXCOORD0;
  [[vk::location(1)]] float4 v1 : TEXCOORD1;
  [[vk::location(2)]] float4 v2 : TEXCOORD2;
  [[vk::location(3)]] float4 v3 : TEXCOORD3;
  [[vk::location(4)]] float4 v4 : TEXCOORD4;
  [[vk::location(5)]] float4 v5 : TEXCOORD5;
  [[vk::location(6)]] float4 v6 : TEXCOORD6;
  [[vk::location(7)]] float4 v7 : TEXCOORD7;
  [[vk::location(8)]] float4 v8 : TEXCOORD8;
  [[vk::location(9)]] float4 v9 : TEXCOORD9;
  [[vk::location(10)]] float4 v10 : TEXCOORD10;
  [[vk::location(11)]] float4 v11 : TEXCOORD11;
  [[vk::location(12)]] float4 v12 : TEXCOORD12;
  [[vk::location(13)]] float4 v13 : TEXCOORD13;
  [[vk::location(14)]] float4 v14 : TEXCOORD14;
  [[vk::location(15)]] float4 v15 : TEXCOORD15;
  [[vk::location(16)]] float4 v16 : COLOR0;
  [[vk::location(17)]] float4 v17 : COLOR1;
  [[vk::location(18)]] float4 v18 : COLOR2;
  [[vk::location(19)]] float4 v19 : COLOR3;
  [[vk::location(20)]] float4 v20 : COLOR4;
  [[vk::location(21)]] float4 v21 : COLOR5;
};

[maxvertexcount(4)]
void main(triangle Vertex i[3], inout TriangleStream<Vertex> strip) {
  const float2 p0 = i[0].pos.xy, p1 = i[1].pos.xy, p2 = i[2].pos.xy;
  const float e12 = dot(p2 - p1, p2 - p1), e20 = dot(p0 - p2, p0 - p2), e01 = dot(p1 - p0, p1 - p0);
  // The longest edge is the diagonal; a is the vertex opposite it.
  uint a = 2, b = 0, c = 1;
  if (e12 > e20 && e12 > e01) {
    a = 0; b = 1; c = 2;
  } else if (e20 > e01) {
    a = 1; b = 2; c = 0;
  }
  Vertex d;
  d.pos = i[b].pos + i[c].pos - i[a].pos;
  d.v0 = i[b].v0 + i[c].v0 - i[a].v0;
  d.v1 = i[b].v1 + i[c].v1 - i[a].v1;
  d.v2 = i[b].v2 + i[c].v2 - i[a].v2;
  d.v3 = i[b].v3 + i[c].v3 - i[a].v3;
  d.v4 = i[b].v4 + i[c].v4 - i[a].v4;
  d.v5 = i[b].v5 + i[c].v5 - i[a].v5;
  d.v6 = i[b].v6 + i[c].v6 - i[a].v6;
  d.v7 = i[b].v7 + i[c].v7 - i[a].v7;
  d.v8 = i[b].v8 + i[c].v8 - i[a].v8;
  d.v9 = i[b].v9 + i[c].v9 - i[a].v9;
  d.v10 = i[b].v10 + i[c].v10 - i[a].v10;
  d.v11 = i[b].v11 + i[c].v11 - i[a].v11;
  d.v12 = i[b].v12 + i[c].v12 - i[a].v12;
  d.v13 = i[b].v13 + i[c].v13 - i[a].v13;
  d.v14 = i[b].v14 + i[c].v14 - i[a].v14;
  d.v15 = i[b].v15 + i[c].v15 - i[a].v15;
  d.v16 = i[b].v16 + i[c].v16 - i[a].v16;
  d.v17 = i[b].v17 + i[c].v17 - i[a].v17;
  d.v18 = i[b].v18 + i[c].v18 - i[a].v18;
  d.v19 = i[b].v19 + i[c].v19 - i[a].v19;
  d.v20 = i[b].v20 + i[c].v20 - i[a].v20;
  d.v21 = i[b].v21 + i[c].v21 - i[a].v21;
  strip.Append(i[a]);
  strip.Append(i[b]);
  strip.Append(i[c]);
  strip.Append(d);
  strip.RestartStrip();
}
