#include "effects_backend.h"
#include <wlr/util/log.h>

static bool vk_init(struct wlr_renderer *r, struct wlr_allocator *a) {
  (void)r;
  (void)a;
  wlr_log(WLR_INFO, "vk: Vulkan backend not implemented yet");
  return false;
}

static void vk_fini(void) {
}

static bool vk_output_init(be_output_state_t *state, int width, int height,
    int blur_w, int blur_h) {
  (void)state;
  (void)width;
  (void)height;
  (void)blur_w;
  (void)blur_h;
  return false;
}

static void vk_output_fini(be_output_state_t *state) {
  (void)state;
}

static void vk_output_resize(be_output_state_t *state, int width, int height,
    int blur_w, int blur_h) {
  (void)state;
  (void)width;
  (void)height;
  (void)blur_w;
  (void)blur_h;
}

static bool vk_ensure_buffer(struct wlr_buffer **buf, uint64_t native[2],
    int w, int h, struct wlr_renderer *r, struct wlr_allocator *a) {
  (void)buf;
  (void)native;
  (void)w;
  (void)h;
  (void)r;
  (void)a;
  return false;
}

static void vk_destroy_buffer(struct wlr_buffer *buf, uint64_t native[2]) {
  (void)buf;
  (void)native;
}

static void vk_frame_begin(void) {
}

static void vk_frame_end(void) {
}

static bool vk_blit(uint64_t src_tex, uint64_t dst_fbo, int w, int h,
    const pixman_box32_t *scissor, int n_scissor) {
  (void)src_tex;
  (void)dst_fbo;
  (void)w;
  (void)h;
  (void)scissor;
  (void)n_scissor;
  return false;
}

static bool vk_blur(be_output_state_t *state, uint64_t src_handle,
    int src_w, int src_h, struct be_blur_params *p,
    uint64_t *out_handle) {
  (void)state;
  (void)src_handle;
  (void)src_w;
  (void)src_h;
  (void)p;
  (void)out_handle;
  return false;
}

static bool vk_apply_mica_tint(be_output_state_t *state,
    uint64_t bg_handle, float tint[4], float tint_strength,
    uint64_t dst_fbo, int w, int h) {
  (void)state;
  (void)bg_handle;
  (void)tint;
  (void)tint_strength;
  (void)dst_fbo;
  (void)w;
  (void)h;
  return false;
}

static bool vk_apply_acrylic(be_output_state_t *state,
    uint64_t bg_handle, struct be_acrylic_params *p,
    uint64_t dst_fbo, int w, int h) {
  (void)state;
  (void)bg_handle;
  (void)p;
  (void)dst_fbo;
  (void)w;
  (void)h;
  return false;
}

static bool vk_render_shadow(struct be_shadow_params *p,
    uint64_t dst_fbo) {
  (void)p;
  (void)dst_fbo;
  return false;
}

static bool vk_render_border(struct be_border_params *p,
    uint64_t dst_fbo) {
  (void)p;
  (void)dst_fbo;
  return false;
}

static bool vk_apply_corner_mask(uint64_t dst_fbo, int dst_w, int dst_h,
    uint64_t bg_tex, struct be_corner_mask_params *p) {
  (void)dst_fbo;
  (void)dst_w;
  (void)dst_h;
  (void)bg_tex;
  (void)p;
  return false;
}

static bool vk_apply_screen_shader(uint64_t src_tex, uint64_t dst_fbo,
    int w, int h, struct be_screen_shader_params *p) {
  (void)src_tex;
  (void)dst_fbo;
  (void)w;
  (void)h;
  (void)p;
  return false;
}

static bool vk_capture_readback(struct wlr_buffer *capture_buffer,
    be_output_state_t *state, uint64_t dst_fbo, int dst_w, int dst_h,
    int src_w, int src_h,
    uint64_t *out_tex) {
  (void)capture_buffer;
  (void)state;
  (void)dst_fbo;
  (void)dst_w;
  (void)dst_h;
  (void)src_w;
  (void)src_h;
  (void)out_tex;
  return false;
}

static bool vk_compile_screen_shader(const char *frag_src) {
  (void)frag_src;
  return false;
}

static void vk_destroy_screen_shader(void) {
}

const effects_backend_t vk_backend = {
  .init = vk_init,
  .fini = vk_fini,
  .output_init = vk_output_init,
  .output_fini = vk_output_fini,
  .output_resize = vk_output_resize,
  .ensure_buffer = vk_ensure_buffer,
  .destroy_buffer = vk_destroy_buffer,
  .frame_begin = vk_frame_begin,
  .frame_end = vk_frame_end,
  .blit = vk_blit,
  .blur = vk_blur,
  .apply_mica_tint = vk_apply_mica_tint,
  .apply_acrylic = vk_apply_acrylic,
  .render_shadow = vk_render_shadow,
  .render_border = vk_render_border,
  .apply_corner_mask = vk_apply_corner_mask,
  .apply_screen_shader = vk_apply_screen_shader,
  .capture_readback = vk_capture_readback,
  .compile_screen_shader = vk_compile_screen_shader,
  .destroy_screen_shader = vk_destroy_screen_shader,
};
