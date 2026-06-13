#include "blur.h"
#include "animation.h"
#include "server.h"
#include "output.h"
#include "toplevel.h"
#include "layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <wlr/util/log.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <drm_fourcc.h>

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

enum blur_algorithm blur_algorithm = BLUR_ALGORITHM_KAWASE;
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

blur_ctx_t blur_ctx = {0};

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

static bool create_capture_output(blur_output_ctx_t *ctx, int width, int height) {
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

static void destroy_capture_output(blur_output_ctx_t *ctx) {
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
  glBindBuffer(GL_ARRAY_BUFFER, blur_ctx.vbo);
  glEnableVertexAttribArray(blur_ctx.attr_pos);
  glVertexAttribPointer(blur_ctx.attr_pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(blur_ctx.attr_pos);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void blur_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int pass_index) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);

  glUseProgram(blur_ctx.prog_kawase);
  glUniform1i(blur_ctx.u_kawase.tex, 0);
  glUniform2f(blur_ctx.u_kawase.halfpixel, 0.5f / (float)w, 0.5f / (float)h);
  glUniform1f(blur_ctx.u_kawase.offset, (float)(pass_index + 1));
  if (blur_ctx.u_kawase.noise_strength >= 0)
    glUniform1f(blur_ctx.u_kawase.noise_strength, blur_noise_strength);
  if (blur_ctx.u_kawase.vibrancy >= 0)
    glUniform1f(blur_ctx.u_kawase.vibrancy, blur_vibrancy);
  if (blur_ctx.u_kawase.vibrancy_darkness >= 0)
    glUniform1f(blur_ctx.u_kawase.vibrancy_darkness, blur_vibrancy_darkness);
  if (blur_ctx.u_kawase.brightness >= 0)
    glUniform1f(blur_ctx.u_kawase.brightness, blur_brightness);
  if (blur_ctx.u_kawase.contrast >= 0)
    glUniform1f(blur_ctx.u_kawase.contrast, blur_contrast);

  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void refraction_pass(GLuint src_tex, GLuint dst_fbo, int w, int h, int refraction_mode) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_tex);

  glUseProgram(blur_ctx.prog_refraction);
  glUniform1i(blur_ctx.u_refraction.tex, 0);

  if (blur_ctx.u_refraction.offset >= 0)
    glUniform1f(blur_ctx.u_refraction.offset, refraction_offset);
  if (blur_ctx.u_refraction.halfpixel >= 0)
    glUniform2f(blur_ctx.u_refraction.halfpixel, 0.5f / (float)w, 0.5f / (float)h);

  if (blur_ctx.u_refraction.refraction_rect_size >= 0)
    glUniform2f(blur_ctx.u_refraction.refraction_rect_size, (float)w, (float)h);

  float max_edge = 0.5f * (float)((w < h) ? w : h);
  float edge = refraction_edge_size_px;
  if (edge > max_edge) edge = max_edge;
  if (edge < 0.0f) edge = 0.0f;
  if (blur_ctx.u_refraction.refraction_edge_size_pixels >= 0)
    glUniform1f(blur_ctx.u_refraction.refraction_edge_size_pixels, edge);

  float max_corner = 0.5f * (float)((w < h) ? w : h);
  float corner = refraction_corner_radius_px;
  if (corner > max_corner) corner = max_corner;
  if (corner < 0.0f) corner = 0.0f;
  if (blur_ctx.u_refraction.refraction_corner_radius_pixels >= 0)
    glUniform1f(blur_ctx.u_refraction.refraction_corner_radius_pixels, corner);

  float strength_norm = refraction_strength / 30.0f;
  if (strength_norm < 0.0f) strength_norm = 0.0f;
  if (strength_norm > 1.0f) strength_norm = 1.0f;
  if (blur_ctx.u_refraction.refraction_strength >= 0)
    glUniform1f(blur_ctx.u_refraction.refraction_strength, strength_norm);

  if (blur_ctx.u_refraction.refraction_normal_pow >= 0)
    glUniform1f(blur_ctx.u_refraction.refraction_normal_pow, refraction_normal_pow);

  float fringing = refraction_rgb_fringing;
  if (fringing < 0.0f) fringing = 0.0f;
  if (fringing > 1.0f) fringing = 1.0f;
  if (blur_ctx.u_refraction.refraction_RGB_fringing >= 0)
    glUniform1f(blur_ctx.u_refraction.refraction_RGB_fringing, fringing);

  if (blur_ctx.u_refraction.refraction_texture_repeat_mode >= 0)
    glUniform1i(blur_ctx.u_refraction.refraction_texture_repeat_mode, refraction_texture_repeat_mode);

  if (blur_ctx.u_refraction.refraction_mode >= 0)
    glUniform1i(blur_ctx.u_refraction.refraction_mode, refraction_mode);

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
  glUseProgram(blur_ctx.prog_box_h);
  glUniform1i(blur_ctx.u_box.tex, 0);
  glUniform2f(blur_ctx.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(blur_ctx.u_box.radius, blur_radius);
  if (blur_ctx.u_box.vibrancy >= 0)
    glUniform1f(blur_ctx.u_box.vibrancy, blur_vibrancy);
  if (blur_ctx.u_box.vibrancy_darkness >= 0)
    glUniform1f(blur_ctx.u_box.vibrancy_darkness, blur_vibrancy_darkness);
  if (blur_ctx.u_box.brightness >= 0)
    glUniform1f(blur_ctx.u_box.brightness, blur_brightness);
  if (blur_ctx.u_box.contrast >= 0)
    glUniform1f(blur_ctx.u_box.contrast, blur_contrast);
  draw_quad();

  // V: ping_tex -> pong_fbo
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(blur_ctx.prog_box_v);
  glUniform1i(blur_ctx.u_box.tex, 0);
  glUniform2f(blur_ctx.u_box.texel_size, 1.0f / w, 1.0f / h);
  glUniform1f(blur_ctx.u_box.radius, blur_radius);
  if (blur_ctx.u_box.vibrancy >= 0)
    glUniform1f(blur_ctx.u_box.vibrancy, blur_vibrancy);
  if (blur_ctx.u_box.vibrancy_darkness >= 0)
    glUniform1f(blur_ctx.u_box.vibrancy_darkness, blur_vibrancy_darkness);
  if (blur_ctx.u_box.brightness >= 0)
    glUniform1f(blur_ctx.u_box.brightness, blur_brightness);
  if (blur_ctx.u_box.contrast >= 0)
    glUniform1f(blur_ctx.u_box.contrast, blur_contrast);
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
  glUseProgram(blur_ctx.prog_gauss_h);
  glUniform1i(blur_ctx.u_gauss.tex, 0);
  glUniform2f(blur_ctx.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(blur_ctx.u_gauss.radius, blur_radius);
  if (blur_ctx.u_gauss.vibrancy >= 0)
    glUniform1f(blur_ctx.u_gauss.vibrancy, blur_vibrancy);
  if (blur_ctx.u_gauss.vibrancy_darkness >= 0)
    glUniform1f(blur_ctx.u_gauss.vibrancy_darkness, blur_vibrancy_darkness);
  if (blur_ctx.u_gauss.brightness >= 0)
    glUniform1f(blur_ctx.u_gauss.brightness, blur_brightness);
  if (blur_ctx.u_gauss.contrast >= 0)
    glUniform1f(blur_ctx.u_gauss.contrast, blur_contrast);
  draw_quad();

  // ping_tex -> pong_fbo (vertical pass)
  glBindFramebuffer(GL_FRAMEBUFFER, pong_fbo);
  glBindTexture(GL_TEXTURE_2D, ping_tex);
  glUseProgram(blur_ctx.prog_gauss_v);
  glUniform1i(blur_ctx.u_gauss.tex, 0);
  glUniform2f(blur_ctx.u_gauss.texel_size, 1.0f/w, 1.0f/h);
  glUniform1f(blur_ctx.u_gauss.radius, blur_radius);
  if (blur_ctx.u_gauss.vibrancy >= 0)
    glUniform1f(blur_ctx.u_gauss.vibrancy, blur_vibrancy);
  if (blur_ctx.u_gauss.vibrancy_darkness >= 0)
    glUniform1f(blur_ctx.u_gauss.vibrancy_darkness, blur_vibrancy_darkness);
  if (blur_ctx.u_gauss.brightness >= 0)
    glUniform1f(blur_ctx.u_gauss.brightness, blur_brightness);
  if (blur_ctx.u_gauss.contrast >= 0)
    glUniform1f(blur_ctx.u_gauss.contrast, blur_contrast);
  draw_quad();

  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLuint apply_blur(blur_output_ctx_t *ctx, GLuint src_tex, int w, int h) {
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

bool blur_init(void) {
  blur_ctx = (blur_ctx_t){0};

  if (!wlr_renderer_is_gles2(server.renderer)) {
    wlr_log(WLR_INFO, "blur: renderer is not GLES2 – blur disabled");
    return false;
  }

  struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server.renderer);
  s_egl_display = wlr_egl_get_display(egl);
  s_egl_context = wlr_egl_get_context(egl);

  if (!egl_make_current()) {
    wlr_log(WLR_ERROR, "blur: failed to make EGL context current");
    return false;
  }

  blur_ctx.prog_kawase = link_program(blur_kawase_frag_src);
  blur_ctx.prog_gauss_h = link_program(blur_gauss_h_frag_src);
  blur_ctx.prog_gauss_v = link_program(blur_gauss_v_frag_src);
  blur_ctx.prog_box_h = link_program(blur_box_h_frag_src);
  blur_ctx.prog_box_v = link_program(blur_box_v_frag_src);
  blur_ctx.prog_blit = link_program(blit_frag_src);
  blur_ctx.prog_mica_tint = link_program(blur_mica_frag_src);
  blur_ctx.prog_acrylic_tint = link_program(blur_acrylic_frag_src);
  blur_ctx.prog_refraction = link_program(blur_refraction_frag_src);
  blur_ctx.prog_ext_blit = link_program(ext_blit_frag_src);
  blur_ctx.prog_border = link_program(border_frag_src);
  blur_ctx.prog_corner_mask = link_program(border_corner_mask_frag_src);

  if (!blur_ctx.prog_kawase || !blur_ctx.prog_gauss_h || !blur_ctx.prog_gauss_v ||
      !blur_ctx.prog_box_h || !blur_ctx.prog_box_v || !blur_ctx.prog_blit ||
      !blur_ctx.prog_mica_tint || !blur_ctx.prog_acrylic_tint || !blur_ctx.prog_refraction) {
    wlr_log(WLR_ERROR, "blur: one or more required shaders failed to compile");
    egl_unset_current();
    return false;
  }

  blur_ctx.u_kawase.tex = glGetUniformLocation(blur_ctx.prog_kawase, "tex");
  blur_ctx.u_kawase.halfpixel = glGetUniformLocation(blur_ctx.prog_kawase, "halfpixel");
  blur_ctx.u_kawase.offset = glGetUniformLocation(blur_ctx.prog_kawase, "offset");
  blur_ctx.u_kawase.noise_strength = glGetUniformLocation(blur_ctx.prog_kawase, "noise_strength");
  blur_ctx.u_kawase.vibrancy = glGetUniformLocation(blur_ctx.prog_kawase, "vibrancy");
  blur_ctx.u_kawase.vibrancy_darkness = glGetUniformLocation(blur_ctx.prog_kawase, "vibrancy_darkness");
  blur_ctx.u_kawase.brightness = glGetUniformLocation(blur_ctx.prog_kawase, "brightness");
  blur_ctx.u_kawase.contrast = glGetUniformLocation(blur_ctx.prog_kawase, "contrast");

  blur_ctx.u_gauss.tex = glGetUniformLocation(blur_ctx.prog_gauss_h, "tex");
  blur_ctx.u_gauss.texel_size = glGetUniformLocation(blur_ctx.prog_gauss_h, "texel_size");
  blur_ctx.u_gauss.radius = glGetUniformLocation(blur_ctx.prog_gauss_h, "radius");
  blur_ctx.u_gauss.vibrancy = glGetUniformLocation(blur_ctx.prog_gauss_h, "vibrancy");
  blur_ctx.u_gauss.vibrancy_darkness = glGetUniformLocation(blur_ctx.prog_gauss_h, "vibrancy_darkness");
  blur_ctx.u_gauss.brightness = glGetUniformLocation(blur_ctx.prog_gauss_h, "brightness");
  blur_ctx.u_gauss.contrast = glGetUniformLocation(blur_ctx.prog_gauss_h, "contrast");

  blur_ctx.u_box.tex = glGetUniformLocation(blur_ctx.prog_box_h, "tex");
  blur_ctx.u_box.texel_size = glGetUniformLocation(blur_ctx.prog_box_h, "texel_size");
  blur_ctx.u_box.radius = glGetUniformLocation(blur_ctx.prog_box_h, "radius");
  blur_ctx.u_box.vibrancy = glGetUniformLocation(blur_ctx.prog_box_h, "vibrancy");
  blur_ctx.u_box.vibrancy_darkness = glGetUniformLocation(blur_ctx.prog_box_h, "vibrancy_darkness");
  blur_ctx.u_box.brightness = glGetUniformLocation(blur_ctx.prog_box_h, "brightness");
  blur_ctx.u_box.contrast = glGetUniformLocation(blur_ctx.prog_box_h, "contrast");

  blur_ctx.u_blit.tex = glGetUniformLocation(blur_ctx.prog_blit, "tex");

  if (blur_ctx.prog_ext_blit)
    blur_ctx.u_ext_blit.tex = glGetUniformLocation(blur_ctx.prog_ext_blit, "tex");

  blur_ctx.u_mica.tex = glGetUniformLocation(blur_ctx.prog_mica_tint, "tex");
  blur_ctx.u_mica.tint = glGetUniformLocation(blur_ctx.prog_mica_tint, "tint");
  blur_ctx.u_mica.tint_strength = glGetUniformLocation(blur_ctx.prog_mica_tint, "tint_strength");

  blur_ctx.u_acrylic.tex = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "tex");
  blur_ctx.u_acrylic.tint = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "tint");
  blur_ctx.u_acrylic.tint_strength = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "tint_strength");
  blur_ctx.u_acrylic.noise_strength = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "noise_strength");
  blur_ctx.u_acrylic.resolution = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "resolution");
  blur_ctx.u_acrylic.light_anchor = glGetUniformLocation(blur_ctx.prog_acrylic_tint, "light_anchor");

  blur_ctx.u_refraction.tex = glGetUniformLocation(blur_ctx.prog_refraction, "tex");
  blur_ctx.u_refraction.offset = glGetUniformLocation(blur_ctx.prog_refraction, "offset");
  blur_ctx.u_refraction.halfpixel = glGetUniformLocation(blur_ctx.prog_refraction, "halfpixel");
  blur_ctx.u_refraction.refraction_rect_size = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_rect_size");
  blur_ctx.u_refraction.refraction_edge_size_pixels = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_edge_size_pixels");
  blur_ctx.u_refraction.refraction_corner_radius_pixels = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_corner_radius_pixels");
  blur_ctx.u_refraction.refraction_strength = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_strength");
  blur_ctx.u_refraction.refraction_normal_pow = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_normal_pow");
  blur_ctx.u_refraction.refraction_RGB_fringing = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_RGB_fringing");
  blur_ctx.u_refraction.refraction_texture_repeat_mode = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_texture_repeat_mode");
  blur_ctx.u_refraction.refraction_mode = glGetUniformLocation(blur_ctx.prog_refraction, "refraction_mode");

  if (blur_ctx.prog_border) {
    blur_ctx.u_border.resolution = glGetUniformLocation(blur_ctx.prog_border, "resolution");
    blur_ctx.u_border.border_radius = glGetUniformLocation(blur_ctx.prog_border, "border_radius");
    blur_ctx.u_border.border_width_px = glGetUniformLocation(blur_ctx.prog_border, "border_width_px");
    blur_ctx.u_border.border_color = glGetUniformLocation(blur_ctx.prog_border, "border_color");
    blur_ctx.u_border.gradient_colors  = glGetUniformLocation(blur_ctx.prog_border, "gradient_colors");
    blur_ctx.u_border.gradient_count = glGetUniformLocation(blur_ctx.prog_border, "gradient_count");
    blur_ctx.u_border.gradient_angle = glGetUniformLocation(blur_ctx.prog_border, "gradient_angle");
    blur_ctx.u_border.gradient2_colors = glGetUniformLocation(blur_ctx.prog_border, "gradient2_colors");
    blur_ctx.u_border.gradient2_count = glGetUniformLocation(blur_ctx.prog_border, "gradient2_count");
    blur_ctx.u_border.gradient2_angle = glGetUniformLocation(blur_ctx.prog_border, "gradient2_angle");
    blur_ctx.u_border.gradient_lerp = glGetUniformLocation(blur_ctx.prog_border, "gradient_lerp");
  }
  if (blur_ctx.prog_corner_mask) {
    blur_ctx.u_corner_mask.tex = glGetUniformLocation(blur_ctx.prog_corner_mask, "tex");
    blur_ctx.u_corner_mask.win_pos_uv = glGetUniformLocation(blur_ctx.prog_corner_mask, "win_pos_uv");
    blur_ctx.u_corner_mask.win_size_uv = glGetUniformLocation(blur_ctx.prog_corner_mask, "win_size_uv");
    blur_ctx.u_corner_mask.win_size_px = glGetUniformLocation(blur_ctx.prog_corner_mask, "win_size_px");
    blur_ctx.u_corner_mask.border_radius_px = glGetUniformLocation(blur_ctx.prog_corner_mask, "border_radius_px");
  }

  blur_ctx.attr_pos = 0;

  static const float quad[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,
    -1.0f,  1.0f,   1.0f,  1.0f,
  };
  glGenBuffers(1, &blur_ctx.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, blur_ctx.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  egl_unset_current();
  blur_ctx.available = true;
  wlr_log(WLR_INFO, "blur: initialised (GLES2)");
  return true;
}

void blur_fini(void) {
  if (!blur_ctx.available) return;
  egl_make_current();
  glDeleteProgram(blur_ctx.prog_kawase);
  glDeleteProgram(blur_ctx.prog_gauss_h);
  glDeleteProgram(blur_ctx.prog_gauss_v);
  glDeleteProgram(blur_ctx.prog_box_h);
  glDeleteProgram(blur_ctx.prog_box_v);
  glDeleteProgram(blur_ctx.prog_blit);
  glDeleteProgram(blur_ctx.prog_mica_tint);
  glDeleteProgram(blur_ctx.prog_acrylic_tint);
  glDeleteProgram(blur_ctx.prog_refraction);
  if (blur_ctx.prog_ext_blit)
    glDeleteProgram(blur_ctx.prog_ext_blit);
  if (blur_ctx.prog_border)
    glDeleteProgram(blur_ctx.prog_border);
  if (blur_ctx.prog_corner_mask)
    glDeleteProgram(blur_ctx.prog_corner_mask);
  if (screen_shader_prog)
    glDeleteProgram(screen_shader_prog);
  glDeleteBuffers(1, &blur_ctx.vbo);
  egl_unset_current();
  screen_shader_prog = 0;
  blur_ctx = (blur_ctx_t){0};
}

blur_output_ctx_t *blur_output_init(int width, int height) {
  if (!blur_ctx.available) return NULL;

  blur_output_ctx_t *ctx = calloc(1, sizeof(*ctx));
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

void blur_output_fini(blur_output_ctx_t *ctx) {
  if (!ctx) return;
  if (blur_ctx.available) {
    egl_make_current();
    destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
    destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
    destroy_fbo(&ctx->screen_fbo, &ctx->screen_tex);
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

void blur_output_resize(blur_output_ctx_t *ctx, int width, int height,
    output_t *output) {
  if (!ctx || !blur_ctx.available) return;
  int ds = blur_downsample > 0 ? blur_downsample : 1;
  int new_bw = (width  / ds) > 0 ? (width  / ds) : 1;
  int new_bh = (height / ds) > 0 ? (height / ds) : 1;
  if (ctx->width == width && ctx->height == height &&
      ctx->blur_w == new_bw && ctx->blur_h == new_bh) return;

  egl_make_current();
  destroy_fbo(&ctx->fbo[0], &ctx->tex[0]);
  destroy_fbo(&ctx->fbo[1], &ctx->tex[1]);
  destroy_fbo(&ctx->screen_fbo, &ctx->screen_tex);
  ctx->width  = width;
  ctx->height = height;
  ctx->blur_w = new_bw;
  ctx->blur_h = new_bh;
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[0], &ctx->tex[0]);
  create_fbo(ctx->blur_w, ctx->blur_h, &ctx->fbo[1], &ctx->tex[1]);
  if (!create_fbo(width, height, &ctx->screen_fbo, &ctx->screen_tex))
    wlr_log(WLR_ERROR, "blur: screen shader FBO resize failed (non-fatal)");
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

void blur_invalidate_mica(blur_output_ctx_t *ctx) {
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
static GLuint capture_bg_to_tex1(output_t *output, blur_output_ctx_t *ctx,
    struct wlr_scene_output *real_scene_output, bool mica_only,
    struct wlr_scene_node *hide_node, bool *hide_flag) {
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

  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);

  if (ok) wlr_output_commit_state(ctx->capture_output, &cap_state);

  egl_make_current();
  glFlush();

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

  wlr_damage_ring_add_whole(&real_scene_output->damage_ring);

  if (!ok || !cap_state.buffer) {
    egl_unset_current();
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

    if (attach_type == GL_TEXTURE && attach_name > 0 && blur_ctx.prog_ext_blit) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
      glViewport(0, 0, ctx->blur_w, ctx->blur_h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
      glUseProgram(blur_ctx.prog_ext_blit);
      glUniform1i(blur_ctx.u_ext_blit.tex, 0);
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
      glUseProgram(blur_ctx.prog_blit);
      glUniform1i(blur_ctx.u_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->tex[1];
    } else if (attach_type == GL_RENDERBUFFER) {
      unsigned char *pixels = malloc((size_t)w * h * 4);
      if (pixels) {
        glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint tmp_tex;
        glGenTextures(1, &tmp_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tmp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        free(pixels);

        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo[1]);
        glViewport(0, 0, ctx->blur_w, ctx->blur_h);
        glUseProgram(blur_ctx.prog_blit);
        glUniform1i(blur_ctx.u_blit.tex, 0);
        draw_quad();
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &tmp_tex);
        result = ctx->tex[1];
      }
    }
  }

  egl_unset_current();
  wlr_output_state_finish(&cap_state);

  return result;
}

// ensure buf/fbo are allocated; returns fbo or 0 on failure
static GLuint ensure_output_buf(struct wlr_buffer **buf_out, GLuint *fbo_out,
    int w, int h) {
  if (*buf_out)
    return *fbo_out;

  const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(server.renderer);
  const struct wlr_drm_format *fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
  if (!fmt) fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
  if (!fmt) return 0;

  struct wlr_buffer *buf = wlr_allocator_create_buffer(server.allocator, w, h, fmt);
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

  const struct wlr_drm_format_set *fmts = wlr_renderer_get_render_formats(server.renderer);
  const struct wlr_drm_format *fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_ARGB8888) : NULL;
  if (!fmt) fmt = fmts ? wlr_drm_format_set_get(fmts, DRM_FORMAT_XRGB8888) : NULL;
  if (!fmt) return 0;

  struct wlr_buffer *buf = wlr_allocator_create_buffer(server.allocator, w, h, fmt);
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

static bool rebuild_live_blur(output_t *output, struct wlr_scene_output *scene_output) {
  blur_output_ctx_t *ctx = output->blur_ctx;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->blur_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    GLuint src = capture_bg_to_tex1(output, ctx, scene_output, false,
      &tl->scene_tree->node, &tl->blur->blur_scene_hidden);
    if (!src) continue;

    egl_make_current();
    GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

    GLuint dest_fbo = ensure_output_buf(&tl->blur->blur_buf, &tl->blur->blur_buf_fbo, w, h);
    if (!dest_fbo) {
      egl_unset_current();
      continue;
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
    glViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blurred);
    glUseProgram(blur_ctx.prog_blit);
    glUniform1i(blur_ctx.u_blit.tex, 0);
    draw_quad();

    client_t *c = tl->node->client;
    if (c && c->border_radius > 0.0f && blur_ctx.prog_corner_mask) {
      struct wlr_box content_r = get_client_rect(tl);
      float ow = (float)w, oh = (float)h;
      float win_u  = (float)(content_r.x - output->lx) / ow;
      float win_v  = 1.0f - (float)(content_r.y - output->ly + content_r.height) / oh;
      float win_sw = (float)content_r.width  / ow;
      float win_sh = (float)content_r.height / oh;
      int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : (int)c->border_width;
      float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

      glEnable(GL_BLEND);
      glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(blur_ctx.prog_corner_mask);
      glUniform1i(blur_ctx.u_corner_mask.tex, 0);
      glUniform2f(blur_ctx.u_corner_mask.win_pos_uv, win_u, win_v);
      glUniform2f(blur_ctx.u_corner_mask.win_size_uv, win_sw, win_sh);
      glUniform2f(blur_ctx.u_corner_mask.win_size_px, (float)content_r.width, (float)content_r.height);
      glUniform1f(blur_ctx.u_corner_mask.border_radius_px, inner_r);
      draw_quad();
      glDisable(GL_BLEND);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    egl_unset_current();
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

static bool rebuild_live_blur_layers(output_t *output, struct wlr_scene_output *scene_output) {
  blur_output_ctx_t *ctx = output->blur_ctx;
  int w = output->width, h = output->height;
  bool any = false;

  for (int i = 0; i < 4; i++) {
    layer_surface_t *ls;
    wl_list_for_each(ls, &output->layers[i], link) {
      if (!ls->blur_node || !ls->mapped) continue;

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

      GLuint src = capture_bg_to_tex1(output, ctx, scene_output, false,
        &ls->scene_tree->node, &ls->blur_scene_hidden);
      if (!src) continue;

      egl_make_current();
      GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

      GLuint dest_fbo = ensure_output_buf(&ls->blur_buf, &ls->blur_buf_fbo, w, h);
      if (!dest_fbo) {
        egl_unset_current();
        continue;
      }

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
      glUseProgram(blur_ctx.prog_blit);
      glUniform1i(blur_ctx.u_blit.tex, 0);
      for (int b = 0; b < nboxes; b++) {
        int box_x1 = boxes[b].x1 + out_lx;
        int box_y1 = boxes[b].y1 + out_ly;
        glScissor(box_x1, box_y1, boxes[b].x2 - boxes[b].x1, boxes[b].y2 - boxes[b].y1);
        draw_quad();
      }

      glDisable(GL_SCISSOR_TEST);
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glFlush();
      egl_unset_current();
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

      // Compute the source box in output-local coordinates
      // r is the blur region in layout (absolute) coordinates
      struct wlr_box r = { .x = lx + blur_r_x, .y = ly + blur_r_y, .width = blur_r_w, .height = blur_r_h };

      struct wlr_fbox src; int dw, dh;
      if (!compute_src_box(output, &r, &src, &dw, &dh)) {
        wlr_scene_buffer_set_buffer(ls->blur_node, NULL);
        wlr_scene_node_set_position(&ls->blur_node->node, 0, 0);
        continue;
      }

      int offset_x = (r.x < output->lx) ? (output->lx - r.x) : 0;
      int offset_y = (r.y < output->ly) ? (output->ly - r.y) : 0;

      // Position at blur region offset within surface
      wlr_scene_node_set_position(&ls->blur_node->node, blur_r_x + offset_x, blur_r_y + offset_y);
      wlr_scene_buffer_set_source_box(ls->blur_node, &src);
      wlr_scene_buffer_set_dest_size(ls->blur_node, dw, dh);
    }
  }
}

static bool rebuild_live_acrylic(output_t *output, struct wlr_scene_output *scene_output) {
  blur_output_ctx_t *ctx = output->blur_ctx;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->blur || !tl->blur->acrylic_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    GLuint src = capture_bg_to_tex1(output, ctx, scene_output, false,
      &tl->scene_tree->node, &tl->blur->blur_scene_hidden);
    if (!src) continue;

    GLuint dest_fbo = ensure_output_buf(&tl->blur->acrylic_buf, &tl->blur->acrylic_buf_fbo, w, h);
    if (!dest_fbo) continue;

    egl_make_current();

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
    glUseProgram(blur_ctx.prog_acrylic_tint);
    glUniform1i(blur_ctx.u_acrylic.tex, 0);
    glUniform4fv(blur_ctx.u_acrylic.tint, 1, acrylic_tint);
    glUniform1f(blur_ctx.u_acrylic.tint_strength, acrylic_tint_strength);
    glUniform1f(blur_ctx.u_acrylic.noise_strength, acrylic_noise_strength);
    glUniform2f(blur_ctx.u_acrylic.resolution, (float)w, (float)h);
    glUniform2f(blur_ctx.u_acrylic.light_anchor, acrylic_light_anchor[0], acrylic_light_anchor[1]);
    draw_quad();

    client_t *c = tl->node->client;
    if (c && c->border_radius > 0.0f && blur_ctx.prog_corner_mask) {
      struct wlr_box content_r = get_client_rect(tl);
      float ow = (float)w, oh = (float)h;
      float win_u  = (float)(content_r.x - output->lx) / ow;
      float win_v  = (float)(content_r.y - output->ly) / oh;
      float win_sw = (float)content_r.width  / ow;
      float win_sh = (float)content_r.height / oh;
      int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : (int)c->border_width;
      float inner_r = (c->border_radius > (float)bw_i) ? c->border_radius - (float)bw_i : 0.0f;

      glEnable(GL_BLEND);
      glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
      glUseProgram(blur_ctx.prog_corner_mask);
      glUniform1i(blur_ctx.u_corner_mask.tex, 0);
      glUniform2f(blur_ctx.u_corner_mask.win_pos_uv, win_u, win_v);
      glUniform2f(blur_ctx.u_corner_mask.win_size_uv, win_sw, win_sh);
      glUniform2f(blur_ctx.u_corner_mask.win_size_px, (float)content_r.width, (float)content_r.height);
      glUniform1f(blur_ctx.u_corner_mask.border_radius_px, inner_r);
      draw_quad();
      glDisable(GL_BLEND);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    egl_unset_current();
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

static bool rebuild_mica(output_t *output, struct wlr_scene_output *scene_output) {
  blur_output_ctx_t *ctx = output->blur_ctx;
  int w = output->width, h = output->height;

  GLuint src = capture_bg_to_tex1(output, ctx, scene_output, true, NULL, NULL);
  if (!src) return false;

  egl_make_current();
  GLuint blurred = apply_blur(ctx, src, ctx->blur_w, ctx->blur_h);

  GLuint dest_fbo = ensure_output_buf(&ctx->mica_buf, &ctx->mica_buf_fbo, w, h);
  if (!dest_fbo) {
    egl_unset_current();
    return false;
  }

  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_FRAMEBUFFER, dest_fbo);
  glViewport(0, 0, w, h);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, blurred);
  glUseProgram(blur_ctx.prog_mica_tint);
  glUniform1i(blur_ctx.u_mica.tex, 0);
  glUniform4fv(blur_ctx.u_mica.tint, 1, mica_tint);
  glUniform1f(blur_ctx.u_mica.tint_strength, mica_tint_strength);
  draw_quad();
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glFlush();
  egl_unset_current();

  ctx->mica_dirty = false;
  return true;
}

static void push_mica_to_toplevels(output_t *output) {
  struct wlr_buffer *buf = output->blur_ctx->mica_buf;
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

static bool blur_render_border(toplevel_t *tl, int content_w, int content_h) {
  if (!blur_ctx.prog_border) return false;
  if (!tl->border_tree) return false;
  if (!tl->rounded) return false;

  float scale = tl->node->output ? tl->node->output->wlr_output->scale : 1.0f;
  client_t *c = tl->node->client;
  int bw_i = (int)c->border_width;
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
  glUseProgram(blur_ctx.prog_border);

  glUniform2f(blur_ctx.u_border.resolution, (float)phys_fw, (float)phys_fh);
  glUniform1f(blur_ctx.u_border.border_radius, (float)c->border_radius * scale);
  glUniform1f(blur_ctx.u_border.border_width_px, (float)bw_i * scale);
  glUniform4fv(blur_ctx.u_border.border_color, 1, tl->rounded->border_color);

  // gradient uniforms
  if (blur_ctx.u_border.gradient_colors >= 0)
    glUniform4fv(blur_ctx.u_border.gradient_colors, BORDER_GRADIENT_MAX_STOPS, tl->rounded->gradient_colors);
  if (blur_ctx.u_border.gradient_count >= 0)
    glUniform1i(blur_ctx.u_border.gradient_count, tl->rounded->gradient_count);
  if (blur_ctx.u_border.gradient_angle >= 0)
    glUniform1f(blur_ctx.u_border.gradient_angle, tl->rounded->gradient_angle);
  if (blur_ctx.u_border.gradient2_colors >= 0)
    glUniform4fv(blur_ctx.u_border.gradient2_colors, BORDER_GRADIENT_MAX_STOPS, tl->rounded->gradient2_colors);
  if (blur_ctx.u_border.gradient2_count >= 0)
    glUniform1i(blur_ctx.u_border.gradient2_count, tl->rounded->gradient2_count);
  if (blur_ctx.u_border.gradient2_angle >= 0)
    glUniform1f(blur_ctx.u_border.gradient2_angle, tl->rounded->gradient2_angle);
  if (blur_ctx.u_border.gradient_lerp >= 0)
    glUniform1f(blur_ctx.u_border.gradient_lerp, tl->rounded->gradient_lerp);

  draw_quad();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_BLEND);
  glFlush();

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

static bool rebuild_corner_masks(output_t *output, struct wlr_scene_output *scene_output) {
  if (!blur_ctx.prog_corner_mask) return false;
  blur_output_ctx_t *ctx = output->blur_ctx;
  int w = output->width, h = output->height;
  bool any = false;

  toplevel_t *tl;
  wl_list_for_each(tl, &server.toplevels, link) {
    if (!tl->rounded || !tl->rounded->corner_mask_node || !tl->node || !tl->node->client) continue;
    if (!tl->node->client->shown) continue;
    if (!tl->node->output || tl->node->output != output) continue;

    client_t *c = tl->node->client;
    if (c->border_radius <= 0.0f) continue;

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

    bool hide_flag = false;
    GLuint src = capture_bg_to_tex1(output, ctx, scene_output, false,
      &tl->scene_tree->node, tl->blur ? &tl->blur->blur_scene_hidden : &hide_flag);
    if (!src) continue;

    egl_make_current();

    GLuint dest_fbo = ensure_output_buf(&tl->rounded->corner_mask_buf,
      &tl->rounded->corner_mask_buf_fbo, w, h);
    if (!dest_fbo) {
    	egl_unset_current();
    	continue;
    }

    float ow = (float)w, oh = (float)h;
    int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : (int)c->border_width;

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
    glUseProgram(blur_ctx.prog_corner_mask);
    glUniform1i(blur_ctx.u_corner_mask.tex, 0);
    glUniform2f(blur_ctx.u_corner_mask.win_pos_uv,  win_u,  win_v);
    glUniform2f(blur_ctx.u_corner_mask.win_size_uv, win_sw, win_sh);
    glUniform2f(blur_ctx.u_corner_mask.win_size_px, outer_w, outer_h);
    glUniform1f(blur_ctx.u_corner_mask.border_radius_px, c->border_radius);
    draw_quad();
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    egl_unset_current();
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
    if (c->border_radius <= 0.0f) {
      wlr_scene_buffer_set_buffer(tl->rounded->corner_mask_node, NULL);
      continue;
    }
    if (!tl->rounded->corner_mask_buf) {
      wlr_scene_node_set_enabled(&tl->rounded->corner_mask_node->node, false);
      continue;
    }

    struct wlr_box content_r = get_client_rect(tl);
    int bw_i = (c->state == STATE_FULLSCREEN) ? 0 : (int)c->border_width;

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

static GLuint capture_full_scene_to_tex(output_t *output, blur_output_ctx_t *ctx, struct wlr_scene_output *real_scene_output) {
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

  bool ok = wlr_scene_output_build_state(ctx->capture_scene_output, &cap_state, NULL);
  if (ok) wlr_output_commit_state(ctx->capture_output, &cap_state);

  egl_make_current();
  glFlush();

  if (server.shader_tree)
    wlr_scene_node_set_enabled(&server.shader_tree->node, true);

  wlr_scene_output_set_position(ctx->capture_scene_output, -0x7fff, -0x7fff);
  wlr_damage_ring_add_whole(&real_scene_output->damage_ring);

  if (!ok || !cap_state.buffer) {
    egl_unset_current();
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

    if (attach_type == GL_TEXTURE && attach_name > 0 && blur_ctx.prog_ext_blit) {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glBindFramebuffer(GL_FRAMEBUFFER, ctx->screen_fbo);
      glViewport(0, 0, w, h);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)attach_name);
      glUseProgram(blur_ctx.prog_ext_blit);
      glUniform1i(blur_ctx.u_ext_blit.tex, 0);
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
      glUseProgram(blur_ctx.prog_blit);
      glUniform1i(blur_ctx.u_blit.tex, 0);
      draw_quad();
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      result = ctx->screen_tex;
    } else if (attach_type == GL_RENDERBUFFER) {
      unsigned char *pixels = malloc((size_t)w * h * 4);
      if (pixels) {
        glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint tmp_tex;
        glGenTextures(1, &tmp_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tmp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        free(pixels);

        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->screen_fbo);
        glViewport(0, 0, w, h);
        glUseProgram(blur_ctx.prog_blit);
        glUniform1i(blur_ctx.u_blit.tex, 0);
        draw_quad();
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &tmp_tex);
        result = ctx->screen_tex;
      }
    }
  }

  egl_unset_current();
  wlr_output_state_finish(&cap_state);
  return result;
}

static void do_screen_shader_frame(output_t *output, struct wlr_scene_output *scene_output) {
  blur_output_ctx_t *ctx = output->blur_ctx;
  if (!ctx || !ctx->screen_shader_node) return;

  if (!screen_shader_enabled || !screen_shader_prog) {
    wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
    return;
  }

  int w = output->width, h = output->height;
  if (w <= 0 || h <= 0) return;

  GLuint src = capture_full_scene_to_tex(output, ctx, scene_output);
  if (!src) {
    wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, false);
    return;
  }

  egl_make_current();

  GLuint dest_fbo = ensure_output_buf(&ctx->screen_shader_buf, &ctx->screen_shader_buf_fbo, w, h);
  if (!dest_fbo) {
    egl_unset_current();
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
  glFlush();

  egl_unset_current();

  wlr_scene_buffer_set_buffer(ctx->screen_shader_node, ctx->screen_shader_buf);
  struct wlr_fbox src_box = {0, 0, (double)w, (double)h};
  wlr_scene_buffer_set_source_box(ctx->screen_shader_node, &src_box);
  wlr_scene_buffer_set_dest_size(ctx->screen_shader_node, w, h);
  wlr_scene_node_set_position(&ctx->screen_shader_node->node, output->lx, output->ly);
  wlr_scene_node_set_enabled(&ctx->screen_shader_node->node, true);
}

void blur_output_frame(output_t *output, struct wlr_scene_output *scene_output) {
  if (!blur_ctx.available) return;
  blur_output_ctx_t *ctx = output->blur_ctx;
  if (!ctx) return;

  // skip expensive blur rebuilds during workspace slide animation
  if (animation_workspace_switch_active()) return;

  if (ctx->width != output->width || ctx->height != output->height)
    blur_output_resize(ctx, output->width, output->height, output);

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
    if (!any_blur) {
      for (int i = 0; i < 4 && !any_blur; i++) {
        layer_surface_t *ls;
        wl_list_for_each(ls, &output->layers[i], link) {
          if (ls->blur_node && ls->mapped) {
            any_blur = true;
            break;
          }
        }
      }
    }
    if (any_blur) {
      rebuild_live_blur(output, scene_output);
      push_blur_to_toplevels(output);
      rebuild_live_blur_layers(output, scene_output);
      push_blur_to_layers(output);
    }
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
      rebuild_live_acrylic(output, scene_output);
      push_acrylic_to_toplevels(output);
    }
  }

  if (mica_enabled && ctx->mica_dirty)
    rebuild_mica(output, scene_output);

  if (mica_enabled && ctx->mica_buf)
    push_mica_to_toplevels(output);

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
      egl_make_current();
      blur_render_border(tl, content_r.width, content_r.height);
      egl_unset_current();
      tl->rounded->border_dirty = false;
    }
  }

  // corner mask render
  {
    bool any_cm = false;
    toplevel_t *tl;
    wl_list_for_each(tl, &server.toplevels, link) {
      if (tl->rounded && tl->rounded->corner_mask_node && tl->node && tl->node->client &&
          tl->node->client->border_radius > 0.0f &&
          tl->node->output && tl->node->output == output) {
        any_cm = true;
        break;
      }
    }
    if (any_cm) {
      rebuild_corner_masks(output, scene_output);
      push_corner_masks_to_toplevels(output);
    }
  }

  do_screen_shader_frame(output, scene_output);
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

const char *blur_algorithm_to_str(enum blur_algorithm algo) {
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
  if (!blur_ctx.available) return false;
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
  if (!blur_ctx.available || !path) return false;

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
  if (screen_shader_prog && blur_ctx.available) {
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
