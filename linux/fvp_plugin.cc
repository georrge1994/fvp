#include "include/fvp/fvp_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <cstring>
#include <iostream>
#include <unordered_map>
#include <epoxy/gl.h>

#include "mdk/RenderAPI.h"
#include "mdk/Player.h"

using namespace std;

class TexturePlayer;

G_DECLARE_FINAL_TYPE(PlayerTexture, player_texture, FL, PLAYER_TEXTURE, FlTextureGL)

#define PLAYER_TEXTURE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), player_texture_get_type(), PlayerTexture))

struct _PlayerTexture {
  FlTextureGL parent_instance;

  GdkGLContext* ctx;
  GLuint texture_id;
  GLuint fbo;

  TexturePlayer* player;
};

G_DEFINE_TYPE(PlayerTexture, player_texture, fl_texture_gl_get_type())


class TexturePlayer final : public mdk::Player
{
public:
  TexturePlayer(int64_t handle, PlayerTexture* tex, int w, int h, FlTextureRegistrar* texRegistrar)
    : mdk::Player(reinterpret_cast<mdkPlayerAPI*>(handle))
    , width(w)
    , height(h)
    , texReg(texRegistrar)
    , flTex(tex)
  {
    flTex->player = this;

    if (!fl_texture_registrar_register_texture(texReg, FL_TEXTURE(flTex))) {
      clog << "fl_texture_registrar_register_texture error" << endl;
      return;
    }
    textureId = fl_texture_get_id(FL_TEXTURE(flTex)); // MUST be after fl_texture_registrar_register_texture(), id is set there

    scale(1, -1); // y is flipped
    setVideoSurfaceSize(width, height);
    setRenderCallback([this](void*) {
      //renderVideo(); // need a gl context
      fl_texture_registrar_mark_texture_frame_available(texReg, FL_TEXTURE(flTex));
      });
  }

  ~TexturePlayer() override {
    if (!fl_texture_registrar_unregister_texture(texReg, FL_TEXTURE(flTex))) {
      clog << "fl_texture_registrar_unregister_texture error" << endl;
    }
    setRenderCallback(nullptr);
    setVideoSurfaceSize(-1, -1);

    g_object_unref(flTex);
  }

  int64_t textureId;
  int width;
  int height;
private:
  FlTextureRegistrar* texReg;
  PlayerTexture* flTex; // hold ref
};

static unordered_map<int64_t, shared_ptr<TexturePlayer>> players;

// called in a current gl context
static gboolean player_texture_populate(FlTextureGL *texture, uint32_t *target, uint32_t *name,
                        uint32_t *width, uint32_t *height, GError **error) {
  PlayerTexture *self = PLAYER_TEXTURE(texture);

  if (self->fbo == 0) {
    self->ctx = gdk_gl_context_get_current(); // fbo can not be shared
    glGenFramebuffers(1, &self->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
  }
  if (self->texture_id == 0) {
    glGenTextures(1, &self->texture_id);
    glBindTexture(GL_TEXTURE_2D, self->texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, self->player->width, self->player->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 0, GL_TEXTURE_2D, self->texture_id, 0);
    const GLenum err = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (err != GL_FRAMEBUFFER_COMPLETE) {
        //glDeleteFramebuffers(1, &fbo);
        clog << "glFramebufferTexture2D error" << endl;
        return FALSE;
    }
    mdk::GLRenderAPI ra{};
    ra.fbo = self->fbo;
    self->player->setRenderAPI(&ra);
  }

  self->player->renderVideo();

  *target = GL_TEXTURE_2D;
  *name = self->texture_id;
  *width = self->player->width;
  *height = self->player->height;

  return TRUE;
}

static void player_texture_dispose(GObject* obj) {
  G_OBJECT_CLASS(player_texture_parent_class)->dispose(obj);
  auto self = PLAYER_TEXTURE(obj);
  auto ctx = gdk_gl_context_get_current();
  gdk_gl_context_make_current(self->ctx);
  glDeleteTextures(1, &self->texture_id);
  glDeleteFramebuffers(1, &self->fbo);
  if (ctx)
    gdk_gl_context_make_current(ctx);
}

static void player_texture_class_init(PlayerTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = player_texture_populate;
  auto gklass = G_OBJECT_CLASS(klass);
  gklass->dispose = player_texture_dispose;
}

static void player_texture_init(PlayerTexture* self) {
  self->texture_id = 0;
  self->fbo = 0;
  self->player = nullptr;
  self->ctx = nullptr;
}


#define FVP_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), fvp_plugin_get_type(), \
                              FvpPlugin))

struct _FvpPlugin {
  GObject parent_instance;

  FlTextureRegistrar* tex_registrar;
};

G_DEFINE_TYPE(FvpPlugin, fvp_plugin, g_object_get_type())

// Called when a method call is received from Flutter.
static void fvp_plugin_handle_method_call(
    FvpPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar* method = fl_method_call_get_name(method_call);

  if (strcmp(method, "CreateRT") == 0) {
    const auto args = fl_method_call_get_args(method_call);
    const auto handle = fl_value_get_int(fl_value_lookup_string(args, "player"));
    const auto width = (int)fl_value_get_int(fl_value_lookup_string(args, "width"));
    const auto height = (int)fl_value_get_int(fl_value_lookup_string(args, "height"));
    auto tex = PLAYER_TEXTURE(g_object_new(player_texture_get_type(), nullptr));
    auto player = make_shared<TexturePlayer>(handle, tex, width, height, self->tex_registrar);
    players[player->textureId] = player;
    g_autoptr(FlValue) result = fl_value_new_int(player->textureId);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "ReleaseRT") == 0) {
    const auto args = fl_method_call_get_args(method_call);
    const auto texId = fl_value_get_int(fl_value_lookup_string(args, "texture"));
    if (auto it = players.find(texId); it != players.cend()) {
        players.erase(it);
    }
    g_autoptr(FlValue) result = fl_value_new_null();
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void fvp_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(fvp_plugin_parent_class)->dispose(object);
}

static void fvp_plugin_class_init(FvpPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fvp_plugin_dispose;
}

static void fvp_plugin_init(FvpPlugin* self) {
  self->tex_registrar = nullptr;
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  FvpPlugin* plugin = FVP_PLUGIN(user_data);
  fvp_plugin_handle_method_call(plugin, method_call);
}

void fvp_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  FvpPlugin* plugin = FVP_PLUGIN(
      g_object_new(fvp_plugin_get_type(), nullptr));
  plugin->tex_registrar = fl_plugin_registrar_get_texture_registrar(registrar);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "fvp",
                            FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);

  auto gdisp = gdk_display_get_default();
  if (GDK_IS_X11_DISPLAY(gdisp)) {
    mdk::SetGlobalOption("X11Display", GDK_DISPLAY_XDISPLAY(gdisp));
  }
}
