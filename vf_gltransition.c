/**
 * FFmpeg filter for applying GLSL transitions between video streams.
 *
 * @see https://gl-transitions.com/
 */

#include "libavutil/opt.h"
#include "internal.h"
#include "framesync.h"
#include <GLES2/gl2.h>
#include "avfilter.h"

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define FROM (0)
#define TO   (1)

#define PIXEL_FORMAT (GL_RGB)

// 移除所有与 EGL 和 GLFW 相关的代码

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
    return 0;
  }

  glShaderSource(shader, 1, &shader_source, 0);
  glCompileShader(shader);

  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  return (status == GL_TRUE ? shader : 0);
}

static int build_program(AVFilterContext *ctx)
{
  GLuint v_shader, f_shader;
  GLTransitionContext *c = ctx->priv;

  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
    av_log(ctx, AV_LOG_ERROR, "invalid vertex shader\n");
    return -1;
  }

  char *source = NULL;

  if (c->source) {
    FILE *f = fopen(c->source, "rb");

    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "invalid transition source file \"%s\"\n", c->source);
      return -1;
    }

    fseek(f, 0, SEEK_END);
    unsigned long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    source = malloc(fsize + 1);
    fread(source, fsize, 1, f);
    fclose(f);

    source[fsize] = 0;
  }

  const char *transition_source = source ? source : f_default_transition_source;

  int len = strlen(f_shader_template) + strlen(transition_source);
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

  GLint status;
  glGetProgramiv(c->program, GL_LINK_STATUS, &status);
  return status == GL_TRUE ? 0 : -1;
}

static void setup_vbo(GLTransitionContext *c)
{
  glGenBuffers(1, &c->posBuf);
  glBindBuffer(GL_ARRAY_BUFFER, c->posBuf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

  GLint loc = glGetAttribLocation(c->program, "position");
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

static void setup_uniforms(AVFilterLink *fromLink)
{
  AVFilterContext     *ctx = fromLink->dst;
  GLTransitionContext *c = ctx->priv;

  c->progress = glGetUniformLocation(c->program, "progress");
  glUniform1f(c->progress, 0.0f);

  // TODO: this should be output ratio
  c->ratio = glGetUniformLocation(c->program, "ratio");
  glUniform1f(c->ratio, fromLink->w / (float)fromLink->h);

  c->_fromR = glGetUniformLocation(c->program, "_fromR");
  glUniform1f(c->_fromR, fromLink->w / (float)fromLink->h);

  // TODO: initialize this in config_props for "to" input
  c->_toR = glGetUniformLocation(c->program, "_toR");
  glUniform1f(c->_toR, fromLink->w / (float)fromLink->h);
}

static int setup_gl(AVFilterLink *inLink)
{
  AVFilterContext *ctx = inLink->dst;
  GLTransitionContext *c = ctx->priv;

  glViewport(0, 0, inLink->w, inLink->h);

  int ret;
  if((ret = build_program(ctx)) < 0) {
    return ret;
  }

  glUseProgram(c->program);
  setup_vbo(c);
  setup_uniforms(inLink);
  setup_tex(inLink);

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

  // 获取输出帧
  outFrame = ff_get_video_buffer(outLink, outLink->w, outLink->h);
  if (!outFrame) {
    return NULL;
  }

  // 复制帧属性
  av_frame_copy_props(outFrame, fromFrame);
  
  glUseProgram(c->program);

  // 计算转场进度
  const float ts = ((fs->pts - c->first_pts) / (float)fs->time_base.den) - c->offset;
  const float progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
  glUniform1f(c->progress, progress);

  // 设置像素存储方式
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  // 绑定 from 纹理
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->from);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, fromFrame->linesize[0] / 3); // 使用 OpenGL ES 常量
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);

  // 绑定 to 纹理
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, c->to);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, toFrame->linesize[0] / 3); // 使用 OpenGL ES 常量
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);

  // 绘制图形
  glDrawArrays(GL_TRIANGLES, 0, 6);

  // 读取像素数据到输出帧
  glPixelStorei(GL_PACK_ROW_LENGTH, outFrame->linesize[0] / 3); // 使用 OpenGL ES 常量
  glReadPixels(0, 0, outLink->w, outLink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, (GLvoid *)outFrame->data[0]);

  // 重置像素存储方式
  glPixelStorei(GL_PACK_ROW_LENGTH, 0); // 使用 OpenGL ES 常量
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // 使用 OpenGL ES 常量

  // 释放输入帧
  av_frame_free(&fromFrame);

  return outFrame;

}

static int blend_frame(FFFrameSync *fs)
{
  AVFilterContext *ctx = fs->parent;
  GLTransitionContext *c = ctx->priv;

  AVFrame *fromFrame, *toFrame, *outFrame;
  int ret;

  if (fromFrame && fromFrame->pts != AV_NOPTS_VALUE) {
    c->first_pts = fromFrame->pts;
  }

  ret = ff_framesync_dualinput_get(fs, &fromFrame, &toFrame);
  if (ret < 0) {
    return ret;
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

  // 初始化 FFramSync 结构体
  int ret = ff_framesync_init_dualinput(&c->fs, ctx);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
  GLTransitionContext *c = ctx->priv;
  ff_framesync_uninit(&c->fs);

  if (c->from) {
    glDeleteTextures(1, &c->from);
  }
  if (c->to) {
    glDeleteTextures(1, &c->to);
  }
  if (c->posBuf) {
    glDeleteBuffers(1, &c->posBuf);
  }
  if (c->program) {
    glDeleteProgram(c->program);
  }
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
  return ff_framesync_activate(&c->fs);
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
  // outLink->time_base = fromLink->time_base;
  outLink->frame_rate = fromLink->frame_rate;

  return ff_framesync_configure(&c->fs);
}

static const AVFilterPad gltransition_inputs[] = {
  {
    .name = "from",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = setup_gl,
  },
  {
    .name = "to",
    .type = AVMEDIA_TYPE_VIDEO,
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
  .description   = NULL_IF_CONFIG_SMALL("OpenGL blend transitions"),
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
