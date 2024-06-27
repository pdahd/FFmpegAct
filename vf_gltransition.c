/**
 * FFmpeg filter for applying GLSL transitions between video streams.
 *
 * @see https://gl-transitions.com/
 */

#include "libavutil/opt.h"
#include "internal.h"
#include "framesync.h"
#include "libavfilter/formats.h"
#include "libavfilter/avfilter.h"
#include "libavutil/log.h"

#define GL_TRANSITION_USING_EGL

#ifdef GL_TRANSITION_USING_EGL
# include <EGL/egl.h>
# include <EGL/eglext.h>
# include <GLES2/gl2.h>
# include <GLES2/gl2ext.h>
#else
# error "This filter only supports EGL and OpenGL ES 2.0."
#endif

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define FROM (0)
#define TO   (1)

#define PIXEL_FORMAT (GL_RGB)

static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
};

static const float position[12] = {
  -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f
};

static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "varying vec2 _uv;\n"
  "void main(void) {\n"
  "  gl_Position = vec4(position, 0.0, 1.0);\n"
  "  vec2 uv = position * 0.5 + 0.5;\n"
  "  _uv = vec2(uv.x, 1.0 - uv.y);\n"
  "}\n";

static const GLchar *f_shader_template =
  "precision mediump float;\n" // OpenGL ES 需要精度限定符
  "varying vec2 _uv;\n"
  "uniform sampler2D from;\n"
  "uniform sampler2D to;\n"
  "uniform float progress;\n"
  "uniform float ratio;\n"
  "uniform float _fromR;\n"
  "uniform float _toR;\n"
  "\n"
  "vec4 getFromColor(vec2 uv) {\n"
  "  return texture2D(from, vec2(uv.x, 1.0 - uv.y));\n"
  "}\n"
  "\n"
  "vec4 getToColor(vec2 uv) {\n"
  "  return texture2D(to, vec2(uv.x, 1.0 - uv.y));\n"
  "}\n"
  "\n"
  "\n%s\n"
  "void main() {\n"
  "  gl_FragColor = transition(_uv);\n"
  "}\n";

// default to a basic fade effect
static const GLchar *f_default_transition_source =
  "vec4 transition (vec2 uv) {\n"
  "  return mix(\n"
  "    getFromColor(uv),\n"
  "    getToColor(uv),\n"
  "    progress\n"
  "  );\n"
  "}\n";

typedef struct {
  const AVClass *class;
  FFFrameSync fs;

  // input options
  double duration;
  double offset;
  char *source;

  // timestamp of the first frame in the output, in the timebase units
  int64_t first_pts;

  // uniforms
  GLuint        from;
  GLuint        to;
  GLint         progress;
  GLint         ratio;
  GLint         _fromR;
  GLint         _toR;

  // internal state
  GLuint        posBuf;
  GLuint        program;
#ifdef GL_TRANSITION_USING_EGL
  EGLDisplay eglDpy;
  EGLConfig eglCfg;
  EGLSurface eglSurf;
  EGLContext eglCtx;
#endif

  GLchar *f_shader_source;
} GLTransitionContext;

#define OFFSET(x) offsetof(GLTransitionContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption gltransition_options[] = {
  { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
  { "offset", "delay before starting transition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
  { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
  {NULL}
};

FRAMESYNC_DEFINE_CLASS(gltransition, GLTransitionContext, fs);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type)
{
  GLuint shader = glCreateShader(type);
  if (!shader) {
    av_log(ctx, AV_LOG_ERROR, "Failed to create shader (type: %d)\n", type);
    return 0;
  }

  glShaderSource(shader, 1, &shader_source, NULL);
  glCompileShader(shader);

  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    GLint logLength;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0) {
      GLchar *log = (GLchar *)malloc(logLength);
      glGetShaderInfoLog(shader, logLength, &logLength, log);
      av_log(ctx, AV_LOG_ERROR, "Shader compilation failed:\n%s\n", log);
      free(log);
    }
    glDeleteShader(shader);
    return 0;
  }

  av_log(ctx, AV_LOG_DEBUG, "Shader compilation successful (type: %d)\n", type);
  return shader;
}

static int build_program(AVFilterContext *ctx)
{
  GLuint v_shader, f_shader;
  GLTransitionContext *c = ctx->priv;
  char *source = NULL;
  GLint status;
  const char *transition_source;
  int len;
  unsigned long fsize;

  av_log(ctx, AV_LOG_DEBUG, "Building GL program...\n");

  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "Invalid vertex shader\n");
    return -1;
  }

  if (c->source) {
    FILE *f = fopen(c->source, "rb");
    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "Invalid transition source file \"%s\"\n", c->source);
      return -1;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    source = malloc(fsize + 1);
    if (!source) {
      av_log(ctx, AV_LOG_ERROR, "Failed to allocate memory for transition source\n");
      fclose(f);
      return AVERROR(ENOMEM);
    }
    fread(source, fsize, 1, f);
    fclose(f);
    source[fsize] = 0;
  }

  transition_source = source ? source : f_default_transition_source;
  len = strlen(f_shader_template) + strlen(transition_source);
  c->f_shader_source = av_calloc(len + 1, sizeof(*c->f_shader_source));
  if (!c->f_shader_source) {
    free(source);
    return AVERROR(ENOMEM);
  }
  snprintf(c->f_shader_source, len * sizeof(*c->f_shader_source), f_shader_template, transition_source);
  av_log(ctx, AV_LOG_DEBUG, "Fragment shader source:\n%s\n", c->f_shader_source);

  if (source) {
    free(source);
  }

  if (!(f_shader = build_shader(ctx, c->f_shader_source, GL_FRAGMENT_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "Invalid fragment shader\n");
    return -1;
  }

  c->program = glCreateProgram();
  if (!c->program) {
    av_log(ctx, AV_LOG_ERROR, "Failed to create GL program\n");
    return -1;
  }

  glAttachShader(c->program, v_shader);
  glAttachShader(c->program, f_shader);
  glLinkProgram(c->program);

  glGetProgramiv(c->program, GL_LINK_STATUS, &status);
  if (!status) {
    GLint logLength;
    glGetProgramiv(c->program, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0) {
      GLchar *log = (GLchar *)malloc(logLength);
      glGetProgramInfoLog(c->program, logLength, &logLength, log);
      av_log(ctx, AV_LOG_ERROR, "Program linking failed:\n%s\n", log);
      free(log);
    }
    glDeleteProgram(c->program);
    return -1;
  }

  av_log(ctx, AV_LOG_DEBUG, "GL program built successfully\n");
  return 0;
}

static void setup_vbo(GLTransitionContext *c)
{
  GLint loc = glGetAttribLocation(c->program, "position");
  glGenBuffers(1, &c->posBuf);
  glBindBuffer(GL_ARRAY_BUFFER, c->posBuf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);
  glEnableVertexAttribArray(loc);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
  av_log(c, AV_LOG_DEBUG, "VBO setup complete\n");
}

static void setup_tex(AVFilterLink *fromLink)
{
  AVFilterContext     *ctx = fromLink->dst;
  GLTransitionContext *c = ctx->priv;
  { // from
    glGenTextures(1, &c->from);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->from);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);
    glUniform1i(glGetUniformLocation(c->program, "from"), 0);
    av_log(ctx, AV_LOG_DEBUG, "Texture 'from' setup complete\n");
  }
  { // to
    glGenTextures(1, &c->to);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, c->to);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);
    glUniform1i(glGetUniformLocation(c->program, "to"), 1);
    av_log(ctx, AV_LOG_DEBUG, "Texture 'to' setup complete\n");
  }
}

static int init_gl(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  EGLint numConfigs;

  static const EGLint contextAttribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };

  av_log(ctx, AV_LOG_DEBUG, "Initializing GL...\n");

  c->eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (c->eglDpy == EGL_NO_DISPLAY) {
    av_log(ctx, AV_LOG_ERROR, "Failed to get EGL display\n");
    return -1;
  }

  if (!eglInitialize(c->eglDpy, NULL, NULL)) {
    av_log(ctx, AV_LOG_ERROR, "Failed to initialize EGL\n");
    return -1;
  }

  if (!eglChooseConfig(c->eglDpy, configAttribs, &c->eglCfg, 1, &numConfigs)) {
    av_log(ctx, AV_LOG_ERROR, "Failed to choose EGL config\n");
    return -1;
  }

  EGLint pbufferAttribs[] = {
      EGL_WIDTH,
      1,
      EGL_HEIGHT,
      1,
      EGL_NONE,
  };

  c->eglSurf = eglCreatePbufferSurface(c->eglDpy, c->eglCfg,
                                       pbufferAttribs);
  if (c->eglSurf == EGL_NO_SURFACE) {
    av_log(ctx, AV_LOG_ERROR, "Failed to create EGL surface\n");
    return -1;
  }

  c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, contextAttribs);
  if (c->eglCtx == EGL_NO_CONTEXT) {
    av_log(ctx, AV_LOG_ERROR, "Failed to create EGL context\n");
    return -1;
  }

  if (!eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx)) {
    av_log(ctx, AV_LOG_ERROR, "Failed to make EGL context current\n");
    return -1;
  }

  av_log(ctx, AV_LOG_DEBUG, "GL initialized successfully\n");

  if (build_program(ctx) < 0) {
    av_log(ctx, AV_LOG_ERROR, "Failed to build GL program\n");
    return -1;
  }

  setup_vbo(c);
  return 0;
}

static int config_input(AVFilterLink *inlink)
{
  AVFilterContext *ctx = inlink->dst;
  GLTransitionContext *c = ctx->priv;
  int ret;

  av_log(ctx, AV_LOG_DEBUG, "Configuring input...\n");

  if ((ret = ff_framesync_configure(&c->fs)) < 0) {
    av_log(ctx, AV_LOG_ERROR, "Failed to configure frame sync: %s\n", av_err2str(ret));
    return ret;
  }

  if (init_gl(ctx) < 0) {
    av_log(ctx, AV_LOG_ERROR, "Failed to initialize GL\n");
    return AVERROR(EINVAL);
  }

  setup_tex(inlink);

  av_log(ctx, AV_LOG_DEBUG, "Input configured successfully\n");
  return 0;
}

static AVFrame *apply_transition(FFFrameSync *fs,
                                 AVFilterContext *ctx,
                                 AVFrame *fromFrame,
                                 const AVFrame *toFrame)
{
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  AVFilterLink *outLink = ctx->outputs[0];
  AVFrame *outFrame;

  outFrame = ff_get_video_buffer(outLink, outLink->w, outLink->h);
  if (!outFrame) {
    return NULL;
  }

  av_frame_copy_props(outFrame, fromFrame);

#ifdef GL_TRANSITION_USING_EGL
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#endif

  glUseProgram(c->program);

  const float ts = ((fs->pts - c->first_pts) / (float)fs->time_base.den) - c->offset;
  const float progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
  glUniform1f(c->progress, progress);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->from);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, fromFrame->linesize[0] / 3);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fromLink->w, fromLink->h, GL_RGB, GL_UNSIGNED_BYTE, fromFrame->data[0]);

  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, c->to);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, toFrame->linesize[0] / 3);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, toLink->w, toLink->h, GL_RGB, GL_UNSIGNED_BYTE, toFrame->data[0]);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  // 读取渲染结果到 CPU 内存
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, outFrame->linesize[0] / 3);
  glReadPixels(0, 0, outLink->w, outLink->h, GL_RGB, GL_UNSIGNED_BYTE, outFrame->data[0]);

  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  av_frame_free(&fromFrame);

  return outFrame;
}

static int blend_frame(FFFrameSync *fs)
{
  AVFilterContext *ctx = fs->parent;
  GLTransitionContext *c = ctx->priv;

  AVFrame *fromFrame, *toFrame, *outFrame;
  int ret;

  ret = ff_framesync_dualinput_get(fs, &fromFrame, &toFrame);
  if (ret < 0) {
    return ret;
  }

  if (c->first_pts == AV_NOPTS_VALUE && fromFrame && fromFrame->pts != AV_NOPTS_VALUE) {
    c->first_pts = fromFrame->pts;
  }

  if (!toFrame) {
    return ff_filter_frame(ctx->outputs[0], fromFrame);
  }

  outFrame = apply_transition(fs, ctx, fromFrame, toFrame);
  if (!outFrame) {
    return AVERROR(ENOMEM);
  }

  return ff_filter_frame(ctx->outputs[0], outFrame);
}

static av_cold int init(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  c->fs.on_event = blend_frame;
  c->first_pts = AV_NOPTS_VALUE;

  return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
  GLTransitionContext *c = ctx->priv;
  ff_framesync_uninit(&c->fs);

#ifdef GL_TRANSITION_USING_EGL
  if (c->eglDpy) {
    glDeleteTextures(1, &c->from);
    glDeleteTextures(1, &c->to);
    glDeleteBuffers(1, &c->posBuf);
    glDeleteProgram(c->program);
    eglMakeCurrent(c->eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(c->eglDpy, c->eglCtx);
    eglDestroySurface(c->eglDpy, c->eglSurf);
    eglTerminate(c->eglDpy);
  }
#endif

  if (c->f_shader_source) {
    av_freep(&c->f_shader_source);
  }
}

static int query_formats(AVFilterContext *ctx)
{
  static const enum AVPixelFormat formats[] = {
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_NONE
  };

  return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static int activate(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  int ret;

  av_log(ctx, AV_LOG_DEBUG, "Activating filter...\n");
  ret = ff_framesync_activate(&c->fs);
  if (ret >= 0) {
    av_log(ctx, AV_LOG_DEBUG, "Filter activated successfully\n");
  } else {
    av_log(ctx, AV_LOG_ERROR, "Failed to activate filter: %s\n", av_err2str(ret));
  }
  return ret;
}

static int config_output(AVFilterLink *outLink)
{
  AVFilterContext *ctx = outLink->src;
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  int ret;

  if (fromLink->format != toLink->format) {
    av_log(ctx, AV_LOG_ERROR, "inputs must be of same pixel format\n");
    return AVERROR(EINVAL);
  }

  if (fromLink->w != toLink->w || fromLink->h != toLink->h) {
    av_log(ctx, AV_LOG_ERROR, "First input link %s parameters "
           "(size %dx%d) do not match the corresponding "
           "second input link %s parameters (size %dx%d)\n",
           ctx->input_pads[FROM].name, fromLink->w, fromLink->h,
           ctx->input_pads[TO].name, toLink->w, toLink->h);
    return AVERROR(EINVAL);
  }

  outLink->w = fromLink->w;
  outLink->h = fromLink->h;
  outLink->frame_rate = fromLink->frame_rate;

  if ((ret = ff_framesync_init_dualinput(&c->fs, ctx)) < 0) {
    return ret;
  }

  return ff_framesync_configure(&c->fs);
}

static const AVFilterPad gltransition_inputs[] = {
  {
    .name = "from",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_input,
  },
  {
    .name = "to",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_input,
  },
  {NULL}
};

static const AVFilterPad gltransition_outputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_output,
  },
  {NULL}
};

AVFilter ff_vf_gltransition = {
  .name          = "gltransition",
  .description   = NULL_IF_CONFIG_SMALL("Apply GLSL transitions between video streams."),
  .priv_size     = sizeof(GLTransitionContext),
  .preinit       = gltransition_framesync_preinit,
  .init          = init,
  .uninit        = uninit,
  .query_formats = query_formats,
  .activate      = activate,
  .inputs        = gltransition_inputs,
  .outputs       = gltransition_outputs,
  .priv_class    = &gltransition_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC
};
