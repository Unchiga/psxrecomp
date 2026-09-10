#version 450
/* Host-overlay fragment: sample the ARGB layer and let fixed-function blending
 * composite it. Straight (non-premultiplied) alpha, matching how every host
 * overlay in this runtime authors its buffer -- the pipeline is configured
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA to agree.
 *
 * Filtering is the sampler's business, not this shader's: the menu and the
 * guest overlays are pixel art scaled by whole numbers, and the sampler is
 * created NEAREST so an upscaled overlay stays sharp instead of turning into
 * the blurred version of itself that a linear filter would give. */
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag;
void main() {
    frag = texture(u_src, v_uv);
}
