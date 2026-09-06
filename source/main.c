/* main.c
 *
 * Geometry Dash (Android, arm64) on the Switch.
 *
 * The game is RobTop's cocos2d-x 2.2 fork inside libcocos2dcpp.so plus the
 * stock Android libfmod.so; both are ELF aarch64 and load natively through
 * so_util (libfmod first, so the game's FMOD imports cross-resolve from it).
 * The Android Java layer is replaced by jni_fake.c; the GLSurfaceView render
 * thread is this main thread: bring up EGL, call nativeSetApkPath +
 * nativeInit(w, h), then loop input dispatch -> nativeRender -> swap.
 *
 * The game reads every asset from the .apk itself (its own minizip against
 * the path from nativeSetApkPath); FMOD reads music/sfx as loose files under
 * /switch/gdash/assets/ via the createSound/createStream path rewrite.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include "config.h"
#include "game_compat.h"
#include "paths.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "pthr.h"
#include "net_shim.h"
#include "prefs.h"
#include "asset_index.h"
#include "asset_cache.h"
#include "asset_prefetch.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module fmod_mod; // libfmod.so   (loaded first: exports feed the game)
so_module game_mod; // libcocos2dcpp.so

static volatile int g_quit = 0;

// Set whenever the player actually does something. The periodic autosave
// costs ~253 ms on the render thread (it serialises ~1 MB of game state), and
// sitting on the menu changes nothing worth writing, so skip it when clean.
// Focus-loss and quit still save unconditionally -- those are the ones that
// protect data.
static volatile int g_state_dirty = 0;
static unsigned g_saves_done = 0, g_saves_skipped = 0;

// Last moment the player did ANYTHING -- a touch down/up/move, or simply
// holding a button. The autosave freezes the render thread for ~266 ms, which
// loses a run outright, so it may only fire after a long stretch of complete
// input silence. In a level you are always either tapping or holding, so that
// stretch never arrives; on a menu it arrives seconds after you stop.
static u64 g_last_input_tick;
static u64 g_last_save_tick;
static volatile int g_back_pending; // a BACK press is waiting to be persisted
#define SAVE_IDLE_SECONDS 5
#define SAVE_BACK_COOLDOWN 10 // seconds between BACK-triggered saves

// Configurable button masks (config.txt: jump_buttons / back_buttons).
static u64 g_click_mask, g_left_mask, g_right_mask, g_back_mask;

// ---------------------------------------------------------------------------
// heap split: newlib heap + .so load region (verbatim from the lbbg port)
// ---------------------------------------------------------------------------

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_reserve = (size_t)SO_REGION_MB * 1024 * 1024;
  fake_heap_size = (size > so_reserve + so_reserve / 2) ? (size - so_reserve)
                                                        : (size / 2);

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  if (stat(path_so_game(), &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from the APK's lib/arm64-v8a/\nnext to the .nro.", SO_NAME);
  if (stat(path_so_fmod(), &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from the APK's lib/arm64-v8a/\nnext to the .nro.", FMOD_SO_NAME);
  if (stat(path_assets(), &st) < 0)
    fatal_error("Could not find the assets folder.\nExtract the APK's assets/ into\n%s/.", path_assets());
  // the assets folder must be the FULL apk assets/, not just the audio files
  char probe[600];
  snprintf(probe, sizeof(probe), "%s/GJ_GameSheet.plist", path_assets());
  if (stat(probe, &st) < 0)
    fatal_error("The assets folder is incomplete.\n\nExtract the APK's ENTIRE assets/ folder\n"
                "(sprites, fonts, plists -- not just audio)\ninto\n%s/.", path_assets());
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920; screen_height = 1080;
    } else {
      screen_width = 1280; screen_height = 720;
    }
  } else {
    screen_width = w; screen_height = h;
  }
}

// The game opens its save files for reading before ever creating them; make
// sure they exist (empty is fine, the game treats empty as "new"). Ported
// from the Vita port's save_files_init().
static void save_files_init(void) {
  static const char *names[] = {
    "CCGameManager.dat",
    "CCGameManager2.dat",
    "CCGameManager.dat.bak",
    "CCLocalLevels.dat",
    "CCLocalLevels.dat.bak",
    "CCLocalLevels2.dat",
  };
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", path_save(), names[i]);
    FILE *f = fopen(path, "r");
    if (f) {
      fclose(f);
      continue;
    }
    f = fopen(path, "w");
    if (f)
      fclose(f);
  }
}

// ---------------------------------------------------------------------------
// cocos2d-x JNI entry points (resolved from libcocos2dcpp.so by name)
// ---------------------------------------------------------------------------

// 2.2 touch natives carry a trailing jdouble timestamp in SECONDS (the Java
// side passes MotionEvent.getEventTime()/1000.0) -- confirmed from the dex
// prototypes (IFFD)V / ([I[F[FD)V and the trailing scvtf/q0 use in the
// disassembly. Passing garbage there breaks the 2.2 input-precision path.
static struct {
  void *(*ccFileUtils)(void);                       // CCFileUtils::sharedFileUtils()
  void (*ccAddSearchPath)(void *self, const char *); // ::addSearchPath(const char*)
  void *(*gmSharedState)(void);                     // GameManager::sharedState()
  void (*gmDoQuickSave)(void *self);                // GameManager::doQuickSave()
  void (*init)(void *env, void *thiz, int w, int h);
  void (*render)(void *env, void *thiz);
  void (*onPause)(void *env, void *thiz);
  void (*onResume)(void *env, void *thiz);
  void (*touchesBegin)(void *env, void *thiz, int id, float x, float y, double ts);
  void (*touchesEnd)(void *env, void *thiz, int id, float x, float y, double ts);
  void (*touchesMove)(void *env, void *thiz, void *ids, void *xs, void *ys, double ts);
  void (*touchesCancel)(void *env, void *thiz, void *ids, void *xs, void *ys, double ts);
  unsigned char (*keyDown)(void *env, void *thiz, int keycode);
  void (*insertText)(void *env, void *thiz, void *jstr);
  void (*deleteBackward)(void *env, void *thiz);
  void *(*getContentText)(void *env, void *thiz);
  void (*setEditTextDialogResult)(void *env, void *thiz, void *jbytearr);
} gd;

static int (*fmod_JNI_OnLoad)(void *vm, void *reserved);
static int (*game_JNI_OnLoad)(void *vm, void *reserved);

#define RESOLVE(field, sym) \
  gd.field = (void *)so_find_addr_rx(&game_mod, sym)
#define RESOLVE_OPT(field, sym) \
  gd.field = (void *)so_try_find_addr_rx(&game_mod, sym)

static void resolve_gd_exports(void) {
  RESOLVE(ccFileUtils,     "_ZN7cocos2d11CCFileUtils15sharedFileUtilsEv");
  RESOLVE(ccAddSearchPath, "_ZN7cocos2d11CCFileUtils13addSearchPathEPKc");
  RESOLVE_OPT(gmSharedState, "_ZN11GameManager11sharedStateEv");
  RESOLVE_OPT(gmDoQuickSave, "_ZN11GameManager11doQuickSaveEv");
  RESOLVE(init,          "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  RESOLVE(render,        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  RESOLVE(onPause,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  RESOLVE(onResume,      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  RESOLVE(touchesBegin,  "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  RESOLVE(touchesEnd,    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  RESOLVE(touchesMove,   "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  RESOLVE_OPT(touchesCancel, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
  RESOLVE(keyDown,       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
  RESOLVE_OPT(insertText,     "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText");
  RESOLVE_OPT(deleteBackward, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward");
  RESOLVE_OPT(getContentText, "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText");
  RESOLVE_OPT(setEditTextDialogResult,
              "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetEditTextDialogResult");

  fmod_JNI_OnLoad = (void *)so_try_find_addr_rx(&fmod_mod, "JNI_OnLoad");
  game_JNI_OnLoad = (void *)so_try_find_addr_rx(&game_mod, "JNI_OnLoad");
}

// ---------------------------------------------------------------------------
// EGL (plain: the game imports no EGL at all, GL binds straight to mesa)
// ---------------------------------------------------------------------------

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLSurface s_surf = EGL_NO_SURFACE;
static EGLContext s_ctx = EGL_NO_CONTEXT;

static void init_egl(void) {
  s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_dpy == EGL_NO_DISPLAY)
    fatal_error("eglGetDisplay failed.");
  eglInitialize(s_dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE,
  };
  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  eglChooseConfig(s_dpy, cfg_attr, &cfg, 1, &ncfg);
  if (ncfg < 1)
    fatal_error("No usable EGL config.");

  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, screen_width, screen_height);
  s_surf = eglCreateWindowSurface(s_dpy, cfg, (EGLNativeWindowType)nw, NULL);
  if (s_surf == EGL_NO_SURFACE)
    fatal_error("eglCreateWindowSurface failed.");

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (s_ctx == EGL_NO_CONTEXT)
    fatal_error("eglCreateContext failed.");

  eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
  eglSwapInterval(s_dpy, 1); // 60 fps
  glViewport(0, 0, screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// software keyboard (JNI callbacks from jni_fake.c land here)
// ---------------------------------------------------------------------------

static int swkbd_get_text(const char *header, const char *initial,
                          char *out, size_t out_len) {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return 0;
  swkbdConfigMakePresetDefault(&kbd);
  if (header && header[0])
    swkbdConfigSetHeaderText(&kbd, header);
  if (initial && initial[0])
    swkbdConfigSetInitialText(&kbd, initial);
  Result rc = swkbdShow(&kbd, out, out_len);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc);
}

void gd_open_ime_keyboard(void) {
  if (!gd.insertText)
    return;
  char initial[256] = "";
  if (gd.getContentText) {
    void *jstr = gd.getContentText(fake_env, NULL);
    if (jstr)
      snprintf(initial, sizeof(initial), "%s", jni_string_utf(jstr));
  }

  char out[256] = "";
  if (!swkbd_get_text("", initial, out, sizeof(out)))
    return;

  // replace the current content: clear it, then insert the new text and a
  // newline (cocos2d text fields treat '\n' as end-of-editing)
  if (gd.deleteBackward) {
    for (size_t i = strlen(initial); i > 0; i--)
      gd.deleteBackward(fake_env, NULL);
  }
  gd.insertText(fake_env, NULL, jni_make_string(out));
  gd.insertText(fake_env, NULL, jni_make_string("\n"));
}

void gd_show_edittext_dialog(const char *title, const char *msg, int maxlen) {
  (void)maxlen;
  if (!gd.setEditTextDialogResult)
    return;
  char out[512] = "";
  if (!swkbd_get_text(title, msg, out, sizeof(out)))
    snprintf(out, sizeof(out), "%s", msg ? msg : "");
  gd.setEditTextDialogResult(fake_env, NULL, jni_make_byte_array((int)strlen(out), out));
}

void gd_request_quit(void) {
  g_quit = 1;
}

// ---------------------------------------------------------------------------
// input pump: touchscreen passthrough + gamepad-to-touch synthesis + cursor
// ---------------------------------------------------------------------------

#define MAX_POINTERS 24 // 0..15 real fingers, 20+ virtual (buttons, cursor)

typedef struct { int active; float x, y; } Pointer;
static Pointer pcur[MAX_POINTERS]; // committed state
static Pointer pnew[MAX_POINTERS]; // desired state this frame

static PadState pad;

// virtual pointer ids
enum {
  VPTR_CLICK  = 20, // click buttons -> tap at the cursor (or click_zone)
  VPTR_LEFT   = 21, // platformer left arrow zone  (fixed screen position)
  VPTR_RIGHT  = 22, // platformer right arrow zone (fixed screen position)
};

// Android keycodes
#define AKEY_BACK 4

// Parse a comma-separated button list from config.txt into a HidNpadButton
// mask. Unknown names are ignored; an empty result falls back to the default.
static u64 parse_buttons(const char *list) {
  static const struct { const char *name; u64 bit; } tbl[] = {
    { "A", HidNpadButton_A },         { "B", HidNpadButton_B },
    { "X", HidNpadButton_X },         { "Y", HidNpadButton_Y },
    { "L", HidNpadButton_L },         { "R", HidNpadButton_R },
    { "ZL", HidNpadButton_ZL },       { "ZR", HidNpadButton_ZR },
    { "Plus", HidNpadButton_Plus },   { "Minus", HidNpadButton_Minus },
    { "Up", HidNpadButton_Up },       { "Down", HidNpadButton_Down },
    { "Left", HidNpadButton_Left },   { "Right", HidNpadButton_Right },
    { "LStick", HidNpadButton_StickL },{ "RStick", HidNpadButton_StickR },
  };
  u64 mask = 0;
  for (const char *p = list ? list : ""; *p; ) {
    while (*p == ',' || *p == ' ') p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ' ') p++;
    const size_t len = (size_t)(p - start);
    if (!len) continue;
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(*tbl); i++) {
      if (strlen(tbl[i].name) == len && strncasecmp(start, tbl[i].name, len) == 0) {
        mask |= tbl[i].bit;
        break;
      }
    }
  }
  return mask;
}

// Resolve a configured list. An explicit "none" (or an empty value) disables
// the control; a list that parses to nothing falls back to `dflt`, so a typo
// cannot silently leave the player without a control.
static u64 buttons_or_default(const char *list, u64 dflt) {
  if (!list)
    return dflt;
  const char *p = list;
  while (*p == ' ')
    p++;
  if (!*p)
    return 0;
  if (strncasecmp(p, "none", 4) == 0) {
    const char *q = p + 4;
    while (*q == ' ')
      q++;
    if (!*q)
      return 0;
  }
  const u64 m = parse_buttons(list);
  return m ? m : dflt;
}

// stick-driven cursor for menu navigation (essential when docked)
static float cursor_x, cursor_y;
static u64 cursor_last_move_tick;

static void cursor_update(void) {
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  // either stick moves the cursor; right stick has priority
  float dx = (float)rs.x / 32767.0f, dy = (float)rs.y / 32767.0f;
  if (fabsf(dx) < 0.25f && fabsf(dy) < 0.25f) {
    dx = (float)ls.x / 32767.0f;
    dy = (float)ls.y / 32767.0f;
  }
  if (fabsf(dx) < 0.25f) dx = 0.f;
  if (fabsf(dy) < 0.25f) dy = 0.f;
  if (dx != 0.f || dy != 0.f) {
    const float speed = (float)config.cursor_speed * ((float)screen_height / 720.0f);
    cursor_x += dx * speed / 60.0f;
    cursor_y -= dy * speed / 60.0f; // stick up = cursor up (screen y down)
    if (cursor_x < 0.f) cursor_x = 0.f;
    if (cursor_y < 0.f) cursor_y = 0.f;
    if (cursor_x > (float)screen_width - 1.f)  cursor_x = (float)screen_width - 1.f;
    if (cursor_y > (float)screen_height - 1.f) cursor_y = (float)screen_height - 1.f;
    cursor_last_move_tick = armGetSystemTick();
  }
}

static int cursor_visible(void) {
  if (!cursor_last_move_tick)
    return 0;
  return (armGetSystemTick() - cursor_last_move_tick) <
         armGetSystemTickFreq() * 3; // 3 s after the last stick motion
}

// scissor-clear rectangle painter (no shaders, state saved/restored)
static void draw_rect(int x, int y, int w, int h, float r, float g, float b) {
  glScissor(x, y, w, h);
  glClearColor(r, g, b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// A configured zone percentage (0-100) as a pixel coordinate, clamped so a
// bad value in config.txt cannot put the tap off-screen.
static float zone_px(int percent, float extent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  float v = (float)percent * 0.01f * extent;
  if (v > extent - 1.0f) v = extent - 1.0f;
  return v;
}

// show_zones: mark where the arrow taps land, so they can be lined up with
// the game's own arrows without guessing. Left is drawn darker than right.
static void zones_render(void) {
  if (!config.show_zones)
    return;
  GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLint old_box[4];
  GLfloat old_clear[4];
  glGetIntegerv(GL_SCISSOR_BOX, old_box);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  glEnable(GL_SCISSOR_TEST);

  const int s = screen_height / 45; // roughly 16 px at 720p
  const struct { int x, y; float r, g, b; } marks[] = {
    { config.left_zone_x,  config.left_zone_y,  1.0f, 0.35f, 0.0f }, // orange
    { config.right_zone_x, config.right_zone_y, 0.0f, 0.8f,  1.0f }, // blue
    { config.click_zone_x, config.click_zone_y, 0.2f, 1.0f,  0.3f }, // green
  };
  for (unsigned i = 0; i < sizeof(marks) / sizeof(*marks); i++) {
    const int cx = (int)zone_px(marks[i].x, (float)screen_width);
    // GL window coords are bottom-left based; the zone y is top-left based
    const int cy = screen_height - 1 - (int)zone_px(marks[i].y, (float)screen_height);
    draw_rect(cx - s / 2 - 1, cy - s / 2 - 1, s + 2, s + 2, 0.f, 0.f, 0.f);
    draw_rect(cx - s / 2, cy - s / 2, s, s, marks[i].r, marks[i].g, marks[i].b);
  }

  glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  if (!had_scissor)
    glDisable(GL_SCISSOR_TEST);
}

static void cursor_render(void) {
  if (!cursor_visible())
    return;
  GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  GLint old_box[4];
  GLfloat old_clear[4];
  glGetIntegerv(GL_SCISSOR_BOX, old_box);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  glEnable(GL_SCISSOR_TEST);

  // GL window coords: origin bottom-left; cursor_y is top-left based
  const int cx = (int)cursor_x;
  const int cy = screen_height - 1 - (int)cursor_y;
  const int s = screen_height / 90; // ~8 px at 720p
  draw_rect(cx - s / 2 - 1, cy - s / 2 - 1, s + 2, s + 2, 0.f, 0.f, 0.f);
  draw_rect(cx - s / 2, cy - s / 2, s, s, 1.f, 1.f, 1.f);

  glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  if (!had_scissor)
    glDisable(GL_SCISSOR_TEST);
}

static void build_touch_pointers(void) {
  HidTouchScreenState st = { 0 };
  if (!hidGetTouchScreenStates(&st, 1))
    return;
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;
  for (int i = 0; i < (int)st.count; i++) {
    const int id = (int)st.touches[i].finger_id;
    if (id < 0 || id >= 16)
      continue;
    pnew[id].active = 1;
    pnew[id].x = (float)st.touches[i].x * sx;
    pnew[id].y = (float)st.touches[i].y * sy;
  }
}

static void build_virtual_pointers(void) {
  const u64 down = padGetButtons(&pad);
  const float w = (float)screen_width, h = (float)screen_height;

  // One click, anchored to the cursor.
  //
  // The port cannot tell a level from a menu, so a touch synthesised at a
  // FIXED screen coordinate is wrong half the time: the old bottom-right jump
  // tap landed on whatever UI happened to sit in that corner, which is issue
  // #7 (the vault door in the custom level menu). Anchoring to the cursor
  // removes the guesswork -- in a level ANY tap jumps, so the position is
  // irrelevant there, while in a menu it clicks exactly what the player aimed
  // at. With the cursor hidden (sticks idle, i.e. not navigating a menu) we
  // tap click_zone instead, which defaults to the level-select level box --
  // irrelevant in a level, where any tap jumps regardless, but it means the
  // click button works on the level box without aiming the cursor first.
  if (down & g_click_mask) {
    pnew[VPTR_CLICK].active = 1;
    if (cursor_visible()) {
      pnew[VPTR_CLICK].x = cursor_x;
      pnew[VPTR_CLICK].y = cursor_y;
    } else {
      pnew[VPTR_CLICK].x = zone_px(config.click_zone_x, w);
      pnew[VPTR_CLICK].y = zone_px(config.click_zone_y, h);
    }
  }

  // Platformer move zones. These have to land on the on-screen arrows, so they
  // are unavoidably fixed positions -- and the defaults are inherited from the
  // Vita port, which is no guarantee they match where THIS build draws them.
  // Hence config.txt: set show_zones 1 to see the markers and aim them.
  // Outside a platformer level they hit whatever is at that spot, so
  // left_buttons / right_buttons can be set to "none" to disable them.
  if (down & g_left_mask) {
    pnew[VPTR_LEFT].active = 1;
    pnew[VPTR_LEFT].x = zone_px(config.left_zone_x, w);
    pnew[VPTR_LEFT].y = zone_px(config.left_zone_y, h);
  }
  if (down & g_right_mask) {
    pnew[VPTR_RIGHT].active = 1;
    pnew[VPTR_RIGHT].x = zone_px(config.right_zone_x, w);
    pnew[VPTR_RIGHT].y = zone_px(config.right_zone_y, h);
  }
}

// monotonic seconds for the touch timestamps (same clock across all events)
static double now_seconds(void) {
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq();
  return (double)armGetSystemTick() / (double)freq;
}

// translate the per-frame pointer delta into the cocos touch protocol
static void dispatch_pointers(void) {
  // reusable JNI arrays for the move batch
  static void *move_ids, *move_xs, *move_ys;
  if (!move_ids) {
    move_ids = jni_make_int_array(MAX_POINTERS, NULL);
    move_xs = jni_make_float_array(MAX_POINTERS, NULL);
    move_ys = jni_make_float_array(MAX_POINTERS, NULL);
  }

  const double ts = now_seconds();

  // downs
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (pnew[i].active && !pcur[i].active)
      gd.touchesBegin(fake_env, NULL, i, pnew[i].x, pnew[i].y, ts);
  }

  // moves, batched like the Java GLSurfaceView does
  int nmove = 0;
  int ids[MAX_POINTERS];
  float xs[MAX_POINTERS], ys[MAX_POINTERS];
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (pnew[i].active && pcur[i].active &&
        (pnew[i].x != pcur[i].x || pnew[i].y != pcur[i].y)) {
      ids[nmove] = i;
      xs[nmove] = pnew[i].x;
      ys[nmove] = pnew[i].y;
      nmove++;
    }
  }
  if (nmove > 0) {
    jni_prim_array_fill(move_ids, ids, nmove, sizeof(int));
    jni_prim_array_fill(move_xs, xs, nmove, sizeof(float));
    jni_prim_array_fill(move_ys, ys, nmove, sizeof(float));
    gd.touchesMove(fake_env, NULL, move_ids, move_xs, move_ys, ts);
  }

  // ups
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (!pnew[i].active && pcur[i].active)
      gd.touchesEnd(fake_env, NULL, i, pcur[i].x, pcur[i].y, ts);
  }

  if (memcmp(pcur, pnew, sizeof(pcur)) != 0) {
    g_state_dirty = 1; // a touch went down, moved, or lifted
    g_last_input_tick = armGetSystemTick();
  }
  memcpy(pcur, pnew, sizeof(pcur));
}

static void update_input(void) {
  padUpdate(&pad);

  cursor_update();

  memset(pnew, 0, sizeof(pnew));
  build_touch_pointers();
  build_virtual_pointers();
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (pnew[i].active) { // held button or finger down: still playing
      g_last_input_tick = armGetSystemTick();
      break;
    }
  }
  dispatch_pointers();

  // BACK (pause / dismiss): B or Plus, edge-triggered
  const u64 pressed = padGetButtonsDown(&pad);
  if ((pressed & g_back_mask) && !g_block_back_button) {
    g_state_dirty = 1;
    g_back_pending = 1; // pausing / leaving a level: a safe moment to save
    g_last_input_tick = armGetSystemTick();
    gd.keyDown(fake_env, NULL, AKEY_BACK);
  }
}

static AppletHookCookie s_applet_hook;
static int s_app_focused = 1;

// GD only saves on background/quit, which is unreliable on Switch; call its
// own quick-save directly.
static void force_save(void) {
  if (gd.gmSharedState && gd.gmDoQuickSave) {
    void *gm = gd.gmSharedState();
    if (gm)
      gd.gmDoQuickSave(gm);
  }
  g_saves_done++;
  g_state_dirty = 0;
  g_back_pending = 0;
  g_last_save_tick = armGetSystemTick();
}

// pause/resume + save on focus change (HOME), like Android onPause/onResume
static void applet_focus_hook(AppletHookType type, void *param) {
  (void)param;
  if (type != AppletHookType_OnFocusState)
    return;
  const int focused = (appletGetFocusState() == AppletFocusState_InFocus);
  if (focused && !s_app_focused) {
    s_app_focused = 1;
    gd.onResume(fake_env, NULL);
  } else if (!focused && s_app_focused) {
    s_app_focused = 0;
    gd.onPause(fake_env, NULL);
    force_save();
  }
}

int main(int argc, char **argv) {
  pthr_pin_render_core();
  cpu_boost(1);

  paths_init(argc > 0 ? argv[0] : NULL);

  read_config(path_config());
  // Rewrite every launch: otherwise keys added by a newer build never appear
  // in an existing config.txt and silently run on defaults the user cannot see.
  write_config(path_config());

  g_click_mask = buttons_or_default(config.click_buttons,
      HidNpadButton_A | HidNpadButton_ZR | HidNpadButton_R |
      HidNpadButton_ZL | HidNpadButton_L);
  g_left_mask  = buttons_or_default(config.left_buttons, HidNpadButton_Left);
  g_right_mask = buttons_or_default(config.right_buttons, HidNpadButton_Right);
  g_back_mask  = buttons_or_default(config.back_buttons,
      HidNpadButton_B | HidNpadButton_Plus);

  check_syscalls();
  check_data();
  asset_index_build(path_assets()); // one directory walk now, none per frame
  // 89 MB holds every non-audio asset; the heap here is well over 1 GB
  asset_cache_init(path_assets(), 96u * 1024u * 1024u);
  game_compat_detect_package(path_so_game());
  set_screen_size(config.screen_width, config.screen_height);

  extern char *fake_heap_start;
  const unsigned heap_mb =
      (unsigned)(((char *)heap_so_base - fake_heap_start) / (1024 * 1024));

  if (heap_mb < 1500)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Launch hbmenu over a game (hold R while\n"
                "starting any installed title), then start\n"
                "this port from there.", heap_mb);

  mkdir(path_save(), 0777);
  save_files_init();
  prefs_load(path_prefs());
  net_init();

  // libfmod.so first: the game's FMOD imports resolve from its exports
  const size_t fmod_reserve = 16 * 1024 * 1024;
  if (so_load(&fmod_mod, path_so_fmod(), heap_so_base, fmod_reserve) < 0)
    fatal_error("Could not load\n%s.", FMOD_SO_NAME);
  if (so_load(&game_mod, path_so_game(), (char *)heap_so_base + fmod_reserve,
              heap_so_limit - fmod_reserve) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();

  so_relocate(&fmod_mod);
  so_resolve(&fmod_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  game_compat_apply_network(&game_mod);
  fmod_hooks_init(&fmod_mod);
  resolve_gd_exports();

  // OPENSSL_cpuid_setup (a static ctor) probes the CPU with a SIGILL/longjmp
  // harness that faults without signal delivery; neutralize it -> portable C.
  {
    uintptr_t cpuid = so_try_find_addr(&game_mod, "OPENSSL_cpuid_setup");
    if (cpuid)
      hook_arm64(cpuid, (uintptr_t)&ret0);
  }

  // Keep the loose-assets search path alive: purgeFileUtils() (on a texture-
  // quality change) would drop it and the game never re-adds it.
  {
    uintptr_t purge = so_try_find_addr(&game_mod, "_ZN7cocos2d11CCFileUtils14purgeFileUtilsEv");
    if (purge)
      hook_arm64(purge, (uintptr_t)&ret0);
  }

  so_finalize(&fmod_mod);
  so_flush_caches(&fmod_mod);
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // fake TLS must exist before any game code runs (static ctors read the
  // stack-guard cookie); init_array below is game code
  pthr_install_fake_tls();
  so_execute_init_array(&fmod_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&fmod_mod);
  so_free_temp(&game_mod);

  jni_init();

  if (fmod_JNI_OnLoad)
    fmod_JNI_OnLoad(fake_vm, NULL);
  if (game_JNI_OnLoad)
    game_JNI_OnLoad(fake_vm, NULL);

  // load assets loose from <base>/assets/: cocos2d fopen()s an absolute
  // ('/'-rooted) search path directly, so no .apk is needed
  gd.ccAddSearchPath(gd.ccFileUtils(), path_assets_search());

  // input before first frame
  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  cursor_x = screen_width / 2.0f;
  cursor_y = screen_height / 2.0f;

  // GL up, then boot the engine on this thread (context current)
  init_egl();
  gd.init(fake_env, NULL, screen_width, screen_height);

  cpu_boost(0); // drop the load-time boost; clocks are the user's business
                // (sys-clk / Horizon OC), and boosting here would only fight
                // them -- measured: it does not affect the remaining stalls.

  // warm the asset cache in the background so the menu stops paying a
  // filesystem open every time it shows a new icon
  asset_prefetch_start();

  appletHook(&s_applet_hook, applet_focus_hook, NULL);

  unsigned frame = 0;
  while (appletMainLoop() && !g_quit) {
    update_input();
    gd.render(fake_env, NULL);
    zones_render();
    cursor_render();
    eglSwapBuffers(s_dpy, s_surf);
    // A BACK press means pausing or leaving a level -- the game is not in
    // motion, so the ~250 ms save costs nothing. This is what guarantees a
    // save whenever you come out of a level, rather than hoping for an idle
    // window. Rate-limited so menu navigation does not save on every press.
    if (g_back_pending && g_state_dirty &&
        (armGetSystemTick() - g_last_save_tick) >
            armGetSystemTickFreq() * SAVE_BACK_COOLDOWN)
      force_save();

    if (++frame >= 60 * 20) { // ~20 s autosave, but only when it is safe
      frame = 0;
      const u64 idle = armGetSystemTick() - g_last_input_tick;
      if (g_state_dirty &&
          idle > armGetSystemTickFreq() * SAVE_IDLE_SECONDS)
        force_save();
      else
        g_saves_skipped++; // mid-level, or nothing worth writing
    }
  }

  asset_prefetch_stop(); // no background reads while we save and tear down

  if (s_app_focused) { // clean in-game quit; background path already saved
    gd.onPause(fake_env, NULL);
    force_save();
  }
  svcSleepThread(200000000ull); // 200 ms for save writes on worker threads

  eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(s_dpy, s_ctx);
  eglDestroySurface(s_dpy, s_surf);
  eglTerminate(s_dpy);
  net_exit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
