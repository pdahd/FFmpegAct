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

#define GL_TRANSITION_USING_EGL

#ifdef GL_TRANSITION_USING_EGL
# include <EGL/egl.h>
# include <EGL/eglext.h>
# include <GLES2/gl2.h>
#else
# error "This filter only supports EGL and OpenGL ES2."
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

static const EGLint pbufferAttribs[] = {
    EGL_WIDTH, 1,
    EGL_HEIGHT, 1,
    EGL_NONE,
};

static const float position[12] = {
  -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f
};

static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "varying vec2 _uv;\n"
  "void main(void) {\n"
  "  gl_Position = vec4(position, 0, 1);\n"
  "  vec2 uv = position * 0.5 + 0.5;\n"
  "  _uv = vec2(uv.x, 1.0 - uv.y);\n"
  "}\n";

static const GLchar *f_shader_template =
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
  EGLDisplay eglDpy;
  EGLConfig eglCfg;
  EGLSurface eglSurf;
  EGLContext eglCtx;
  GLchar *f_shader_source;
} GLTransitionContext;

#define OFFSET(x) offsetof(GLTransitionContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption gltransition_options[] = {
  { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
  { "offset", "delay before startingtransition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
  { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
  {NULL}
};

FRAMESYNC_DEFINE_CLASS(gltransition, GLTransitionContext, fs);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type)
{
    GLuint shader = glCreateShader(type);
    if (!shader || !glIsShader(shader)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create shader.\n");
        return 0;
    }

    glShaderSource(shader, 1, &shader_source, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char buffer[512];
        glGetShaderInfoLog(shader, 512, NULL, buffer);
        av_log(ctx, AV_LOG_ERROR, "Shader compile error: %s\n", buffer);
        glDeleteShader(shader); // Don't forget to delete the shader if it failed to compile
        return 0;
    }

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

  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "invalid vertex shader\n");
    return -1;
  }

  if (c->source) {
    FILE *f = fopen(c->source, "rb");
    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "invalid transition source file \"%s\"\n", c->source);
      return -1;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    source = malloc(fsize + 1);
    fread(source, fsize, 1, f);
    fclose(f);
    source[fsize] = 0;
  }

  transition_source = source ? source : f_default_transition_source;
  len = strlen(f_shader_template) + strlen(transition_source);
  c->f_shader_source = av_calloc(len, sizeof(*c->f_shader_source));
  if (!c->f_shader_source) {
    return AVERROR(ENOMEM);
  }
  snprintf(c->f_shader_source, len * sizeof(*c->f_shader_source), f_shader_template, transition_source);
  av_log(ctx, AV_LOG_DEBUG, "\n%s\n", c->f_shader_source);

  if (source) {
    free(source);
    source = NULL;
  }

  if (!(f_shader = build_shader(ctx, c->f_shader_source, GL_FRAGMENT_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "invalid fragment shader\n");
    return -1;
  }

  c->program = glCreateProgram();
  glAttachShader(c->program, v_shader);
  glAttachShader(c->program, f_shader);
  glLinkProgram(c->program);
  glGetProgramiv(c->program, GL_LINK_STATUS, &status);
  return status == GL_TRUE ? 0 : -1;
}

static void setup_vbo(GLTransitionContext *c)
{
  GLint loc = glGetAttribLocation(c->program, "position");
  glGenBuffers(1, &c->posBuf);
  glBindBuffer(GL_ARRAY_BUFFER, c->posBuf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);
  glEnableVertexAttribArray(loc);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
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

    // 获取 EGL 显示
    c->eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (c->eglDpy == EGL_NO_DISPLAY) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get EGL display\n");
        return -1;
    }

    // 初始化 EGL
    if (!eglInitialize(c->eglDpy, NULL, NULL)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize EGL\n");
        return -1;
    }

    // 选择 EGL 配置
    if (!eglChooseConfig(c->eglDpy, configAttribs, &c->eglCfg, 1, &numConfigs)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to choose EGL config\n");
        return -1;
    }

    // 创建 EGL 表面
    c->eglSurf = eglCreatePbufferSurface(c->eglDpy, c->eglCfg, pbufferAttribs);
    if (c->eglSurf == EGL_NO_SURFACE) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create EGL surface\n");
        return -1;
    }

    // 创建 EGL 上下文
    c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, contextAttribs);
    if (c->eglCtx == EGL_NO_CONTEXT) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create EGL context\n");
        return -1;
    }

    // 激活 EGL 上下文
    if (!eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to make EGL context current\n");
        return -1;
    }

    // 构建 GL 程序
    if (build_program(ctx) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to build GL program\n");
        return -1;
    }

    // 设置 VBO
    setup_vbo(c);
    return 0;
}

static int config_input(AVFilterLink *inlink)
{
  AVFilterContext *ctx = inlink->dst;
  GLTransitionContext *c = ctx->priv;
  int ret;

  if ((ret = ff_framesync_configure(&c->fs)) < 0)
    return ret;

  if (init_gl(ctx) < 0)
    return AVERROR(EINVAL);

  setup_tex(inlink);
  return 0;
}

static int filter_frame(FFFrameSync *fs, AVFrame **out, int index)
{
    AVFilterContext *ctx = fs->parent;
    GLTransitionContext *c = ctx->priv;
    AVFrame *in;
    int ret = ff_framesync_get_frame(fs, index, &in, 0);
    if (ret < 0)
        return ret;

    if (!*out) {
        *out = ff_get_video_buffer(ctx->outputs[0], ctx->outputs[0]->w, ctx->outputs[0]->h);
        if (!*out)
            return AVERROR(ENOMEM);
        av_frame_copy_props(*out, in);
    }

    // 确保变量声明在代码块的开始处
    GLint loc;
    float progress;
    float ratio;

    // 激活着色器程序
    glUseProgram(c->program);

    // 更新 uniform 变量
    // 注意：这里需要您根据实际情况计算 progress
    progress = /* 计算进度 */;
    glUniform1f(c->progress, progress);
    ratio = (float)(*out)->width / (float)(*out)->height;
    glUniform1f(c->ratio, ratio);
    glUniform1f(c->_fromR, 1.0f); // 这里可能需要根据实际情况调整
    glUniform1f(c->_toR, 1.0f); // 这里可能需要根据实际情况调整

    // 更新纹理数据
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->from);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in->width, in->height, PIXEL_FORMAT, GL_UNSIGNED_BYTE, in->data[0]);

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, c->to);
    // 注意：这里假设 "to" 纹理已经在之前的某个步骤中设置好了

    // 执行绘制命令
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // 从 OpenGL ES 读取渲染结果到输出帧
    glReadPixels(0, 0, (*out)->width, (*out)->height, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (*out)->data[0]);

    // 将渲染好的帧发送到下一个滤镜
    ret = ff_filter_frame(ctx->outputs[0], *out);
    *out = NULL; // 确保 *out 被重置，避免重复使用

    return ret;
}

static int filter_frame_event(FFFrameSync *fs)
{
  AVFilterContext *ctx = fs->parent;
  AVFrame *out;
  int ret = filter_frame(fs, &out, 0);
  if (ret < 0)
    return ret;
  return ff_filter_frame(ctx->outputs[0], out);
}

static av_cold int init(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  ff_framesync_init(&c->fs, ctx, 2);
  c->fs.on_event = filter_frame_event;
  return 0;
}

static c const AVFilterPad gltransition_inputs[] = {
  {
    .name         = "from",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = config_input,
  },
  {
    .name         = "to",
    .type         = AVMEDIA_TYPE_VIDEO,
    .config_props = config_input,
  },
  { NULL }
};

static const AVFilterPad gltransition_outputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
  },
  { NULL }
};

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
  return ff_framesync_activate(&c->fs);
}

AVFilter ff_vf_gltransition = {
  .name          = "gltransition",
  .description   = NULL_IF_CONFIG_SMALL("Apply GLSL transitions between video streams."),
  .priv_size     = sizeof(GLTransitionContext),
  .init          = init,
  .uninit        = uninit,
  .formats.query_func    = query_formats,
  .activate      = activate,
  .inputs        = gltransition_inputs,
  .outputs       = gltransition_outputs,
  .priv_class    = &gltransition_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
