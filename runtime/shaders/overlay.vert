#version 450
/* Host-overlay quad (vertex-less), for compositing an ARGB layer onto the
 * swapchain with real alpha blending.
 *
 * Separate from blit.vert because that one maps its rect through native VRAM
 * dimensions (512x256) into the internal render target. Overlays are placed in
 * SWAPCHAIN pixels and land on the presented image, so the rect arrives already
 * in normalised device coordinates and the vertex stage does no mapping at all.
 * Six gl_VertexIndex positions form the two triangles. */
layout(push_constant) uniform PC {
    vec4 u_rect;    /* x0, y0, x1, y1 in NDC */
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 c[6] = vec2[6](vec2(0,0), vec2(1,0), vec2(0,1),
                        vec2(1,0), vec2(0,1), vec2(1,1));
    vec2 uv = c[gl_VertexIndex];
    v_uv = uv;
    gl_Position = vec4(mix(pc.u_rect.x, pc.u_rect.z, uv.x),
                       mix(pc.u_rect.y, pc.u_rect.w, uv.y), 0.0, 1.0);
}
