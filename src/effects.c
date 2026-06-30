#include "animation.h"
#include "effects.h"
#include "tree.h"
#include "layer.h"
#include "output.h"
#include "server.h"
#include "toplevel.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/gles2.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/util/log.h>

#include "effect_tex_vert_src.h"
#include "blit_frag_src.h"
#include "ext_blit_frag_src.h"
#include "border_frag_src.h"
#include "border_corner_mask_frag_src.h"
#include "blur_kawase_frag_src.h"
#include "blur_box_h_frag_src.h"
#include "blur_box_v_frag_src.h"
#include "blur_gauss_h_frag_src.h"
#include "blur_gauss_v_frag_src.h"
#include "blur_mica_frag_src.h"
#include "blur_acrylic_frag_src.h"
#include "blur_refraction_frag_src.h"
#include "grayscale_frag_src.h"
#include "invert_frag_src.h"
#include "nightlight_frag_src.h"
#include "sepia_frag_src.h"
#include "shadow_frag_src.h"

enum blur_algorithm blur_algorithm = BLUR_ALGORITHM_KAWASE;

static const struct wlr_drm_format *s_render_fmt = NULL;
bool blur_enabled = true;
int blur_passes = 1;
float blur_radius = 5.0f;
int blur_downsample = 4;

float blur_vibrancy = 0.0f;
float blur_vibrancy_darkness = 0.5f;
float blur_noise_strength = 0.0f;
float blur_brightness = 1.0f;
float blur_contrast = 1.0f;

bool mica_enabled = false;
float mica_tint[4] = {0.12f, 0.12f, 0.14f, 1.0f};
float mica_tint_strength = 0.35f;

float acrylic_tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
float acrylic_tint_strength = 0.3f;
float acrylic_noise_strength = 0.02f;
float acrylic_light_anchor[2] = {0.5f, 0.5f};
int acrylic_blur_passes = 4;

bool screen_shader_enabled = false;

float refraction_strength = 30.0f;
float refraction_edge_size_px = 18.0f;
float refraction_corner_radius_px = 8.0f;
float refraction_normal_pow = 6.0f;
float refraction_rgb_fringing = 22.0f / 30.0f;
int refraction_texture_repeat_mode = 1;
float refraction_offset = 1.0f;
float refraction_noise_strength = 0.03f;
float refraction_noise_scale = 1.0f;

float shadow_size = 8.0f;
float shadow_offset_x = 0.0f;
float shadow_offset_y = 4.0f;
float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.5f};

effects_state_t effects_state = {0};

static GLuint screen_shader_prog = 0;
static GLint screen_shader_u_tex = -1;
static GLint screen_shader_u_resolution = -1;
static GLint screen_shader_u_time = -1;
static char screen_shader_name_str[256] = "none";
static struct timespec screen_shader_start_time;

static EGLDisplay s_egl_display = EGL_NO_DISPLAY;
static EGLContext s_egl_context = EGL_NO_CONTEXT;
static bool egl_make_current(void) {
  return eglMakeCurrent(s_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_egl_context) == EGL_TRUE;
}

static void egl_unset_current(void) {
  eglMakeCurrent(s_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static const struct wlr_backend_impl capture_backend_impl = {0};

static bool capture_output_test(struct wlr_output *output, const struct wlr_output_state *state) {
	(void)output;
  uint32_t supported = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
    WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
  return (state->committed & ~supported) == 0;
}

static bool capture_output_commit(struct wlr_output *output, const struct wlr_output_state *state) {
  (void)output;
  (void)state;
  return true;
}

static const struct wlr_output_impl capture_output_impl = {
  .test = capture_output_test,
  .commit = capture_output_commit,
};

static size_t capture_output_num = 0;

const struct wlr_drm_format_set *wlr_renderer_get_render_formats(struct wlr_renderer *renderer);

static bool create_capture_output(effects_output_t *ctx, int width, int height) {
  (void)width;
  (void)height;

  // backend
  ctx->capture_backend = calloc(1, sizeof(struct wlr_backend));
  if (!ctx->capture_backend) return false;
  wlr_backend_init(ctx->capture_backend, &capture_backend_impl);
  ctx->capture_backend->buffer_caps = WLR_BUFFER_CAP_DMABUF | WLR_BUFFER_CAP_SHM;

  // output
  ctx->capture_output = calloc(1, sizeof(struct wlr_output));
  if (!ctx->capture_output) {
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_backend = NULL;
    return false;
  }

  struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
  wlr_output_init(ctx->capture_output, ctx->capture_backend,
    &capture_output_impl, loop, NULL);

  char name[64];
  snprintf(name, sizeof(name), "BLUR-%zu", ++capture_output_num);
  wlr_output_set_name(ctx->capture_output, name);

  wlr_output_init_render(ctx->capture_output,  server.allocator, server.renderer);

  // scene output, parked off-screen so surfaces don't become associated with it
  // while we're not actively capturing
  ctx->capture_scene_output = wlr_scene_output_create(server.scene, ctx->capture_output);
  if (!ctx->capture_scene_output) {
    wlr_output_finish(ctx->capture_output);
    free(ctx->capture_output);
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_output = NULL;
    ctx->capture_backend = NULL;
    return false;
  }

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);
  wlr_log(WLR_INFO, "blur: created capture output %s", name);
  return true;
}

static void destroy_capture_output(effects_output_t *ctx) {
  if (ctx->capture_scene_output) {
    wlr_scene_output_destroy(ctx->capture_scene_output);
    ctx->capture_scene_output = NULL;
  }
  if (ctx->capture_output) {
    wlr_output_finish(ctx->capture_output);
    free(ctx->capture_output);
    ctx->capture_output = NULL;
  }
  if (ctx->capture_backend) {
    wlr_backend_finish(ctx->capture_backend);
    free(ctx->capture_backend);
    ctx->capture_backend = NULL;
  }
}

static GLuint compile_shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    GLsizei len = 0;
    glGetShaderInfoLog(s, sizeof(log), &len, log);
    const char *shader_type = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
    wlr_log(WLR_ERROR, "blur: %s shader compilation error: %.*s", shader_type, len, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link_program(const char *frag_src) {
  GLuint vert = compile_shader(GL_VERTEX_SHADER, effect_tex_vert_src);
  GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
  if (!vert || !frag) {
    glDeleteShader(vert);
    glDeleteShader(frag);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glBindAttribLocation(prog, 0, "pos");
  glLinkProgram(prog);
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    GLsizei len = 0;
    glGetProgramInfoLog(prog, sizeof(log), &len, log);
    wlr_log(WLR_ERROR, "blur: program linking error: %.*s", len, log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static bool create_fbo(int w, int h, GLuint *fbo_out, GLuint *tex_out) {
  GLuint tex, fbo;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
     GL_TEXTURE_2D, tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    wlr_log(WLR_ERROR, "blur: FBO incomplete (0x%x)", status);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    return false;
  }
  *fbo_out = fbo;
  *tex_out = tex;
  return true;
}

static void destroy_fbo(GLuint *fbo, GLuint *tex) {
  if (*fbo) {
    glDeleteFramebuffers(1, fbo);
    *fbo = 0;
  }
  if (*tex) {
    glDeleteTextures(1, tex);
    *tex = 0;
  }
}

static void draw_quad(void) {
  glBindBuffer(GL_ARRAY_BUFFER, effects_state.vbo);
  glEnableVertexAttribArray(effects_state.attr_pos);
  glVertexAttribPointer(effects_state.attr_pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(effects_state.attr_pos);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void blur_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int pass_index) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);

  glUseProgram(effects_state.prog_kawase);
  glUniform1i(effects_state.u_kawase.tex, 0);
  glUniform2f(effects_state.u_kawase.halfpixel, 0.5f / (float)w, 0.5f / (float)h);
  glUniform1f(effects_state.u_kawase.offset, blur_radius * (float)(pass_index + 1));
  if (effects_state.u_kawase.noise_strength >= 0)
    glUniform1f(effects_state.u_kawase.noise_strength, blur_noise_strength);
  if (effects_state.u_kawase.vibrancy >= 0)
    glUniform1f(effects_state.u_kawase.vibrancy, blur_vibrancy);
  if (effects_state.u_kawase.vibrancy_darkness >= 0)
    glUniform1f(effects_state.u_kawase.vibrancy_darkness, blur_vibrancy_darkness);
  if (effects_state.u_kawase.brightness >= 0)
    glUniform1f(effects_state.u_kawase.brightness, blur_brightness);
  if (effects_state.u_kawase.contrast >= 0)
    glUniform1f(effects_state.u_kawase.contrast, blur_contrast);

  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void refraction_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int refraction_mode) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);

  glUseProgram(effects_state.prog_refraction);
  glUniform1i(effects_state.u_refraction.tex, 0);

  if (effects_state.u_refraction.offset >= 0)
    glUniform1f(effects_state.u_refraction.offset, refraction_offset);
  if (effects_state.u_refraction.halfpixel >= 0)
    glUniform2f(effects_state.u_refraction.halfpixel, 0.5f / (float)w, 0.5f / (float)h);

  if (effects_state.u_refraction.refraction_rect_size >= 0)
    glUniform2f(effects_state.u_refraction.refraction_rect_size, (float)w, (float)h);

  float max_edge = 0.5f * (float)((w < h) ? w : h);
  float edge = refraction_edge_size_px;
  if (edge > max_edge) edge = max_edge;
  if (edge < 0.0f) edge = 0.0f;
  if (effects_state.u_refraction.refraction_edge_size_pixels >= 0)
    glUniform1f(effects_state.u_refraction.refraction_edge_size_pixels, edge);

  float max_corner = 0.5f * (float)((w < h) ? w : h);
  float corner = refraction_corner_radius_px;
  if (corner > max_corner) corner = max_corner;
  if (corner < 0.0f) corner = 0.0f;
  if (effects_state.u_refraction.refraction_corner_radius_pixels >= 0)
    glUniform1f(effects_state.u_refraction.refraction_corner_radius_pixels, corner);

  float strength_norm = refraction_strength / 30.0f;
  if (strength_norm < 0.0f) strength_norm = 0.0f;
  if (strength_norm > 1.0f) strength_norm = 1.0f;
  if (effects_state.u_refraction.refraction_strength >= 0)
    glUniform1f(effects_state.u_refraction.refraction_strength, strength_norm);

  if (effects_state.u_refraction.refraction_normal_pow >= 0)
    glUniform1f(effects_state.u_refraction.refraction_normal_pow, refraction_normal_pow);

  float fringing = refraction_rgb_fringing;
  if (fringing < 0.0f) fringing = 0.0f;
  if (fringing > 1.0f) fringing = 1.0f;
  if (effects_state.u_refraction.refraction_RGB_fringing >= 0)
    glUniform1f(effects_state.u_refraction.refraction_RGB_fringing, fringing);

  if (effects_state.u_refraction.refraction_texture_repeat_mode >= 0)
    glUniform1i(effects_state.u_refraction.refraction_texture_repeat_mode, refraction_texture_repeat_mode);

  if (effects_state.u_refraction.refraction_mode >= 0)
    glUniform1i(effects_state.u_refraction.refraction_mode, refraction_mode);

  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void box_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex,
    GLuint pong_fbo, int w, int h) {
  // H: src_tex -> ping_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, ping_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUseProgram(effects_state.prog_box_h);
  glUniform1i(effects_state.u_box.tex, 0);
  glUniform2f(effects_state.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(effects_state.u_box.radius, blur_radius);
  if (effects_state.u_box.vibrancy >= 0)
    glUniform1f(effects_state.u_box.vibrancy, blur_vibrancy);
  if (effects_state.u_box.vibrancy_darkness >= 0)
    glUniform1f(effects_state.u_box.vibrancy_darkness, blur_vibrancy_darkness);
  if (effects_state.u_box.brightness >= 0)
    glUniform1f(effects_state.u_box.brightness, blur_brightness);
  if (effects_state.u_box.contrast >= 0)
    glUniform1f(effects_state.u_box.contrast, blur_contrast);
  draw_quad();

  // V: ping_tex -> pong_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(effects_state.prog_box_v);
  glUniform1i(effects_state.u_box.tex, 0);
  glUniform2f(effects_state.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(effects_state.u_box.radius, blur_radius);
  if (effects_state.u_box.vibrancy >= 0)
    glUniform1f(effects_state.u_box.vibrancy, blur_vibrancy);
  if (effects_state.u_box.vibrancy_darkness >= 0)
    glUniform1f(effects_state.u_box.vibrancy_darkness, blur_vibrancy_darkness);
  if (effects_state.u_box.brightness >= 0)
    glUniform1f(effects_state.u_box.brightness, blur_brightness);
  if (effects_state.u_box.contrast >= 0)
    glUniform1f(effects_state.u_box.contrast, blur_contrast);
  draw_quad();

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void gaussian_pass(GLuint src_tex, GLuint ping_fbo, GLuint ping_tex,
    GLuint pong_fbo, int w, int h) {
  // src_tex -> ping_fbo (horizontal pass)
  glBindFramebuffer(GL_FRAMEBUFFER, ping_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);
  glUseProgram(effects_state.prog_gauss_h);
  glUniform1i(effects_state.u_gauss.tex, 0);
  glUniform2f(effects_state.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(effects_state.u_gauss.radius, blur_radius);
  if (effects_state.u_gauss.vibrancy >= 0)
    glUniform1f(effects_state.u_gauss.vibrancy, blur_vibrancy);
  if (effects_state.u_gauss.vibrancy_darkness >= 0)
    glUniform1f(effects_state.u_gauss.vibrancy_darkness, blur_vibrancy_darkness);
  if (effects_state.u_gauss.brightness >= 0)
    glUniform1f(effects_state.u_gauss.brightness, blur_brightness);
  if (effects_state.u_gauss.contrast >= 0)
    glUniform1f(effects_state.u_gauss.contrast, blur_contrast);
  draw_quad();

  // ping_tex -> pong_fbo (vertical pass)
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(effects_state.prog_gauss_v);
  glUniform1i(effects_state.u_gauss.tex, 0);
  glUniform2f(effects_state.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(effects_state.u_gauss.radius, blur_radius);
  if (effects_state.u_gauss.vibrancy >= 0)
    glUniform1f(effects_state.u_gauss.vibrancy, blur_vibrancy);
  if (effects_state.u_gauss.vibrancy_darkness >= 0)
    glUniform1f(effects_state.u_gauss.vibrancy_darkness, blur_vibrancy_darkness);
  if (effects_state.u_gauss.brightness >= 0)
    glUniform1f(effects_state.u_gauss.brightness, blur_brightness);
  if (effects_state.u_gauss.contrast >= 0)
    glUniform1f(effects_state.u_gauss.contrast, blur_contrast);
  draw_quad();

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLuint apply_blur(effects_output_t *ctx, GLuint src_tex, int w, int h) {
  if (blur_passes <= 0 || blur_algorithm == BLUR_ALGORITHM_NONE)
    return src_tex;

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);

  int ping = 0;
  GLuint current = src_tex;

  for (int i = 0; i < blur_passes; i++) {
    int pong = ping ^ 1;
    if (blur_algorithm == BLUR_ALGORITHM_GAUSSIAN) {
      gaussian_pass(current, ctx->fbo[ping], ctx->tex[ping],
        ctx->fbo[pong], w, h);
      current = ctx->tex[pong];
      ping = pong;
    } else if (blur_algorithm == BLUR_ALGORITHM_BOX) {
      box_pass(current, ctx->fbo[ping], ctx->tex[ping],
        ctx->fbo[pong], w, h);
      current = ctx->tex[pong];
      ping = pong;
    } else if (blur_algorithm == BLUR_ALGORITHM_REFRACTION || blur_algorithm == BLUR_ALGORITHM_LENS_REFRACTION) {
      int mode = (blur_algorithm == BLUR_ALGORITHM_LENS_REFRACTION) ? 1 : 0;
      refraction_pass(current, ctx->fbo[pong], w, h, mode);
      current = ctx->tex[pong];
      ping = pong;
    } else {
      blur_pass(current, ctx->fbo[ping], w, h, i);
      current = ctx->tex[ping];
      ping ^= 1;
    }
  }
  return current;
}

bool effects_init(void) {
  effects_state = (effects_state_t){0};

  if (!wlr_renderer_is_gles2(server.renderer)) {
    wlr_log(WLR_INFO, "blur: renderer is not GLES2 – blur disabled");
    return false;
  }

  struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server.renderer);
  s_egl_display = wlr_egl_get_display(egl);
  s_egl_context = wlr_egl_get_context(egl);

  {
    const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(server.renderer);
    s_render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
    if (!s_render_fmt) s_render_fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
    if (!s_render_fmt) {
      wlr_log(WLR_ERROR, "Failed to find a suitable DRM render format");
      return false;
    }
  }

  if (!egl_make_current()) {
    wlr_log(WLR_ERROR, "blur: failed to make EGL context current");
    return false;
  }

  effects_state.prog_kawase = link_program(blur_kawase_frag_src);
  effects_state.prog_gauss_h = link_program(blur_gauss_h_frag_src);
  effects_state.prog_gauss_v = link_program(blur_gauss_v_frag_src);
  effects_state.prog_box_h = link_program(blur_box_h_frag_src);
  effects_state.prog_box_v = link_program(blur_box_v_frag_src);
  effects_state.prog_blit = link_program(blit_frag_src);
  effects_state.prog_mica_tint = link_program(blur_mica_frag_src);
  effects_state.prog_acrylic_tint = link_program(blur_acrylic_frag_src);
  effects_state.prog_refraction = link_program(blur_refraction_frag_src);
  effects_state.prog_ext_blit = link_program(ext_blit_frag_src);
  effects_state.prog_border = link_program(border_frag_src);
  effects_state.prog_corner_mask = link_program(border_corner_mask_frag_src);
  effects_state.prog_shadow = link_program(shadow_frag_src);

  if (!effects_state.prog_kawase || !effects_state.prog_gauss_h || !effects_state.prog_gauss_v ||
      !effects_state.prog_box_h || !effects_state.prog_box_v || !effects_state.prog_blit ||
      !effects_state.prog_mica_tint || !effects_state.prog_acrylic_tint || !effects_state.prog_refraction) {
    wlr_log(WLR_ERROR, "blur: one or more required shaders failed to compile");
    egl_unset_current();
    return false;
  }

  effects_state.u_kawase.tex = glGetUniformLocation(effects_state.prog_kawase, "tex");
  effects_state.u_kawase.halfpixel = glGetUniformLocation(effects_state.prog_kawase, "halfpixel");
  effects_state.u_kawase.offset = glGetUniformLocation(effects_state.prog_kawase, "offset");
  effects_state.u_kawase.noise_strength = glGetUniformLocation(effects_state.prog_kawase, "noise_strength");
  effects_state.u_kawase.vibrancy = glGetUniformLocation(effects_state.prog_kawase, "vibrancy");
  effects_state.u_kawase.vibrancy_darkness = glGetUniformLocation(effects_state.prog_kawase, "vibrancy_darkness");
  effects_state.u_kawase.brightness = glGetUniformLocation(effects_state.prog_kawase, "brightness");
  effects_state.u_kawase.contrast = glGetUniformLocation(effects_state.prog_kawase, "contrast");

  effects_state.u_gauss.tex = glGetUniformLocation(effects_state.prog_gauss_h, "tex");
  effects_state.u_gauss.texel_size = glGetUniformLocation(effects_state.prog_gauss_h, "texel_size");
  effects_state.u_gauss.radius = glGetUniformLocation(effects_state.prog_gauss_h, "radius");
  effects_state.u_gauss.vibrancy = glGetUniformLocation(effects_state.prog_gauss_h, "vibrancy");
  effects_state.u_gauss.vibrancy_darkness = glGetUniformLocation(effects_state.prog_gauss_h, "vibrancy_darkness");
  effects_state.u_gauss.brightness = glGetUniformLocation(effects_state.prog_gauss_h, "brightness");
  effects_state.u_gauss.contrast = glGetUniformLocation(effects_state.prog_gauss_h, "contrast");

  effects_state.u_box.tex = glGetUniformLocation(effects_state.prog_box_h, "tex");
  effects_state.u_box.texel_size = glGetUniformLocation(effects_state.prog_box_h, "texel_size");
  effects_state.u_box.radius = glGetUniformLocation(effects_state.prog_box_h, "radius");
  effects_state.u_box.vibrancy = glGetUniformLocation(effects_state.prog_box_h, "vibrancy");
  effects_state.u_box.vibrancy_darkness = glGetUniformLocation(effects_state.prog_box_h, "vibrancy_darkness");
  effects_state.u_box.brightness = glGetUniformLocation(effects_state.prog_box_h, "brightness");
  effects_state.u_box.contrast = glGetUniformLocation(effects_state.prog_box_h, "contrast");

  effects_state.u_blit.tex = glGetUniformLocation(effects_state.prog_blit, "tex");

  if (effects_state.prog_ext_blit)
    effects_state.u_ext_blit.tex = glGetUniformLocation(effects_state.prog_ext_blit, "tex");

  effects_state.u_mica.tex = glGetUniformLocation(effects_state.prog_mica_tint, "tex");
  effects_state.u_mica.tint = glGetUniformLocation(effects_state.prog_mica_tint, "tint");
  effects_state.u_mica.tint_strength = glGetUniformLocation(effects_state.prog_mica_tint, "tint_strength");

  effects_state.u_acrylic.tex = glGetUniformLocation(effects_state.prog_acrylic_tint, "tex");
  effects_state.u_acrylic.tint = glGetUniformLocation(effects_state.prog_acrylic_tint, "tint");
  effects_state.u_acrylic.tint_strength = glGetUniformLocation(effects_state.prog_acrylic_tint, "tint_strength");
  effects_state.u_acrylic.noise_strength = glGetUniformLocation(effects_state.prog_acrylic_tint, "noise_strength");
  effects_state.u_acrylic.resolution = glGetUniformLocation(effects_state.prog_acrylic_tint, "resolution");
  effects_state.u_acrylic.light_anchor = glGetUniformLocation(effects_state.prog_acrylic_tint, "light_anchor");

  effects_state.u_refraction.tex = glGetUniformLocation(effects_state.prog_refraction, "tex");
  effects_state.u_refraction.offset = glGetUniformLocation(effects_state.prog_refraction, "offset");
  effects_state.u_refraction.halfpixel = glGetUniformLocation(effects_state.prog_refraction, "halfpixel");
  effects_state.u_refraction.refraction_rect_size = glGetUniformLocation(effects_state.prog_refraction, "refraction_rect_size");
  effects_state.u_refraction.refraction_edge_size_pixels = glGetUniformLocation(effects_state.prog_refraction, "refraction_edge_size_pixels");
  effects_state.u_refraction.refraction_corner_radius_pixels = glGetUniformLocation(effects_state.prog_refraction, "refraction_corner_radius_pixels");
  effects_state.u_refraction.refraction_strength = glGetUniformLocation(effects_state.prog_refraction, "refraction_strength");
  effects_state.u_refraction.refraction_normal_pow = glGetUniformLocation(effects_state.prog_refraction, "refraction_normal_pow");
  effects_state.u_refraction.refraction_RGB_fringing = glGetUniformLocation(effects_state.prog_refraction, "refraction_RGB_fringing");
  effects_state.u_refraction.refraction_texture_repeat_mode = glGetUniformLocation(effects_state.prog_refraction, "refraction_texture_repeat_mode");
  effects_state.u_refraction.refraction_mode = glGetUniformLocation(effects_state.prog_refraction, "refraction_mode");

  if (effects_state.prog_border) {
    effects_state.u_border.resolution = glGetUniformLocation(effects_state.prog_border, "resolution");
    effects_state.u_border.border_radius = glGetUniformLocation(effects_state.prog_border, "border_radius");
    effects_state.u_border.border_width_px = glGetUniformLocation(effects_state.prog_border, "border_width_px");
    effects_state.u_border.border_color = glGetUniformLocation(effects_state.prog_border, "border_color");
    effects_state.u_border.gradient_colors  = glGetUniformLocation(effects_state.prog_border, "gradient_colors");
    effects_state.u_border.gradient_count = glGetUniformLocation(effects_state.prog_border, "gradient_count");
    effects_state.u_border.gradient_angle = glGetUniformLocation(effects_state.prog_border, "gradient_angle");
    effects_state.u_border.gradient2_colors = glGetUniformLocation(effects_state.prog_border, "gradient2_colors");
    effects_state.u_border.gradient2_count = glGetUniformLocation(effects_state.prog_border, "gradient2_count");
    effects_state.u_border.gradient2_angle = glGetUniformLocation(effects_state.prog_border, "gradient2_angle");
    effects_state.u_border.gradient_lerp = glGetUniformLocation(effects_state.prog_border, "gradient_lerp");
  }
  if (effects_state.prog_corner_mask) {
    effects_state.u_corner_mask.tex = glGetUniformLocation(effects_state.prog_corner_mask, "tex");
    effects_state.u_corner_mask.win_pos_uv = glGetUniformLocation(effects_state.prog_corner_mask, "win_pos_uv");
    effects_state.u_corner_mask.win_size_uv = glGetUniformLocation(effects_state.prog_corner_mask, "win_size_uv");
    effects_state.u_corner_mask.win_size_px = glGetUniformLocation(effects_state.prog_corner_mask, "win_size_px");
    effects_state.u_corner_mask.border_radius_px = glGetUniformLocation(effects_state.prog_corner_mask, "border_radius_px");
  }
  if (effects_state.prog_shadow) {
    effects_state.u_shadow.resolution = glGetUniformLocation(effects_state.prog_shadow, "resolution");
    effects_state.u_shadow.shadow_size = glGetUniformLocation(effects_state.prog_shadow, "shadow_size");
    effects_state.u_shadow.shadow_color = glGetUniformLocation(effects_state.prog_shadow, "shadow_color");
    effects_state.u_shadow.border_radius = glGetUniformLocation(effects_state.prog_shadow, "border_radius");
    effects_state.u_shadow.inner_size = glGetUniformLocation(effects_state.prog_shadow, "inner_size");
    effects_state.u_shadow.hole_pos = glGetUniformLocation(effects_state.prog_shadow, "hole_pos");
    effects_state.u_shadow.hole_size = glGetUniformLocation(effects_state.prog_shadow, "hole_size");
  }

  effects_state.attr_pos = 0;

  static const float quad[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,
    -1.0f,  1.0f,   1.0f,  1.0f,
  };
  glGenBuffers(1, &effects_state.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, effects_state.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  egl_unset_current();
  effects_state.available = true;
  wlr_log(WLR_INFO, "blur: initialised (GLES2)");
  return true;
}

void effects_fini(void) {
  if (!effects_state.available) return;
  egl_make_current();
  glDeleteProgram(effects_state.prog_kawase);
  glDeleteProgram(effects_state.prog_gauss_h);
  glDeleteProgram(effects_state.prog_gauss_v);
  glDeleteProgram(effects_state.prog_box_h);
  glDeleteProgram(effects_state.prog_box_v);
  glDeleteProgram(effects_state.prog_blit);
  glDeleteProgram(effects_state.prog_mica_tint);
  glDeleteProgram(effects_state.prog_acrylic_tint);
  glDeleteProgram(effects_state.prog_refraction);
  if (effects_state.prog_ext_blit)
    glDeleteProgram(effects_state.prog_ext_blit);
  if (effects_state.prog_border)
    glDeleteProgram(effects_state.prog_border);
  if (effects_state.prog_corner_mask)
    glDeleteProgram(effects_state.prog_corner_mask);
  if (effects_state.prog_shadow)
    glDeleteProgram(effects_state.prog_shadow);
  if (screen_shader_prog)
    glDeleteProgram(screen_shader_prog);
  glDeleteBuffers(1, &effects_state.vbo);
  egl_unset_current();
  screen_shader_prog = 0;
  effects_state = (effects_state_t){0};
}

effects_output_t *effects_output_init(int width, int height) {
  if (!effects_state.available) return NULL;

  effects_output_t *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;
  ctx->width  = width;
  ctx->height = height;
  int ds = blur_downsample > 0 ? blur_downsample : 1;
  ctx->blur_w = (width  / ds) > 0 ? (width  / ds) : 1;
  ctx->blur_h = (height / ds) > 0 ? (height / ds) : 1;

  egl_make_current();
  bool ok = create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[0], &ctx->tex[0]) &&
    create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[1], &ctx->tex[1]);
  if (!create_fbo(width, height, &ctx->screen_fbo, &ctx->screen_tex))
    wlr_log(WLR_ERROR, "blur: screen shader FBO creation failed (non-fatal)");

  glGenTextures(1, &ctx->staging_tex);
  glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  egl_unset_current();

  if (!ok) {
    free(ctx);
    return NULL;
  }

  if (!create_capture_output(ctx, width, height)) {
    wlr_log(WLR_ERROR, "blur: failed to create capture output");
    egl_make_current();
    destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
    destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
    destroy_fbo(&ctx->screen_fbo, &ctx->screen_tex);
    glDeleteTextures(1, &ctx->staging_tex);
    ctx->staging_tex = 0;
    egl_unset_current();
    free(ctx);
    return NULL;
  }

  if (server.shader_tree) {
    ctx->screen_shader_node = wlr_scene_buffer_create(server.shader_tree, NULL);
    if (ctx->screen_shader_node)
      wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
  }

  ctx->mica_dirty = true;
  return ctx;
}

void effects_output_fini(effects_output_t *ctx) {
  if (!ctx) return;
  if (effects_state.available) {
    egl_make_current();
    destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
    destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
    destroy_fbo(&ctx->screen_fbo, &ctx->screen_tex);
    if (ctx->staging_tex) {
      glDeleteTextures(1, &ctx->staging_tex);
      ctx->staging_tex = 0;
    }
    egl_unset_current();
  }
  if (ctx->mica_buf) {
    wlr_buffer_unlock(ctx->mica_buf);
    ctx->mica_buf = NULL;
    ctx->mica_buf_fbo = 0;
  }
  if (ctx->screen_shader_buf) {
    wlr_buffer_unlock(ctx->screen_shader_buf);
    ctx->screen_shader_buf = NULL;
    ctx->screen_shader_buf_fbo = 0;
  }
  if (ctx->screen_shader_node) {
    wlr_scene_node_destroy(&ctx->screen_shader_node->node);
    ctx->screen_shader_node = NULL;
  }
  destroy_capture_output(ctx);
  free(ctx);
}

void effects_output_resize(effects_output_t *ctx, int width, int height,
    output_t *output) {
  if (!ctx || !effects_state.available) return;
  int ds = blur_downsample > 0 ? blur_downsample : 1;
  int new_bw = (width  / ds) > 0 ? (width  / ds) : 1;
  int new_bh = (height / ds) > 0 ? (height / ds) : 1;
  if (ctx->width == width && ctx->height == height &&
      ctx->blur_w == new_bw && ctx->blur_h == new_bh) return;

  egl_make_current();
  destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
  destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
  destroy_fbo(&ctx->screen_fbo, &ctx->screen_tex);
  if (ctx->staging_tex) {
    glDeleteTextures(1, &ctx->staging_tex);
    ctx->staging_tex = 0;
  }
  ctx->width  = width;
  ctx->height = height;
  ctx->blur_w = new_bw;
  ctx->blur_h = new_bh;
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[0], &ctx->tex[0]);
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[1], &ctx->tex[1]);
  if (!create_fbo(width, height, &ctx->screen_fbo, &ctx->screen_tex))
    wlr_log(WLR_ERROR, "blur: screen shader FBO resize failed (non-fatal)");
  glGenTextures(1, &ctx->staging_tex);
  glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  egl_unset_current();

  if (ctx->mica_buf) {
    wlr_buffer_unlock(ctx->mica_buf);
    ctx->mica_buf = NULL;
    ctx->mica_buf_fbo = 0;
  }
  if (ctx->screen_shader_buf) {
    wlr_buffer_unlock(ctx->screen_shader_buf);
    ctx->screen_shader_buf = NULL;
    ctx->screen_shader_buf_fbo = 0;
  }

  // free per-toplevel blur/acrylic buffers since output dimensions changed
  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (tl->blur) {
      if (tl->blur->blur_buf) {
        wlr_buffer_unlock(tl->blur->blur_buf);
        tl->blur->blur_buf = NULL;
        tl->blur->blur_buf_fbo = 0;
      }
      if (tl->blur->acrylic_buf) {
        wlr_buffer_unlock(tl->blur->acrylic_buf);
        tl->blur->acrylic_buf = NULL;
        tl->blur->acrylic_buf_fbo = 0;
      }
    }
    if (tl->rounded) {
      if (tl->rounded->corner_mask_buf) {
        wlr_buffer_unlock(tl->rounded->corner_mask_buf);
        tl->rounded->corner_mask_buf = NULL;
        tl->rounded->corner_mask_buf_fbo = 0;
      }
      if (tl->rounded->border_shader_buf) {
        wlr_buffer_unlock(tl->rounded->border_shader_buf);
        tl->rounded->border_shader_buf = NULL;
        tl->rounded->border_shader_buf_fbo = 0;
        tl->rounded->border_shader_buf_w = 0;
        tl->rounded->border_shader_buf_h = 0;
      }
      tl->rounded->border_dirty = true;
    }
  }

  // free per-layer-surface blur buffers since output dimensions changed
  if (output) {
    for (int i = 0; i < 4; i++) {
      layer_surface_t *ls;
      wl_list_for_each(ls, &output->layers[i], link) {
        if (ls->blur_buf) {
          wlr_buffer_unlock(ls->blur_buf);
          ls->blur_buf = NULL;
          ls->blur_buf_fbo = 0;
        }
      }
    }
  }

  ctx->mica_dirty = true;
}

void effects_invalidate_mica(effects_output_t *ctx) {
  if (ctx) ctx->mica_dirty = true;
}

static bool compute_src_box(output_t *output, const struct wlr_box *r,
    struct wlr_fbox *src_out, int *dw_out, int *dh_out) {
  float bw = (float)output->width;
  float bh = (float)output->height;
  float sx = (float)(r->x - output->lx);
  float sy = (float)(r->y - output->ly);
  float sw = (float)r->width;
  float sh = (float)r->height;

  if (sx < 0.0f) {
	  sw += sx;
	  sx = 0.0f;
  }
  if (sy < 0.0f) {
  	sh += sy;
   	sy = 0.0f;
  }
  if (sx >= bw || sy >= bh || sw <= 0.0f || sh <= 0.0f) return false;

  if (sx + sw > bw) sw = bw - sx;
  if (sy + sh > bh) sh = bh - sy;
  if (sw <= 0.0f || sh <= 0.0f) return false;

  *src_out = (struct wlr_fbox){
    .x = sx,
    .y = sy,
    .width = sw,
    .height = sh
  };
  *dw_out = (int)sw;
  *dh_out = (int)sh;
  return true;
}

// read back the capture output buffer into ctx->tex[1] and return its FBO attachment
static GLuint capture_readback(effects_output_t *ctx, GLuint capture_fbo, int w, int h) {
  GLuint result = 0;
  if (!capture_fbo) {
    wlr_log(WLR_INFO, "blur: capture_readback: no FBO");
    return 0;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
  GLint attach_type = 0, attach_name = 0;
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attach_type);
  if (attach_type == GL_TEXTURE) {
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attach_name);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  if (attach_type == GL_TEXTURE && attach_name > 0 && effects_state.prog_ext_blit) {
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
    glViewport(0, 0, ctx->blur_w, ctx->blur_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
    glUseProgram(effects_state.prog_ext_blit);
    glUniform1i(effects_state.u_ext_blit.tex, 0);
    draw_quad();
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    result = ctx->tex[1];
  } else if (attach_type == GL_TEXTURE && attach_name > 0) {
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
    glViewport(0, 0, ctx->blur_w, ctx->blur_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)attach_name);
    glUseProgram(effects_state.prog_blit);
    glUniform1i(effects_state.u_blit.tex, 0);
    draw_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    result = ctx->tex[1];
  } else if (attach_type == GL_RENDERBUFFER) {
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
    glViewport(0, 0, ctx->blur_w, ctx->blur_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
    glUseProgram(effects_state.prog_blit);
    glUniform1i(effects_state.u_blit.tex, 0);
    draw_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    result = ctx->tex[1];
  }
  return result;
}

// capture scene with all blur/mica/acrylic toplevel scene trees hidden
static GLuint capture_bg_shared(output_t *output, effects_output_t *ctx) {
  int w = output->width, h = output->height;
  if (!ctx->capture_output || !ctx->capture_scene_output) return 0;
  if (w <= 0 || h <= 0) return 0;

  wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

  wlr_scene_node_set_enabled(&server.top_tree->node, false);
  wlr_scene_node_set_enabled(&server.full_tree->node, false);
  wlr_scene_node_set_enabled(&server.over_tree->node, false);
  wlr_scene_node_set_enabled(&server.lock_tree->node, false);

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur) continue;
    if ((tl->blur->blur_node || tl->blur->mica_node || tl->blur->acrylic_node) &&
        tl->scene_tree && tl->scene_tree->node.enabled) {
      wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
      tl->blur->blur_scene_hidden = true;
    }
  }

  wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

  struct wlr_output_state cap_state;
  wlr_output_state_init(&cap_state);
  wlr_output_state_set_enabled(&cap_state, true);
  wlr_output_state_set_custom_mode(&cap_state, w, h, 0);

  egl_unset_current();
  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);
  egl_make_current();

  wl_list_for_each(tl, &server.toplevels, link)
    if (tl->blur && tl->blur->blur_scene_hidden) {
      wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
      tl->blur->blur_scene_hidden = false;
    }

  wlr_scene_node_set_enabled(&server.top_tree->node, true);
  wlr_scene_node_set_enabled(&server.full_tree->node, true);
  wlr_scene_node_set_enabled(&server.over_tree->node, true);
  wlr_scene_node_set_enabled(&server.lock_tree->node, true);

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

  if (!ok || !cap_state.buffer) {
    wlr_output_state_finish(&cap_state);
    return 0;
  }

  GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
  GLuint result = capture_readback(ctx, capture_fbo, w, h);
  wlr_output_state_finish(&cap_state);
  return result;
}

static GLuint capture_bg_to_tex1(output_t *output, effects_output_t *ctx,
    bool mica_only, struct wlr_scene_node *hide_node, bool *hide_flag) {
  int w = output->width, h = output->height;

  if (!ctx->capture_output || !ctx->capture_scene_output) return 0;

  wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

  if (w <= 0 || h <= 0) return 0;

  wlr_scene_node_set_enabled(&server.top_tree->node, false);
  wlr_scene_node_set_enabled(&server.full_tree->node, false);
  wlr_scene_node_set_enabled(&server.over_tree->node, false);
  wlr_scene_node_set_enabled(&server.lock_tree->node, false);

  if (mica_only) {
    wlr_scene_node_set_enabled(&server.tile_tree->node, false);
    wlr_scene_node_set_enabled(&server.float_tree->node, false);
  }

  toplevel_t *tl;
  if (hide_node) {
    *hide_flag = false;
    if (hide_node->enabled) {
      wlr_scene_node_set_enabled(hide_node, false);
      *hide_flag = true;
    }
  } else {
    // hide everything with blur/mica (for mica capture)
    wl_list_for_each(tl, &server.toplevels, link) {
      if (!tl->blur) continue;
      tl->blur->blur_scene_hidden = false;
      if ((tl->blur->blur_node || tl->blur->mica_node) && tl->scene_tree &&
          tl->scene_tree->node.enabled) {
        wlr_scene_node_set_enabled(&tl->scene_tree->node, false);
        tl->blur->blur_scene_hidden = true;
      }
    }
  }

  wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

  struct wlr_output_state cap_state;
  wlr_output_state_init(&cap_state);
  wlr_output_state_set_enabled(&cap_state, true);
  wlr_output_state_set_custom_mode(&cap_state, w, h, 0);

  egl_unset_current();
  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);

  egl_make_current();

  if (hide_node) {
    if (*hide_flag)
      wlr_scene_node_set_enabled(hide_node, true);
  } else {
    wl_list_for_each(tl, &server.toplevels, link)
      if (tl->blur && tl->blur->blur_scene_hidden)
        wlr_scene_node_set_enabled(&tl->scene_tree->node, true);
  }

  wlr_scene_node_set_enabled(&server.top_tree->node, true);
  wlr_scene_node_set_enabled(&server.full_tree->node, true);
  wlr_scene_node_set_enabled(&server.over_tree->node, true);
  wlr_scene_node_set_enabled(&server.lock_tree->node, true);
  if (mica_only) {
    wlr_scene_node_set_enabled(&server.tile_tree->node, true);
    wlr_scene_node_set_enabled(&server.float_tree->node, true);
  }

  // prevent output overlap by parking offscreen
  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

  if (!ok || !cap_state.buffer) {
    wlr_log(WLR_INFO, "capture_bg_to_tex1: no buffer from build_state");
    wlr_output_state_finish(&cap_state);
    return 0;
  }

  GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
  GLuint result = capture_readback(ctx, capture_fbo, w, h);

  wlr_output_state_finish(&cap_state);

  return result;
}

// capture background once with all blur-layer surfaces and corner-mask toplevels hidden
static GLuint capture_bg_combined(output_t *output, effects_output_t *ctx) {
  int w = output->width, h = output->height;
  if (!ctx->capture_output || !ctx->capture_scene_output) return 0;
  if (w <= 0 || h <= 0) return 0;

  wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

  wlr_scene_node_set_enabled(&server.top_tree->node, false);
  wlr_scene_node_set_enabled(&server.full_tree->node, false);
  wlr_scene_node_set_enabled(&server.over_tree->node, false);
  wlr_scene_node_set_enabled(&server.lock_tree->node, false);

  // hide all blur-layer surfaces
  for (int i = 0; i < 4; i++) {
    layer_surface_t *ls;
    wl_list_for_each(ls, &output->layers[i], link) {
      if (ls->blur_node && ls->mapped) {
        ls->blur_scene_hidden = false;
        if (ls->scene_tree->node.enabled) {
          wlr_scene_node_set_enabled(&ls->scene_tree->node, false);
          ls->blur_scene_hidden = true;
        }
      }
    }
  }

  wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

  struct wlr_output_state cap_state;
  wlr_output_state_init(&cap_state);
  wlr_output_state_set_enabled(&cap_state, true);
  wlr_output_state_set_custom_mode(&cap_state, w, h, 0);

  egl_unset_current();
  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);

  egl_make_current();

  // restore blur-layer surfaces
  for (int i = 0; i < 4; i++) {
    layer_surface_t *ls;
    wl_list_for_each(ls, &output->layers[i], link)
      if (ls->blur_scene_hidden)
        wlr_scene_node_set_enabled(&ls->scene_tree->node, true);
  }

  wlr_scene_node_set_enabled(&server.top_tree->node, true);
  wlr_scene_node_set_enabled(&server.full_tree->node, true);
  wlr_scene_node_set_enabled(&server.over_tree->node, true);
  wlr_scene_node_set_enabled(&server.lock_tree->node, true);

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

  if (!ok || !cap_state.buffer) {
    wlr_log(WLR_INFO, "capture_bg_combined: no buffer from build_state");
    wlr_output_state_finish(&cap_state);
    return 0;
  }

  GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
  GLuint result = capture_readback(ctx, capture_fbo, w, h);

  wlr_output_state_finish(&cap_state);
  return result;
}

// ensure buf/fbo are allocated; returns fbo or 0 on failure
static GLuint ensure_output_buf(struct wlr_buffer **buf_out, GLuint *fbo_out,
    int w, int h) {
  if (*buf_out) return *fbo_out;
  if (!s_render_fmt) return 0;

  struct wlr_buffer *buf = wlr_allocator_create_buffer(server.allocator, w, h, s_render_fmt);
  if (!buf) return 0;

  GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, buf);
  if (!fbo) {
    wlr_buffer_drop(buf);
    return 0;
  }

  wlr_buffer_lock(buf);
  wlr_buffer_drop(buf);
  *buf_out = buf;
  *fbo_out = fbo;
  return fbo;
}

static GLuint ensure_sized_buf(struct wlr_buffer **buf_out, GLuint *fbo_out,
    int *w_stored, int *h_stored, int w, int h) {
  if (*buf_out && *w_stored == w && *h_stored == h)
    return *fbo_out;

  if (*buf_out) {
    wlr_buffer_unlock(*buf_out);
    *buf_out = NULL;
    *fbo_out = 0;
  }

  if (!s_render_fmt) return 0;

  struct wlr_buffer *buf = wlr_allocator_create_buffer(server.allocator, w, h, s_render_fmt);
  if (!buf) return 0;

  GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, buf);
  if (!fbo) {
    wlr_buffer_drop(buf);
    return 0;
  }

  wlr_buffer_lock(buf);
  wlr_buffer_drop(buf);
  *buf_out = buf;
  *fbo_out = fbo;
  *w_stored = w;
  *h_stored = h;
  return fbo;
}

static struct wlr_box get_client_rect(toplevel_t *tl);

static bool rebuild_live_blur(output_t *output,
    pixman_region32_t *damage, GLuint shared_blurred) {
  effects_output_t *ctx = output->effects;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->blur_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    // skip if toplevel already has a valid blur and the damage doesn't overlap it
    if (tl->blur->blur_buf && damage && !pixman_region32_empty(damage)) {
      struct wlr_box r = get_client_rect(tl);
      pixman_region32_t tl_rgn, intersection;
      pixman_region32_init_rect(&tl_rgn, r.x - output->lx, r.y - output->ly, r.width, r.height);
      pixman_region32_init(&intersection);
      pixman_region32_intersect(&intersection, damage, &tl_rgn);
      bool overlaps = !pixman_region32_empty(&intersection);
      pixman_region32_fini(&intersection);
      pixman_region32_fini(&tl_rgn);
      if (!overlaps) continue;
    }

    GLuint blurred;
    if (shared_blurred) {
      blurred = shared_blurred;
    } else {
      GLuint src = capture_bg_to_tex1(output, ctx, false,
        &tl->scene_tree->node, &tl->blur->blur_scene_hidden);
      if (!src) continue;
      blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);
    }

    GLuint dest_fbo = ensure_output_buf(&tl->blur->blur_buf, &tl->blur->blur_buf_fbo, w, h);
    if (!dest_fbo)
      continue;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred);
    glUseProgram(effects_state.prog_blit);
    glUniform1i(effects_state.u_blit.tex, 0);
    draw_quad();

    client_t *c = tl->node->client;
    if (c && c->border_radius > 0.0f && c->state != STATE_FULLSCREEN && effects_state.prog_corner_mask) {
      struct wlr_box content_r = get_client_rect(tl);
      float ow = (float)w, oh = (float)h;
      float win_u  = (float)(content_r.x - output->lx) / ow;
      float win_v  = 1.0f - (float)(content_r.y - output->ly + content_r.height) / oh;
      float win_sw = (float)content_r.width  / ow;
      float win_sh = (float)content_r.height / oh;
      int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;
      float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

      glEnable(GL_BLEND);
      glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(effects_state.prog_corner_mask);
      glUniform1i(effects_state.u_corner_mask.tex, 0);
      glUniform2f(effects_state.u_corner_mask.win_pos_uv, win_u, win_v);
      glUniform2f(effects_state.u_corner_mask.win_size_uv, win_sw, win_sh);
      glUniform2f(effects_state.u_corner_mask.win_size_px, (float)content_r.width, (float)content_r.height);
      glUniform1f(effects_state.u_corner_mask.border_radius_px, inner_r);
      draw_quad();
      glDisable(GL_BLEND);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    any = true;
  }
  return any;
}

static void push_blur_to_toplevels(output_t *output) {
  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->blur_node || !tl->node) continue;
    output_t *m = tl->node->output;
    if (!m || m != output) continue;

    if (!tl->blur->blur_buf) {
      wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
      continue;
    }

    wlr_scene_buffer_set_buffer(tl->blur->blur_node, tl->blur->blur_buf);

    client_t *c = tl->node->client;
    struct wlr_box r;
    if (c->state == STATE_FULLSCREEN && tl->node->output) r = tl->node->output->rectangle;
    else if (c->state == STATE_FLOATING)                  r = c->floating_rectangle;
    else                                                  r = c->tiled_rectangle;

    struct wlr_fbox src; int dw, dh;
    if (!compute_src_box(output, &r, &src, &dw, &dh)) {
      wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
      wlr_scene_node_set_position(&tl->blur->blur_node->node, 0, 0);
      continue;
    }
    int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
    int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
    wlr_scene_node_set_position(&tl->blur->blur_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->blur->blur_node, &src);
    wlr_scene_buffer_set_dest_size(tl->blur->blur_node, dw, dh);
  }
}

static bool rebuild_live_blur_layers(output_t *output, GLuint bg_tex,
    pixman_region32_t *damage) {
  effects_output_t *ctx = output->effects;
  int w = output->width, h = output->height;
  bool any = false;

  for (int i = 0; i < 4; i++) {
    layer_surface_t *ls;
    wl_list_for_each(ls, &output->layers[i], link) {
      if (!ls->blur_node || !ls->mapped) continue;

      // skip if blur buffer exists and damage doesn't overlap the blur region
      if (ls->blur_buf && damage && !pixman_region32_empty(damage)) {
        int nboxes_check;
        pixman_box32_t *boxes_check = pixman_region32_rectangles(&ls->blur_region, &nboxes_check);
        if (nboxes_check > 0) {
          int lx_check, ly_check;
          if (wlr_scene_node_coords(&ls->scene_tree->node, &lx_check, &ly_check)) {
            int out_lx_check = lx_check - output->lx;
            int out_ly_check = ly_check - output->ly;
            pixman_region32_t blur_rgn, intersection;
            pixman_region32_init_rect(&blur_rgn,
              boxes_check[0].x1 + out_lx_check, boxes_check[0].y1 + out_ly_check,
              boxes_check[0].x2 - boxes_check[0].x1, boxes_check[0].y2 - boxes_check[0].y1);
            for (int b = 1; b < nboxes_check; b++)
              pixman_region32_union_rect(&blur_rgn, &blur_rgn,
                boxes_check[b].x1 + out_lx_check, boxes_check[b].y1 + out_ly_check,
                boxes_check[b].x2 - boxes_check[b].x1, boxes_check[b].y2 - boxes_check[b].y1);

            pixman_region32_init(&intersection);
            pixman_region32_intersect(&intersection, damage, &blur_rgn);
            bool overlaps = !pixman_region32_empty(&intersection);
            pixman_region32_fini(&intersection);
            pixman_region32_fini(&blur_rgn);
            if (!overlaps) continue;
          }
        }
      }

      // get blur region bounds
      int nboxes;
      pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
      if (nboxes == 0) continue;

      // get surface position to convert blur region to output coordinates
      int lx, ly;
      if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly)) continue;

      // convert to output-local coordinates
      int out_lx = lx - output->lx;
      int out_ly = ly - output->ly;

      // find the bounding box of all rectangles in the blur region
      int blur_r_x1 = boxes[0].x1 + out_lx, blur_r_y1 = boxes[0].y1 + out_ly;
      int blur_r_x2 = boxes[0].x2 + out_lx, blur_r_y2 = boxes[0].y2 + out_ly;
      for (int b = 1; b < nboxes; b++) {
        int box_x1 = boxes[b].x1 + out_lx, box_y1 = boxes[b].y1 + out_ly;
        int box_x2 = boxes[b].x2 + out_lx, box_y2 = boxes[b].y2 + out_ly;
        if (box_x1 < blur_r_x1) blur_r_x1 = box_x1;
        if (box_y1 < blur_r_y1) blur_r_y1 = box_y1;
        if (box_x2 > blur_r_x2) blur_r_x2 = box_x2;
        if (box_y2 > blur_r_y2) blur_r_y2 = box_y2;
      }

      int blur_width = blur_r_x2 - blur_r_x1;
      int blur_height = blur_r_y2 - blur_r_y1;
      if (blur_width <= 0 || blur_height <= 0) continue;

      // store the blur region offset for push_blur_to_layers
      ls->blur_region_offset_x = boxes[0].x1;
      ls->blur_region_offset_y = boxes[0].y1;
      for (int b = 1; b < nboxes; b++) {
        if (boxes[b].x1 < ls->blur_region_offset_x) ls->blur_region_offset_x = boxes[b].x1;
        if (boxes[b].y1 < ls->blur_region_offset_y) ls->blur_region_offset_y = boxes[b].y1;
      }
      ls->blur_region_width = blur_width;
      ls->blur_region_height = blur_height;

      GLuint src;
      if (bg_tex) {
        src = bg_tex;
      } else {
        src = capture_bg_to_tex1(output, ctx, false,
          &ls->scene_tree->node, &ls->blur_scene_hidden);
        if (!src) continue;
      }

      GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

      GLuint dest_fbo = ensure_output_buf(&ls->blur_buf, &ls->blur_buf_fbo, w, h);
      if (!dest_fbo)
        continue;

      // clear with transparent, draw the blurred texture clipped to the blur region
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_STENCIL_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
      glViewport(0, 0, w, h);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      // draw the blurred texture only within the blur region rectangles
      glEnable(GL_SCISSOR_TEST);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, blurred);
      glUseProgram(effects_state.prog_blit);
      glUniform1i(effects_state.u_blit.tex, 0);
      for (int b = 0; b < nboxes; b++) {
        int box_x1 = boxes[b].x1 + out_lx;
        int box_y1 = boxes[b].y1 + out_ly;
        glScissor(box_x1, box_y1, boxes[b].x2 - boxes[b].x1, boxes[b].y2 - boxes[b].y1);
        draw_quad();
      }

      glDisable(GL_SCISSOR_TEST);
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      any = true;
    }
  }
  return any;
}

static void push_blur_to_layers(output_t *output) {
  for (int i = 0; i < 4; i++) {
    layer_surface_t *ls;
    wl_list_for_each(ls, &output->layers[i], link) {
      if (!ls->blur_node) continue;

      if (!ls->blur_buf) {
        wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
        continue;
      }

      // get blur region bounds
      if (ls->blur_region_width <= 0 || ls->blur_region_height <= 0) {
        wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
        continue;
      }

      wlr_scene_buffer_set_buffer(ls->blur_node, ls->blur_buf);

      // get surface position for source box calculation
      int lx, ly;
      if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly)) {
        wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
        continue;
      }

      // position blur_node at the blur region offset within the scene_tree
      int blur_r_x = ls->blur_region_offset_x;
      int blur_r_y = ls->blur_region_offset_y;
      int blur_r_w = ls->blur_region_width;
      int blur_r_h = ls->blur_region_height;

      // compute the source box in output-local coordinates
      struct wlr_box r = { .x = lx + blur_r_x, .y = ly + blur_r_y, .width = blur_r_w, .height = blur_r_h };

      struct wlr_fbox src;
      int dw, dh;
      if (!compute_src_box(output, &r, &src, &dw, &dh)) {
        wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
        wlr_scene_node_set_position(&ls->blur_node->node, 0, 0);
        continue;
      }

      int offset_x = (r.x < output->lx) ? (output->lx - r.x) : 0;
      int offset_y = (r.y < output->ly) ? (output->ly - r.y) : 0;

      // position at blur region offset within surface
      wlr_scene_node_set_position(&ls->blur_node->node, blur_r_x + offset_x, blur_r_y + offset_y);
      wlr_scene_buffer_set_source_box(ls->blur_node, &src);
      wlr_scene_buffer_set_dest_size(ls->blur_node, dw, dh);
    }
  }
}

static bool rebuild_live_acrylic(output_t *output,
    pixman_region32_t *damage, GLuint shared_capture) {
  effects_output_t *ctx = output->effects;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->acrylic_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    // skip if toplevel already has a valid acrylic buffer and the damage doesn't overlap it
    if (tl->blur->acrylic_buf && damage && !pixman_region32_empty(damage)) {
      struct wlr_box r = get_client_rect(tl);
      pixman_region32_t tl_rgn, intersection;
      pixman_region32_init_rect(&tl_rgn, r.x - output->lx, r.y - output->ly, r.width, r.height);
      pixman_region32_init(&intersection);
      pixman_region32_intersect(&intersection, damage, &tl_rgn);
      bool overlaps = !pixman_region32_empty(&intersection);
      pixman_region32_fini(&intersection);
      pixman_region32_fini(&tl_rgn);
      if (!overlaps) continue;
    }

    GLuint src;
    if (shared_capture) {
      src = shared_capture;
    } else {
      src = capture_bg_to_tex1(output, ctx, false,
        &tl->scene_tree->node, &tl->blur->blur_scene_hidden);
      if (!src) continue;
    }

    GLuint dest_fbo = ensure_output_buf(&tl->blur->acrylic_buf, &tl->blur->acrylic_buf_fbo, w, h);
    if (!dest_fbo) continue;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    GLuint blurred = src;
    if (acrylic_blur_passes > 0) {
      int ping = (src == ctx->tex[1]) ? 0 : 1;
      GLuint current = src;
      for (int i = 0; i < acrylic_blur_passes; i++) {
        blur_pass(current, ctx->fbo[ping], ctx->blur_w, ctx->blur_h, i);
        current = ctx->tex[ping];
        ping ^= 1;
      }
      blurred = current;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred);
    glUseProgram(effects_state.prog_acrylic_tint);
    glUniform1i(effects_state.u_acrylic.tex, 0);
    glUniform4fv(effects_state.u_acrylic.tint, 1, acrylic_tint);
    glUniform1f(effects_state.u_acrylic.tint_strength, acrylic_tint_strength);
    glUniform1f(effects_state.u_acrylic.noise_strength, acrylic_noise_strength);
    glUniform2f(effects_state.u_acrylic.resolution, (float)w, (float)h);
    glUniform2f(effects_state.u_acrylic.light_anchor, acrylic_light_anchor[0], acrylic_light_anchor[1]);
    draw_quad();

    client_t *c = tl->node->client;
    if (c && c->border_radius > 0.0f && c->state != STATE_FULLSCREEN && effects_state.prog_corner_mask) {
      struct wlr_box content_r = get_client_rect(tl);
      float ow = (float)w, oh = (float)h;
      float win_u = (float)(content_r.x - output->lx) / ow;
      float win_v = (float)(content_r.y - output->ly) / oh;
      float win_sw = (float)content_r.width  / ow;
      float win_sh = (float)content_r.height / oh;
      int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;
      float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

      glEnable(GL_BLEND);
      glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(effects_state.prog_corner_mask);
      glUniform1i(effects_state.u_corner_mask.tex, 0);
      glUniform2f(effects_state.u_corner_mask.win_pos_uv, win_u, win_v);
      glUniform2f(effects_state.u_corner_mask.win_size_uv, win_sw, win_sh);
      glUniform2f(effects_state.u_corner_mask.win_size_px, (float)content_r.width, (float)content_r.height);
      glUniform1f(effects_state.u_corner_mask.border_radius_px, inner_r);
      draw_quad();
      glDisable(GL_BLEND);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    any = true;
  }
  return any;
}

static void push_acrylic_to_toplevels(output_t *output) {
  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->acrylic_node || !tl->node) continue;
    output_t *m = tl->node->output;
    if (!m || m != output) continue;

    if (!tl->blur->acrylic_buf) {
      wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
      continue;
    }

    wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, tl->blur->acrylic_buf);

    client_t *c = tl->node->client;
    struct wlr_box r;
    if (c->state == STATE_FULLSCREEN && tl->node->output) r = tl->node->output->rectangle;
    else if (c->state == STATE_FLOATING)                  r = c->floating_rectangle;
    else                                                  r = c->tiled_rectangle;

    struct wlr_fbox src; int dw, dh;
    if (!compute_src_box(output, &r, &src, &dw, &dh)) {
      wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
      wlr_scene_node_set_position(&tl->blur->acrylic_node->node, 0, 0);
      continue;
    }

    int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
    int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
    wlr_scene_node_set_position(&tl->blur->acrylic_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->blur->acrylic_node, &src);
    wlr_scene_buffer_set_dest_size(tl->blur->acrylic_node, dw, dh);
  }
}

static bool rebuild_mica(output_t *output) {
  effects_output_t *ctx = output->effects;
  int w = output->width, h = output->height;

  GLuint src = capture_bg_to_tex1(output, ctx, true, NULL, NULL);
  if (!src) return false;

  GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

  GLuint dest_fbo = ensure_output_buf(&ctx->mica_buf, &ctx->mica_buf_fbo, w, h);
  if (!dest_fbo)
    return false;

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, blurred);
  glUseProgram(effects_state.prog_mica_tint);
  glUniform1i(effects_state.u_mica.tex, 0);
  glUniform4fv(effects_state.u_mica.tint, 1, mica_tint);
  glUniform1f(effects_state.u_mica.tint_strength, mica_tint_strength);
  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  ctx->mica_dirty = false;
  return true;
}

static void push_mica_to_toplevels(output_t *output) {
  struct wlr_buffer *buf = output->effects->mica_buf;
  if (!buf) return;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->mica_node || !tl->node) continue;
    output_t *m = tl->node->output;
    if (!m || m != output) continue;

    wlr_scene_buffer_set_buffer(tl->blur->mica_node, buf);

    client_t *c = tl->node->client;
    struct wlr_box r;
    if (c->state == STATE_FULLSCREEN && tl->node->output) r = tl->node->output->rectangle;
    else if (c->state == STATE_FLOATING)                  r = c->floating_rectangle;
    else                                                  r = c->tiled_rectangle;

    struct wlr_fbox src;
    int dw, dh;
    if (!compute_src_box(output, &r, &src, &dw, &dh)) {
      wlr_scene_buffer_set_buffer(tl->blur->mica_node, NULL);
      wlr_scene_node_set_position(&tl->blur->mica_node->node, 0, 0);
      continue;
    }

    int node_ox = (r.x < output->lx) ? (output->lx - r.x) : 0;
    int node_oy = (r.y < output->ly) ? (output->ly - r.y) : 0;
    wlr_scene_node_set_position(&tl->blur->mica_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->blur->mica_node, &src);
    wlr_scene_buffer_set_dest_size(tl->blur->mica_node, dw, dh);
  }
}

static struct wlr_box get_client_rect(toplevel_t *tl) {
  client_t *c = tl->node->client;
  if (c->state == STATE_FULLSCREEN && tl->node->output) return tl->node->output->rectangle;
  else if (c->state == STATE_FLOATING)                  return c->floating_rectangle;
  else                                                  return c->tiled_rectangle;
}

static bool scene_buffer_no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
  (void)buffer;
  (void)sx;
  (void)sy;
  return false;
}

static bool blur_render_shadow(toplevel_t *tl) {
  if (!effects_state.prog_shadow) return false;
  if (!tl->shadow) return false;
  if (!tl->node || !tl->node->client) return false;

  client_t *c = tl->node->client;
  if (!c->shadow) return false;
  if (c->state == STATE_FULLSCREEN) return false;

  struct wlr_box client_r = get_client_rect(tl);
  if (client_r.width <= 0 || client_r.height <= 0) return false;

  int size = (int)shadow_size;
  if (size <= 0) return false;
  int buf_w = client_r.width + 2 * size;
  int buf_h = client_r.height + 2 * size;
  if (buf_w <= 0 || buf_h <= 0) return false;

  if (!tl->shadow->shadow_node) {
    tl->shadow->shadow_node = wlr_scene_buffer_create(tl->scene_tree, NULL);
    if (!tl->shadow->shadow_node) return false;
    wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
    tl->shadow->shadow_node->point_accepts_input = scene_buffer_no_input;
  }

  float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
  double phys_buf_w = buf_w * scale;
  double phys_buf_h = buf_h * scale;

  GLuint dest_fbo = ensure_sized_buf(&tl->shadow->shadow_buf, &tl->shadow->shadow_buf_fbo,
    &tl->shadow->shadow_buf_w, &tl->shadow->shadow_buf_h, (int)phys_buf_w, (int)phys_buf_h);
  if (!dest_fbo) return false;

  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, (int)phys_buf_w, (int)phys_buf_h);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(effects_state.prog_shadow);

  glUniform2f(effects_state.u_shadow.resolution, (float)phys_buf_w, (float)phys_buf_h);
  glUniform1f(effects_state.u_shadow.shadow_size, size * scale);
  glUniform4fv(effects_state.u_shadow.shadow_color, 1, c->shadow_color);
  glUniform1f(effects_state.u_shadow.border_radius, c->border_radius * scale);
  glUniform2f(effects_state.u_shadow.inner_size, client_r.width * scale, client_r.height * scale);

  float hole_x = (tl->content_tree->node.x - shadow_offset_x + size) * scale;
  float hole_y = (tl->content_tree->node.y - shadow_offset_y + size) * scale;
  glUniform2f(effects_state.u_shadow.hole_pos, hole_x, hole_y);
  glUniform2f(effects_state.u_shadow.hole_size, client_r.width * scale, client_r.height * scale);

  draw_quad();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND);

  wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
  wlr_scene_buffer_set_buffer(tl->shadow->shadow_node, tl->shadow->shadow_buf);
  struct wlr_fbox src_box = {0, 0, phys_buf_w, phys_buf_h};
  wlr_scene_buffer_set_source_box(tl->shadow->shadow_node, &src_box);
  wlr_scene_buffer_set_dest_size(tl->shadow->shadow_node, buf_w, buf_h);

  wlr_scene_node_set_position(&tl->shadow->shadow_node->node, shadow_offset_x - size, shadow_offset_y - size);
  wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, true);

  return true;
}

static bool blur_render_border(toplevel_t *tl, int content_w, int content_h) {
  if (!effects_state.prog_border) return false;
  if (!tl->border_tree) return false;
  if (!tl->rounded) return false;

  float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
  client_t *c = tl->node->client;
  int bw_i = border_width;
  if (bw_i <= 0) return false;

  double log_fw = (double)content_w + 2 * bw_i;
  double log_fh = (double)content_h + 2 * bw_i;
  if (log_fw <= 0 || log_fh <= 0) return false;

  double phys_fw = log_fw * scale;
  double phys_fh = log_fh * scale;

  if (!tl->rounded->border_shader_node) {
    tl->rounded->border_shader_node = wlr_scene_buffer_create(tl->border_tree, NULL);
    if (!tl->rounded->border_shader_node) return false;
    wlr_scene_node_set_position(&tl->rounded->border_shader_node->node, 0, 0);
    tl->rounded->border_shader_node->point_accepts_input = scene_buffer_no_input;
  }

  GLuint dest_fbo = ensure_sized_buf(&tl->rounded->border_shader_buf, &tl->rounded->border_shader_buf_fbo,
    &tl->rounded->border_shader_buf_w, &tl->rounded->border_shader_buf_h, phys_fw, phys_fh);
  if (!dest_fbo) return false;

  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, phys_fw, phys_fh);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(effects_state.prog_border);

  glUniform2f(effects_state.u_border.resolution, (float)phys_fw, (float)phys_fh);
  glUniform1f(effects_state.u_border.border_radius, (float)c->border_radius * scale);
  glUniform1f(effects_state.u_border.border_width_px, (float)bw_i * scale);
  glUniform4fv(effects_state.u_border.border_color, 1, tl->rounded->border_color);

  // gradient uniforms
  if (effects_state.u_border.gradient_colors >= 0)
    glUniform4fv(effects_state.u_border.gradient_colors, BORDER_GRADIENT_MAX_STOPS, tl->rounded->gradient_colors);
  if (effects_state.u_border.gradient_count >= 0)
    glUniform1i(effects_state.u_border.gradient_count, tl->rounded->gradient_count);
  if (effects_state.u_border.gradient_angle >= 0)
    glUniform1f(effects_state.u_border.gradient_angle, tl->rounded->gradient_angle);
  if (effects_state.u_border.gradient2_colors >= 0)
    glUniform4fv(effects_state.u_border.gradient2_colors, BORDER_GRADIENT_MAX_STOPS, tl->rounded->gradient2_colors);
  if (effects_state.u_border.gradient2_count >= 0)
    glUniform1i(effects_state.u_border.gradient2_count, tl->rounded->gradient2_count);
  if (effects_state.u_border.gradient2_angle >= 0)
    glUniform1f(effects_state.u_border.gradient2_angle, tl->rounded->gradient2_angle);
  if (effects_state.u_border.gradient_lerp >= 0)
    glUniform1f(effects_state.u_border.gradient_lerp, tl->rounded->gradient_lerp);

  draw_quad();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND);

  wlr_scene_buffer_set_buffer(tl->rounded->border_shader_node, tl->rounded->border_shader_buf);
  struct wlr_fbox src_box = {0, 0, phys_fw, phys_fh};
  wlr_scene_buffer_set_source_box(tl->rounded->border_shader_node, &src_box);
  wlr_scene_buffer_set_dest_size(tl->rounded->border_shader_node, log_fw, log_fh);
  wlr_scene_node_set_enabled(&tl->rounded->border_shader_node->node, true);

  static const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int i = 0; i < 4; i++)
    if (tl->border_rects[i])
      wlr_scene_rect_set_color(tl->border_rects[i], transparent);

  return true;
}

static bool rebuild_corner_masks(output_t *output, GLuint bg_tex) {
  if (!effects_state.prog_corner_mask) return false;
  effects_output_t *ctx = output->effects;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->rounded || !tl->rounded->corner_mask_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    client_t *c = tl->node->client;
    if (c->border_radius <= 0.0f || c->state == STATE_FULLSCREEN) continue;

    struct wlr_box container_r = get_client_rect(tl);
    int cx = tl->content_tree->node.x;
    int cy = tl->content_tree->node.y;
    int surf_w = (tl->geometry.width > 0 && tl->geometry.width < container_r.width)
      ? (int)tl->geometry.width : container_r.width;
    int surf_h = (tl->geometry.height > 0 && tl->geometry.height < container_r.height)
      ? (int)tl->geometry.height : container_r.height;
    struct wlr_box content_r = {
      .x = container_r.x + cx,
      .y = container_r.y + cy,
      .width  = surf_w,
      .height = surf_h,
    };
    if (content_r.width <= 0 || content_r.height <= 0) continue;

    GLuint src;
    if (bg_tex) {
      src = bg_tex;
    } else {
      int cw = output->width, ch = output->height;

      wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);
      wlr_scene_node_set_enabled(&server.top_tree->node, false);
      wlr_scene_node_set_enabled(&server.full_tree->node, false);
      wlr_scene_node_set_enabled(&server.over_tree->node, false);
      wlr_scene_node_set_enabled(&server.lock_tree->node, false);

      struct { struct wlr_scene_node *node; bool was; } restore[8];
      int nr = 0;

      #define HIDE_IF(n) do {                     \
        if ((n) && (n)->enabled) {                \
          restore[nr].node = (n);                 \
          restore[nr].was = true;                 \
          wlr_scene_node_set_enabled((n), false); \
          nr++;                                   \
        }                                         \
      } while(0)

      HIDE_IF(&tl->content_tree->node);
      HIDE_IF(&tl->border_tree->node);
      if (tl->blur) {
        HIDE_IF(&tl->blur->blur_node->node);
        HIDE_IF(&tl->blur->mica_node->node);
        HIDE_IF(&tl->blur->acrylic_node->node);
      }
      if (tl->rounded)
        HIDE_IF(&tl->rounded->corner_mask_node->node);

      wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);
      struct wlr_output_state cap_state;
      wlr_output_state_init(&cap_state);
      wlr_output_state_set_enabled(&cap_state, true);
      wlr_output_state_set_custom_mode(&cap_state, cw, ch, 0);
      egl_unset_current();
      bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);
      egl_make_current();

      for (int i = 0; i < nr; i++)
        wlr_scene_node_set_enabled(restore[i].node, true);

      #undef HIDE_IF

      wlr_scene_node_set_enabled(&server.top_tree->node, true);
      wlr_scene_node_set_enabled(&server.full_tree->node, true);
      wlr_scene_node_set_enabled(&server.over_tree->node, true);
      wlr_scene_node_set_enabled(&server.lock_tree->node, true);
      wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

      if (!ok || !cap_state.buffer) {
        wlr_output_state_finish(&cap_state);
        continue;
      }

      GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
      src = capture_readback(ctx, capture_fbo, cw, ch);
      wlr_output_state_finish(&cap_state);
      if (!src) continue;
    }

    GLuint dest_fbo = ensure_output_buf(&tl->rounded->corner_mask_buf,
      &tl->rounded->corner_mask_buf_fbo, w, h);
    if (!dest_fbo)
      continue;

    float ow = (float)w, oh = (float)h;
    int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;

    float outer_x = (float)(content_r.x - output->lx - bw_i);
    float outer_y = (float)(content_r.y - output->ly - bw_i);
    float outer_w = (float)(content_r.width  + 2 * bw_i);
    float outer_h = (float)(content_r.height + 2 * bw_i);

    float win_u = outer_x / ow;
    float win_v = outer_y / oh;
    float win_sw = outer_w / ow;
    float win_sh = outer_h / oh;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUseProgram(effects_state.prog_corner_mask);
    glUniform1i(effects_state.u_corner_mask.tex, 0);
    glUniform2f(effects_state.u_corner_mask.win_pos_uv,  win_u,  win_v);
    glUniform2f(effects_state.u_corner_mask.win_size_uv, win_sw, win_sh);
    glUniform2f(effects_state.u_corner_mask.win_size_px, outer_w, outer_h);
    glUniform1f(effects_state.u_corner_mask.border_radius_px, c->border_radius);
    draw_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    any = true;
  }
  return any;
}

static void push_corner_masks_to_toplevels(output_t *output) {
  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->rounded || !tl->rounded->corner_mask_node || !tl->node || !tl->node->client) continue;
    output_t *m = tl->node->output;
    if (!m || m != output) continue;

    client_t *c = tl->node->client;
    if (c->border_radius <= 0.0f || c->state == STATE_FULLSCREEN) {
      wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, NULL);
      continue;
    }
    if (!tl->rounded->corner_mask_buf) {
      wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, false);
      continue;
    }

    struct wlr_box content_r = get_client_rect(tl);
    int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : border_width;

    struct wlr_box outer_r = {
      .x = content_r.x - bw_i,
      .y = content_r.y - bw_i,
      .width = content_r.width  + 2 * bw_i,
      .height = content_r.height + 2 * bw_i,
    };

    struct wlr_fbox src; int dw, dh;
    if (!compute_src_box(output, &outer_r, &src, &dw, &dh)) {
      wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, false);
      continue;
    }

    int node_ox = (outer_r.x < output->lx) ? (output->lx - outer_r.x) : -bw_i;
    int node_oy = (outer_r.y < output->ly) ? (output->ly - outer_r.y) : -bw_i;

    wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, true);
    wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, tl->rounded->corner_mask_buf);
    wlr_scene_node_set_position(&tl->rounded->corner_mask_node->node, node_ox, node_oy);
    wlr_scene_buffer_set_source_box(tl->rounded->corner_mask_node, &src);
    wlr_scene_buffer_set_dest_size(tl->rounded->corner_mask_node, dw, dh);
  }
}

static GLuint capture_full_scene_to_tex(output_t *output, effects_output_t *ctx) {
  int w = output->width, h = output->height;

  if (!ctx->capture_output || !ctx->capture_scene_output || !ctx->screen_fbo) return 0;

  wlr_scene_output_set_position(ctx->capture_scene_output, output->lx, output->ly);

  if (w <= 0 || h <= 0) return 0;

  // hide the shader overlay to avoid a feedback loop
  if (server.shader_tree)
    wlr_scene_node_set_enabled(&server.shader_tree->node, false);

  wlr_damage_ring_add_whole(&ctx->capture_scene_output->damage_ring);

  struct wlr_output_state cap_state;
  wlr_output_state_init(&cap_state);
  wlr_output_state_set_enabled(&cap_state, true);
  wlr_output_state_set_custom_mode(&cap_state, w, h, 0);

  egl_unset_current();
  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);

  egl_make_current();

  if (server.shader_tree)
    wlr_scene_node_set_enabled(&server.shader_tree->node, true);

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);

  if (!ok || !cap_state.buffer) {
    wlr_output_state_finish(&cap_state);
    return 0;
  }

  GLuint capture_fbo = wlr_gles2_renderer_get_buffer_fbo(server.renderer, cap_state.buffer);
  GLuint result = 0;

  if (capture_fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    GLint attach_type = 0, attach_name = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attach_type);
    if (attach_type == GL_TEXTURE) {
      glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &attach_name);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (attach_type == GL_TEXTURE && attach_name > 0 && effects_state.prog_ext_blit) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->screen_fbo);
      glViewport(0, 0, w, h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
      glUseProgram(effects_state.prog_ext_blit);
      glUniform1i(effects_state.u_ext_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->screen_tex;
    } else if (attach_type == GL_TEXTURE && attach_name > 0) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->screen_fbo);
      glViewport(0, 0, w, h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, (GLuint)attach_name);
      glUseProgram(effects_state.prog_blit);
      glUniform1i(effects_state.u_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->screen_tex;
    } else if (attach_type == GL_RENDERBUFFER) {
      glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
      glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
      glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
      glBindTexture(GL_TEXTURE_2D, 0);
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->screen_fbo);
      glViewport(0, 0, w, h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, ctx->staging_tex);
      glUseProgram(effects_state.prog_blit);
      glUniform1i(effects_state.u_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->screen_tex;
    }
  }

  wlr_output_state_finish(&cap_state);
  return result;
}

static void handle_screen_shader_frame(output_t *output) {
  effects_output_t *ctx = output->effects;
  if (!ctx || !ctx->screen_shader_node) return;

  if (!screen_shader_enabled || !screen_shader_prog) {
    wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
    return;
  }

  int w = output->width, h = output->height;
  if (w <= 0 || h <= 0) return;

  GLuint src = capture_full_scene_to_tex(output, ctx);
  if (!src) {
    wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
    return;
  }

  GLuint dest_fbo = ensure_output_buf(&ctx->screen_shader_buf, &ctx->screen_shader_buf_fbo, w, h);
  if (!dest_fbo) {
    wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
    return;
  }

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src);
  glUseProgram(screen_shader_prog);

  if (screen_shader_u_tex >= 0)
    glUniform1i(screen_shader_u_tex, 0);
  if (screen_shader_u_resolution >= 0)
    glUniform2f(screen_shader_u_resolution, (float)w, (float)h);

  if (screen_shader_u_time >= 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    float t = (float)(now.tv_sec - screen_shader_start_time.tv_sec) +
      (float)(now.tv_nsec - screen_shader_start_time.tv_nsec) * 1e-9f;
    glUniform1f(screen_shader_u_time, t);
  }
  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  wlr_scene_buffer_set_buffer(ctx->screen_shader_node, ctx->screen_shader_buf);
  struct wlr_fbox src_box = {0, 0, (double)w, (double)h};
  wlr_scene_buffer_set_source_box(ctx->screen_shader_node, &src_box);
  wlr_scene_buffer_set_dest_size(ctx->screen_shader_node, w, h);
  wlr_scene_node_set_position(&ctx->screen_shader_node->node, output->lx, output->ly);
  wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, true);
}

void effects_evict_buffers(void) {
  if (!effects_state.available) return;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    bool visible = tl->node && tl->node->client && tl->node->client->shown;

    if (tl->blur) {
    	// visual eviction
      if (tl->blur->blur_node && !blur_enabled)
        wlr_scene_buffer_set_buffer(tl->blur->blur_node, NULL);
      if (tl->blur->acrylic_node && !blur_enabled)
        wlr_scene_buffer_set_buffer(tl->blur->acrylic_node, NULL);
      if (tl->blur->mica_node && !mica_enabled)
        wlr_scene_buffer_set_buffer(tl->blur->mica_node, NULL);

     	// memory eviction
      if (tl->blur->blur_buf && !visible) {
        wlr_buffer_unlock(tl->blur->blur_buf);
        tl->blur->blur_buf = NULL;
        tl->blur->blur_buf_fbo = 0;
      }
      if (tl->blur->acrylic_buf && !visible) {
        wlr_buffer_unlock(tl->blur->acrylic_buf);
        tl->blur->acrylic_buf = NULL;
        tl->blur->acrylic_buf_fbo = 0;
      }
    }

    // only evict from hidden toplevels
    if (tl->rounded) {
      if (tl->rounded->corner_mask_buf && !visible) {
        if (tl->rounded->corner_mask_node)
          wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, NULL);
        wlr_buffer_unlock(tl->rounded->corner_mask_buf);
        tl->rounded->corner_mask_buf = NULL;
        tl->rounded->corner_mask_buf_fbo = 0;
      }
      if (tl->rounded->border_shader_buf && !visible) {
        if (tl->rounded->border_shader_node)
          wlr_scene_buffer_set_buffer(tl->rounded->border_shader_node, NULL);
        wlr_buffer_unlock(tl->rounded->border_shader_buf);
        tl->rounded->border_shader_buf = NULL;
        tl->rounded->border_shader_buf_fbo = 0;
        tl->rounded->border_shader_buf_w = 0;
        tl->rounded->border_shader_buf_h = 0;
      }
    }
  }
}

static bool damage_in_tree(pixman_region32_t *damage, struct wlr_scene_tree *tree) {
  int nrects;
  pixman_box32_t *rects = pixman_region32_rectangles(damage, &nrects);
  for (int i = 0; i < nrects; i++) {
    double cx = (rects[i].x1 + rects[i].x2) * 0.5;
    double cy = (rects[i].y1 + rects[i].y2) * 0.5;
    double nx, ny;
    if (wlr_scene_node_at(&tree->node, cx, cy, &nx, &ny))
      return true;
  }
  return false;
}

static bool background_capture_needed(struct wlr_scene_output *scene_output) {
  pixman_region32_t *damage = &scene_output->damage_ring.current;
  if (pixman_region32_empty(damage)) return false;

  // only check trees not hidden during background capture
  struct wlr_scene_tree *relevant[] = {
    server.bg_tree, server.bot_tree, server.tile_tree,
    server.float_tree, server.shader_tree, server.drag_tree,
  };

  for (size_t i = 0; i < sizeof(relevant)/sizeof(relevant[0]); i++)
    if (damage_in_tree(damage, relevant[i])) return true;

  return false;
}

void effects_output_frame(output_t *output, struct wlr_scene_output *scene_output) {
  if (!effects_state.available) return;
  effects_output_t *ctx = output->effects;
  if (!ctx) return;

  if (animation_workspace_switch_active()) return;

  effects_evict_buffers();

  if (ctx->width != output->width || ctx->height != output->height)
    effects_output_resize(ctx, output->width, output->height, output);

  egl_make_current();

  {
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (!tl->shadow || (!tl->shadow->shadow_dirty && !tl->shadow->shadow_geometry_dirty)) continue;
      if (!tl->node || !tl->node->client || !tl->node->client->shown) continue;
      if (!tl->node->output || tl->node->output != output) continue;

      if (!tl->node->client->shadow) {
        tl->shadow->shadow_dirty = false;
        tl->shadow->shadow_geometry_dirty = false;
        if (tl->shadow->shadow_node)
          wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, false);
        continue;
      }

      if (tl->shadow->shadow_geometry_dirty && !tl->shadow->shadow_dirty) {
        int size = (int)shadow_size;
        if (tl->shadow->shadow_node && size > 0) {
          struct wlr_box client_r = get_client_rect(tl);
          int buf_w = client_r.width + 2 * size;
          int buf_h = client_r.height + 2 * size;
          if (buf_w > 0 && buf_h > 0) {
            float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
            if (tl->shadow->shadow_buf_w == (int)(buf_w * scale) &&
                tl->shadow->shadow_buf_h == (int)(buf_h * scale)) {
              wlr_scene_node_lower_to_bottom(&tl->shadow->shadow_node->node);
              wlr_scene_node_set_position(&tl->shadow->shadow_node->node, shadow_offset_x - size, shadow_offset_y - size);
              wlr_scene_node_set_enabled(&tl->shadow->shadow_node->node, true);
              tl->shadow->shadow_geometry_dirty = false;
              continue;
            }
          }
        }
      }

      blur_render_shadow(tl);
      tl->shadow->shadow_dirty = false;
      tl->shadow->shadow_geometry_dirty = false;
    }
  }

  bool bg_damaged = background_capture_needed(scene_output);
  bool mica_dirty = mica_enabled && ctx->mica_dirty;

  // when no relevant damage and mica not dirty, skip all background captures
  if (!bg_damaged && !mica_dirty) {
  	if (blur_enabled) {
      push_blur_to_toplevels(output);
      push_blur_to_layers(output);
   	}
   	push_corner_masks_to_toplevels(output);
    push_acrylic_to_toplevels(output);
    push_mica_to_toplevels(output);
    goto after_capture;
  }

  // check if layer blur surfaces need rendering
  bool has_layer_blur = false;
  if (blur_enabled) {
    for (int i = 0; i < 4 && !has_layer_blur; i++) {
      layer_surface_t *ls;
      wl_list_for_each(ls, &output->layers[i], link) {
        if (ls->blur_node && ls->mapped) {
          has_layer_blur = true;
          break;
        }
      }
    }
  }

  // check if corner masks need rendering
  bool any_cm = false;
  {
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (tl->rounded && tl->rounded->corner_mask_node && tl->node && tl->node->client &&
          tl->node->client->border_radius > 0.0f &&
          tl->node->client->state != STATE_FULLSCREEN &&
          tl->node->output && tl->node->output == output) {
        any_cm = true;
        break;
      }
    }
  }

  // shared background capture for toplevel blur/acrylic
  GLuint shared_capture = 0;
  GLuint shared_blurred = 0;
  {
    bool any_toplevel_effect = false;
    if (bg_damaged) {
      toplevel_t *tl;
      wl_list_for_each(tl, &server.toplevels, link) {
        if (!tl->blur || !tl->node || !tl->node->client || !tl->node->client->shown) continue;
        if (tl->node->output != output) continue;
        if (tl->blur->blur_node || tl->blur->acrylic_node) {
          any_toplevel_effect = true;
          break;
        }
      }
    }
    if (any_toplevel_effect) {
      shared_capture = capture_bg_shared(output, ctx);
      if (shared_capture && blur_enabled)
        shared_blurred = apply_blur(ctx, shared_capture, ctx->blur_w, ctx->blur_h);
    }
  }

  // toplevel blur
  if (blur_enabled) {
    bool any_blur = false;
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (tl->blur && tl->blur->blur_node && tl->node && tl->node->client && tl->node->client->shown &&
          tl->node->output && tl->node->output == output) {
        any_blur = true;
        break;
      }
    }
    if (any_blur) {
      rebuild_live_blur(output, &scene_output->damage_ring.current, shared_blurred);
      push_blur_to_toplevels(output);
    }
  }

  // apply corner masks and blur if needed
  if (has_layer_blur) {
    bool any_layer_needs_blur = false;
    if (blur_enabled && bg_damaged) {
      pixman_region32_t *damage = &scene_output->damage_ring.current;
      for (int i = 0; i < 4 && !any_layer_needs_blur; i++) {
        layer_surface_t *ls;
        wl_list_for_each(ls, &output->layers[i], link) {
          if (!ls->blur_node || !ls->mapped) continue;
          if (!ls->blur_buf) { any_layer_needs_blur = true; break; }
          if (pixman_region32_empty(damage)) continue;
          int nboxes;
          pixman_box32_t *boxes = pixman_region32_rectangles(&ls->blur_region, &nboxes);
          if (nboxes == 0) continue;
          int lx, ly;
          if (!wlr_scene_node_coords(&ls->scene_tree->node, &lx, &ly)) continue;
          int out_lx = lx - output->lx, out_ly = ly - output->ly;
          pixman_region32_t blur_rgn, intersection;
          pixman_region32_init_rect(&blur_rgn,
            boxes[0].x1 + out_lx, boxes[0].y1 + out_ly,
            boxes[0].x2 - boxes[0].x1, boxes[0].y2 - boxes[0].y1);
          for (int b = 1; b < nboxes; b++)
            pixman_region32_union_rect(&blur_rgn, &blur_rgn,
              boxes[b].x1 + out_lx, boxes[b].y1 + out_ly,
              boxes[b].x2 - boxes[b].x1, boxes[b].y2 - boxes[b].y1);

          pixman_region32_init(&intersection);
          pixman_region32_intersect(&intersection, damage, &blur_rgn);
          if (!pixman_region32_empty(&intersection)) any_layer_needs_blur = true;
          pixman_region32_fini(&intersection);
          pixman_region32_fini(&blur_rgn);
        }
      }
    }

    if (any_layer_needs_blur && any_cm) {
      GLuint bg_tex = capture_bg_combined(output, ctx);
      if (bg_tex && blur_enabled) {
        rebuild_live_blur_layers(output, bg_tex, &scene_output->damage_ring.current);
        push_blur_to_layers(output);
      } else if (blur_enabled) {
        rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
        push_blur_to_layers(output);
      }
      if (effects_state.prog_corner_mask) {
        rebuild_corner_masks(output, 0);
        push_corner_masks_to_toplevels(output);
      }
    } else if (any_layer_needs_blur) {
      rebuild_live_blur_layers(output, 0, &scene_output->damage_ring.current);
      push_blur_to_layers(output);
    } else if (any_cm && effects_state.prog_corner_mask) {
      rebuild_corner_masks(output, 0);
      push_corner_masks_to_toplevels(output);
    }
  } else if (any_cm && effects_state.prog_corner_mask) {
    rebuild_corner_masks(output, 0);
    push_corner_masks_to_toplevels(output);
  }

  {
    bool any_acrylic = false;
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (tl->blur && tl->blur->acrylic_node && tl->node && tl->node->client && tl->node->client->shown &&
          tl->node->output && tl->node->output == output) {
        any_acrylic = true;
        break;
      }
    }
    if (any_acrylic) {
      rebuild_live_acrylic(output, &scene_output->damage_ring.current, shared_capture);
      push_acrylic_to_toplevels(output);
    }
  }

  if (mica_dirty)
    rebuild_mica(output);

  if (mica_enabled && ctx->mica_buf)
    push_mica_to_toplevels(output);

after_capture:

  // shader border
  {
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (!tl->rounded || !tl->rounded->border_dirty) continue;
      if (!tl->node || !tl->node->client || !tl->node->client->shown) continue;
      if (!tl->node->output || tl->node->output != output) continue;
      client_t *c = tl->node->client;

      // use shader if rounded corners or a gradient is set
      bool has_gradient = (tl->rounded->gradient_count >= 2);
      if (c->border_radius <= 0.0f && !has_gradient) {
        tl->rounded->border_dirty = false;
        continue;
      }

      struct wlr_box content_r = get_client_rect(tl);
      if ((c->state == STATE_TILED || c->state == STATE_PSEUDO_TILED) && tl->geometry.width > 0 && tl->geometry.height > 0) {
        if ((int)tl->geometry.width < content_r.width)
          content_r.width = tl->geometry.width;
        if ((int)tl->geometry.height < content_r.height)
          content_r.height = tl->geometry.height;
      }

      blur_render_border(tl, content_r.width, content_r.height);
      tl->rounded->border_dirty = false;
    }
  }

  handle_screen_shader_frame(output);

  egl_unset_current();
}

enum blur_algorithm blur_algorithm_from_str(const char *str) {
  if (!str) return BLUR_ALGORITHM_KAWASE;
  if (strcmp(str, "kawase") == 0) return BLUR_ALGORITHM_KAWASE;
  if (strcmp(str, "gaussian") == 0) return BLUR_ALGORITHM_GAUSSIAN;
  if (strcmp(str, "box") == 0) return BLUR_ALGORITHM_BOX;
  if (strcmp(str, "refraction") == 0) return BLUR_ALGORITHM_REFRACTION;
  if (strcmp(str, "lens_refraction") == 0) return BLUR_ALGORITHM_LENS_REFRACTION;
  if (strcmp(str, "none") == 0) return BLUR_ALGORITHM_NONE;
  wlr_log(WLR_ERROR, "blur: unknown algorithm '%s', using kawase", str);
  return BLUR_ALGORITHM_KAWASE;
}

const char *effects_algorithm_to_str(enum blur_algorithm algo) {
  switch (algo) {
  case BLUR_ALGORITHM_KAWASE: return "kawase";
  case BLUR_ALGORITHM_GAUSSIAN: return "gaussian";
  case BLUR_ALGORITHM_BOX: return "box";
  case BLUR_ALGORITHM_REFRACTION: return "refraction";
  case BLUR_ALGORITHM_LENS_REFRACTION: return "lens_refraction";
  default: return "none";
  }
}

static void screen_shader_set_prog(GLuint prog, const char *name) {
  if (screen_shader_prog)
    glDeleteProgram(screen_shader_prog);

  screen_shader_prog = prog;

  if (prog) {
    screen_shader_u_tex = glGetUniformLocation(prog, "tex");
    screen_shader_u_resolution = glGetUniformLocation(prog, "resolution");
    screen_shader_u_time = glGetUniformLocation(prog, "time");
    clock_gettime(CLOCK_MONOTONIC, &screen_shader_start_time);
    snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "%s", name);
    screen_shader_enabled = true;
  } else {
    screen_shader_u_tex = screen_shader_u_resolution = screen_shader_u_time = -1;
    snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "none");
  }
}

bool screen_shader_set(const char *name) {
  if (!effects_state.available) return false;
  if (!name || strcmp(name, "none") == 0) {
    screen_shader_clear();
    return true;
  }

  const char *frag = NULL;
  if      (strcmp(name, "grayscale")  == 0) frag = grayscale_frag_src;
  else if (strcmp(name, "invert")     == 0) frag = invert_frag_src;
  else if (strcmp(name, "sepia")      == 0) frag = sepia_frag_src;
  else if (strcmp(name, "nightlight") == 0) frag = nightlight_frag_src;
  else return false;

  egl_make_current();
  GLuint prog = link_program(frag);
  if (!prog) {
    egl_unset_current();
    return false;
  }
  screen_shader_set_prog(prog, name);
  egl_unset_current();
  return true;
}

bool screen_shader_load_file(const char *path) {
  if (!effects_state.available || !path) return false;

  FILE *f = fopen(path, "r");
  if (!f) {
    wlr_log(WLR_ERROR, "screen_shader: cannot open '%s'", path);
    return false;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);

  if (size <= 0 || size > 1024 * 1024) {
    fclose(f);
    wlr_log(WLR_ERROR, "screen_shader: file '%s' too large or empty", path);
    return false;
  }

  char *src = malloc((size_t)size + 1);
  if (!src) {
  	fclose(f);
   	return false;
  }

  size_t nread = fread(src, 1, (size_t)size, f);
  fclose(f);
  src[nread] = '\0';

  egl_make_current();
  GLuint prog = link_program(src);
  free(src);
  if (!prog) {
    egl_unset_current();
    return false;
  }
  screen_shader_set_prog(prog, path);
  egl_unset_current();
  return true;
}

void screen_shader_clear(void) {
  if (screen_shader_prog && effects_state.available) {
    egl_make_current();
    glDeleteProgram(screen_shader_prog);
    egl_unset_current();
  }

  screen_shader_prog = 0;
  screen_shader_enabled = false;
  screen_shader_u_tex = screen_shader_u_resolution = screen_shader_u_time = -1;
  snprintf(screen_shader_name_str, sizeof(screen_shader_name_str), "none");
}

const char *screen_shader_get_name(void) {
  return screen_shader_name_str;
}
