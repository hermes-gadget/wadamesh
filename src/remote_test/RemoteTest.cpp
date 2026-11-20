// SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
// wadamesh remote-test controller — serial command surface for driving the
// touch UI from a host (screenshots, taps, typed text, navigation).
//
// Build: env LilyGo_TDeck_companion_radio_touch_remote (-D WADAMESH_REMOTE_TEST=1)
// Ported from SigurdOS's SIGURDOS_REMOTE_TEST controller; kept deliberately
// self-contained so upstream rebases only touch a few fenced hook lines.
//
// Output lines are prefixed "[rt] " so a host can parse them out of the normal
// boot/mesh chatter. Input lines are only consumed when the first byte is an
// ASCII letter, so companion-protocol frames are never stolen.
//
// Screenshots reuse the wadamesh DOC_CAPTURE streamer (16-bit BMP framed by
// <<<WMSHOT ...>>> / <<<WMEND>>>) via the UITask::remoteCaptureToSerial shim,
// so the host toolchain is the same one scripts/doc/capture.py already uses.
// ============================================================================
#include "RemoteTest.h"

#if defined(WADAMESH_REMOTE_TEST) && WADAMESH_REMOTE_TEST

#include <Arduino.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <lvgl.h>

#include "MyMesh.h"
#include "RemoteTestCommands.h"
#include "UITask.h"
#include "helpers/esp32/WebMirror.h"

namespace remote_test {
namespace {

// ── tunables ───────────────────────────────────────────────────────────────
constexpr size_t   kLineBuffer         = 256;
constexpr uint32_t kTapDefaultHoldMs   = 80;
constexpr uint16_t kTapMaxHoldMs       = 4000;
constexpr uint8_t  kTypeCharsPerPass   = 3;
constexpr int      kTreeMaxDepth       = 8;
constexpr int      kTreeMaxNodes       = 160;

UITask* s_ui   = nullptr;
MyMesh* s_mesh = nullptr;

char   s_line[kLineBuffer];
size_t s_line_len  = 0;
bool   s_line_open = false;

struct TapJob {
  bool     active;
  int16_t  x, y;
  uint8_t  phase;      // 0 idle, 1 pressed (waiting for hold to elapse)
  uint32_t press_ms;
  uint16_t hold_ms;
};
TapJob s_tap = {};

struct TypeJob {
  bool     active;
  char     text[200];
  size_t   len;
  size_t   pos;
  uint32_t last_ms;
};
TypeJob s_type = {};

// ── small helpers ──────────────────────────────────────────────────────────
const char* className(const lv_obj_t* obj) {
  if (!obj) return "?";
  if (lv_obj_check_type(obj, &lv_label_class))        return "label";
  if (lv_obj_check_type(obj, &lv_btn_class))          return "btn";
  if (lv_obj_check_type(obj, &lv_btnmatrix_class))    return "btnmatrix";
  if (lv_obj_check_type(obj, &lv_textarea_class))     return "textarea";
  if (lv_obj_check_type(obj, &lv_keyboard_class))     return "keyboard";
  if (lv_obj_check_type(obj, &lv_img_class))          return "img";
  if (lv_obj_check_type(obj, &lv_tabview_class))      return "tabview";
  if (lv_obj_check_type(obj, &lv_slider_class))       return "slider";
  if (lv_obj_check_type(obj, &lv_switch_class))       return "switch";
  if (lv_obj_check_type(obj, &lv_checkbox_class))     return "checkbox";
  if (lv_obj_check_type(obj, &lv_dropdown_class))     return "dropdown";
  if (lv_obj_check_type(obj, &lv_roller_class))       return "roller";
  if (lv_obj_check_type(obj, &lv_bar_class))          return "bar";
  if (lv_obj_check_type(obj, &lv_chart_class))        return "chart";
  if (lv_obj_check_type(obj, &lv_msgbox_class))       return "msgbox";
  if (lv_obj_check_type(obj, &lv_list_class))         return "list";
  if (lv_obj_check_type(obj, &lv_obj_class))          return "obj";
  return "other";
}

lv_obj_t* findTabview() {
  lv_obj_t* root = lv_scr_act();
  if (!root) return nullptr;
  // The app's tabview is a direct child of the screen; tolerate one wrapper
  // layer on boards that add one.
  const uint32_t n = lv_obj_get_child_cnt(root);
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(root, i);
    if (c && lv_obj_check_type(c, &lv_tabview_class)) return c;
  }
  for (uint32_t i = 0; i < n; ++i) {
    lv_obj_t* c = lv_obj_get_child(root, i);
    if (!c) continue;
    const uint32_t m = lv_obj_get_child_cnt(c);
    for (uint32_t j = 0; j < m; ++j) {
      lv_obj_t* g = lv_obj_get_child(c, j);
      if (g && lv_obj_check_type(g, &lv_tabview_class)) return g;
    }
  }
  return nullptr;
}

int tabCount() {
  lv_obj_t* tv = findTabview();
  if (!tv) return 0;
  lv_obj_t* content = lv_tabview_get_content(tv);
  return content ? static_cast<int>(lv_obj_get_child_cnt(content)) : 0;
}

int tabIndex() {
  lv_obj_t* tv = findTabview();
  return tv ? static_cast<int>(lv_tabview_get_tab_act(tv)) : -1;
}

const char* tabName(int idx, int count) {
  if (idx < 0) return "?";
  if (idx == 0) return "chats";
  if (idx == 1) return "contacts";
  if (idx == 2) return "home";
  if (idx == 3) return "map";
  if (count == 6 && idx == 4) return "sensors";
  if (count > 0 && idx == count - 1) return "settings";
  return "other";
}

// ── output ─────────────────────────────────────────────────────────────────
void out(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print("[rt] ");
  Serial.println(buf);
}

// ── jobs ───────────────────────────────────────────────────────────────────
void startTap(int16_t x, int16_t y, uint16_t hold_ms) {
  if (s_tap.active) {
    out("tap busy");
    return;
  }
  s_tap.active   = true;
  s_tap.x        = x;
  s_tap.y        = y;
  s_tap.hold_ms  = hold_ms ? hold_ms : kTapDefaultHoldMs;
  s_tap.phase    = 1;
  s_tap.press_ms = millis();
  g_web_mirror.pushPointer(x, y, 1);
}

void serviceTap(uint32_t now) {
  if (!s_tap.active || s_tap.phase != 1) return;
  if (now - s_tap.press_ms < s_tap.hold_ms) return;
  g_web_mirror.pushPointer(s_tap.x, s_tap.y, 0);
  s_tap.phase  = 0;
  s_tap.active = false;
  out("tap %d %d done", s_tap.x, s_tap.y);
}

void startType(const char* text) {
  if (!text || !*text) { out("type: empty"); return; }
  if (s_type.active) { out("type busy"); return; }
  size_t n = 0;
  for (const char* p = text; *p && n + 1 < sizeof(s_type.text); ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c > 0x7E) continue;   // ASCII printables only (v1)
    s_type.text[n++] = static_cast<char>(c);
  }
  s_type.text[n] = '\0';
  s_type.len     = n;
  s_type.pos     = 0;
  s_type.last_ms = 0;
  s_type.active  = n > 0;
  out("type %u chars", static_cast<unsigned>(n));
}

void serviceType(uint32_t now) {
  if (!s_type.active) return;
  if (s_type.last_ms && now - s_type.last_ms < 15) return;
  s_type.last_ms = now;
  for (uint8_t i = 0; i < kTypeCharsPerPass && s_type.pos < s_type.len; ++i) {
    s_ui->remoteKey(static_cast<uint16_t>(static_cast<unsigned char>(s_type.text[s_type.pos++])));
  }
  if (s_type.pos >= s_type.len) {
    s_type.active = false;
    out("type done");
  }
}

void printTree(lv_obj_t* obj, int depth, int* budget) {
  if (!obj || !budget || *budget <= 0 || depth > kTreeMaxDepth) return;
  --*budget;
  Serial.printf("[rt] tree %*s%s %d,%d %dx%d%s%s\n", depth * 2, "",
                className(obj), static_cast<int>(lv_obj_get_x(obj)),
                static_cast<int>(lv_obj_get_y(obj)),
                static_cast<int>(lv_obj_get_width(obj)),
                static_cast<int>(lv_obj_get_height(obj)),
                lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? " hidden" : "",
                lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? " clickable" : "");
  const uint32_t n = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < n; ++i) printTree(lv_obj_get_child(obj, i), depth + 1, budget);
}

// ── commands ───────────────────────────────────────────────────────────────
void cmdHelp() {
  out("commands: help ping screen nav tab back tap key type status tree widgets capture setrf display lock unlock wake reboot version");
  out("nav <0..N|chats|contacts|home|map|sensors|settings>");
  out("tap <x> <y> [hold_ms]   key <enter|backspace|esc|tab|up|down|left|right|space|delete>");
  out("type <ascii text>       capture [name]  -> 16-bit BMP (WMSHOT)");
  out("setrf <freq_mhz> <bw_khz> <sf> <cr> <dbm>");
}

void cmdNav(char* args) {
  char* tok = nextToken(&args);
  if (!tok) { out("nav: need index or name"); return; }
  const int count = tabCount();
  if (count <= 0) { out("nav: no tabview"); return; }
  int idx = -1;
  uint32_t v = 0;
  if (parseU32(tok, &v) && v < static_cast<uint32_t>(count)) {
    idx = static_cast<int>(v);
  } else if (strcmp(tok, "chats") == 0) {
    idx = 0;
  } else if (strcmp(tok, "contacts") == 0) {
    idx = 1;
  } else if (strcmp(tok, "home") == 0) {
    idx = 2;
  } else if (strcmp(tok, "map") == 0) {
    idx = 3;
  } else if (strcmp(tok, "sensors") == 0) {
    idx = (count == 6) ? 4 : -1;
  } else if (strcmp(tok, "settings") == 0) {
    idx = count - 1;
  }
  if (idx < 0 || idx >= count) { out("nav: unknown target (count=%d)", count); return; }
  if (!s_ui->remoteSelectTab(idx)) { out("nav: switch failed"); return; }
  out("nav %d %s", idx, tabName(idx, count));
}

void cmdTab(char* args) {
  char* tok = nextToken(&args);
  const int count = tabCount();
  if (count <= 0) { out("tab: no tabview"); return; }
  int cur = tabIndex();
  if (!tok || strcmp(tok, "next") == 0) cur = (cur + 1) % count;
  else if (strcmp(tok, "prev") == 0 || strcmp(tok, "previous") == 0) cur = (cur + count - 1) % count;
  else { out("tab: next|prev"); return; }
  if (!s_ui->remoteSelectTab(cur)) { out("tab: switch failed"); return; }
  out("nav %d %s", cur, tabName(cur, count));
}

void cmdKey(char* args) {
  char* tok = nextToken(&args);
  if (!tok) { out("key: need a name"); return; }
  int code = -1;
  if (strcmp(tok, "enter") == 0 || strcmp(tok, "return") == 0) code = 0x0D;
  else if (strcmp(tok, "backspace") == 0) code = 0x08;
  else if (strcmp(tok, "delete") == 0 || strcmp(tok, "del") == 0) code = 0x7F;
  else if (strcmp(tok, "esc") == 0 || strcmp(tok, "escape") == 0) code = LV_KEY_ESC;
  else if (strcmp(tok, "tab") == 0) code = LV_KEY_NEXT;
  else if (strcmp(tok, "up") == 0) code = LV_KEY_UP;
  else if (strcmp(tok, "down") == 0) code = LV_KEY_DOWN;
  else if (strcmp(tok, "left") == 0) code = LV_KEY_LEFT;
  else if (strcmp(tok, "right") == 0) code = LV_KEY_RIGHT;
  else if (strcmp(tok, "space") == 0) code = ' ';
  else if (tok[1] == '\0') code = static_cast<unsigned char>(tok[0]);
  if (code < 0) { out("key: unknown '%s'", tok); return; }
  s_ui->remoteKey(static_cast<uint16_t>(code));
  out("key %s", tok);
}

void cmdStatus() {
  const int count = tabCount();
  const int idx   = tabIndex();
  const NodePrefs* prefs = s_mesh->getNodePrefs();
  if (!prefs) { out("status: no prefs"); return; }
  out("status uptime_ms=%lu heap=%u psram_free=%u tab=%d name=%s tabs=%d off=%d locked=%d",
      static_cast<unsigned long>(millis()),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      idx, tabName(idx, count), count,
      s_ui->isScreenOff() ? 1 : 0, s_ui->isManualLock() ? 1 : 0);
  out("radio freq=%.3f bw=%.1f sf=%u cr=%u tx=%d airtime=%.2f",
      prefs->freq, prefs->bw, static_cast<unsigned>(prefs->sf),
      static_cast<unsigned>(prefs->cr), static_cast<int>(prefs->tx_power_dbm),
      prefs->airtime_factor);
}

void cmdSetrf(char* args) {
  char* freq_s = nextToken(&args);
  char* bw_s   = nextToken(&args);
  char* sf_s   = nextToken(&args);
  char* cr_s   = nextToken(&args);
  char* dbm_s  = nextToken(&args);
  if (!freq_s || !bw_s || !sf_s || !cr_s || !dbm_s) {
    out("setrf: <freq_mhz> <bw_khz> <sf> <cr> <dbm>");
    return;
  }
  uint32_t sf = 0, cr = 0;
  int32_t dbm = 0;
  if (!parseU32(sf_s, &sf, 12) || !parseU32(cr_s, &cr, 8) || !parseI32(dbm_s, &dbm)) {
    out("setrf: bad sf/cr/dbm");
    return;
  }
  const float freq = atof(freq_s);
  const float bw   = atof(bw_s);
  if (freq < 150.0f || freq > 960.0f || bw <= 0.0f) {
    out("setrf: freq/bw out of range");
    return;
  }
  const NodePrefs* prefs = s_mesh->getNodePrefs();
  if (!prefs) { out("setrf: no prefs"); return; }
  if (!s_ui->setRadioParams(freq, bw, static_cast<uint8_t>(sf), static_cast<uint8_t>(cr),
                            static_cast<int8_t>(dbm), prefs->airtime_factor)) {
    out("setrf failed");
    return;
  }
  out("radio freq=%.3f bw=%.1f sf=%u cr=%u tx=%d",
      freq, bw, static_cast<unsigned>(sf), static_cast<unsigned>(cr),
      static_cast<int>(dbm));
}

void cmdCapture(char* args) {
  char* name = nextToken(&args);
  const char* shot = (name && *name) ? name : "rt";
  const int W = lv_disp_get_hor_res(nullptr);
  const int H = lv_disp_get_ver_res(nullptr);
  if (W <= 0 || H <= 0) { out("capture: bad display"); return; }

  // Reuse the firmware's own screenshot mirror: arming this buffer makes the
  // LVGL flush callback also copy every flushed area into it, so a forced full
  // redraw yields the composited screen (layers included).
  lv_color_t* buf = (lv_color_t*)heap_caps_malloc((size_t)W * H * sizeof(lv_color_t),
                                                 MALLOC_CAP_SPIRAM);
  if (!buf) buf = (lv_color_t*)malloc((size_t)W * H * sizeof(lv_color_t));
  if (!buf) { out("capture: no buffer"); return; }

  s_ui->remoteShotSetBuffer(buf, W, H);
  lv_obj_invalidate(lv_scr_act());
  lv_obj_invalidate(lv_layer_top());
  lv_obj_invalidate(lv_layer_sys());
  lv_refr_now(nullptr);
  s_ui->remoteShotSetBuffer(nullptr, 0, 0);

  // 16-bit BMP (BI_BITFIELDS, RGB565), rows bottom-up — same framing the
  // upstream DOC_CAPTURE tooling (scripts/doc/capture.py) already understands.
  const uint32_t row_bytes = ((uint32_t)(W * 16 + 31) / 32) * 4;
  const uint32_t img_bytes = row_bytes * (uint32_t)H;
  const uint32_t total     = 66u + img_bytes;
  uint8_t hdr[66];
  memset(hdr, 0, sizeof hdr);
  auto p16 = [&](int o, uint16_t v) { hdr[o] = v & 0xFF; hdr[o + 1] = v >> 8; };
  auto p32 = [&](int o, uint32_t v) {
    hdr[o] = v & 0xFF; hdr[o + 1] = (v >> 8) & 0xFF;
    hdr[o + 2] = (v >> 16) & 0xFF; hdr[o + 3] = (v >> 24) & 0xFF;
  };
  hdr[0] = 'B'; hdr[1] = 'M';
  p32(2, total); p32(10, 66); p32(14, 40);
  p32(18, (uint32_t)W); p32(22, (uint32_t)H);
  p16(26, 1); p16(28, 16); p32(30, 3); p32(34, img_bytes);
  p32(38, 2835); p32(42, 2835);
  p32(54, 0x0000F800); p32(58, 0x000007E0); p32(62, 0x0000001F);

  out("capture begin name=%s w=%d h=%d bytes=%u", shot, W, H, static_cast<unsigned>(total));
  Serial.printf("\n<<<WMSHOT name=%s w=%d h=%d bytes=%u>>>\n", shot, W, H,
                static_cast<unsigned>(total));
  Serial.write(hdr, sizeof hdr);
  static const uint8_t pad[4] = {0, 0, 0, 0};
  const uint32_t pad_n = row_bytes - (uint32_t)W * 2u;
  for (int y = H - 1; y >= 0; --y) {
    Serial.write((const uint8_t*)(buf + (size_t)y * W), (size_t)W * 2u);
    if (pad_n) Serial.write(pad, pad_n);
    if ((y & 0x0F) == 0) esp_task_wdt_reset();
  }
  Serial.print("\n<<<WMEND>>>\n");
  Serial.flush();
  free(buf);
  out("capture end name=%s", shot);
}

void execute(char* line) {
  CommandLine cmd = {};
  const ParseResult r = parseLine(line, &cmd);
  if (r == ParseResult::Empty || r == ParseResult::NotACmd) return;
  if (r == ParseResult::TooLong) { out("err: line too long"); return; }
  if (!isKnownCommand(cmd.command)) { out("err: unknown cmd '%s' (try help)", cmd.command); return; }

  const char* c = cmd.command;
  char* args = cmd.args;
  if (strcmp(c, "help") == 0 || strcmp(c, "?") == 0) cmdHelp();
  else if (strcmp(c, "ping") == 0) out("pong %lu", static_cast<unsigned long>(millis()));
  else if (strcmp(c, "version") == 0) out("wadamesh-remote " __DATE__ " " __TIME__);
  else if (strcmp(c, "screen") == 0) {
    const int count = tabCount();
    const int idx   = tabIndex();
    out("screen tab=%d name=%s tabs=%d off=%d locked=%d", idx, tabName(idx, count), count,
        s_ui->isScreenOff() ? 1 : 0, s_ui->isManualLock() ? 1 : 0);
  } else if (strcmp(c, "nav") == 0 || strcmp(c, "navigate") == 0) cmdNav(args);
  else if (strcmp(c, "tab") == 0) cmdTab(args);
  else if (strcmp(c, "back") == 0) {
    if (tabCount() > 0 && s_ui->remoteSelectTab(2)) out("nav 2 home");
    else out("back: failed");
  } else if (strcmp(c, "tap") == 0) {
    char* xs = nextToken(&args);
    char* ys = nextToken(&args);
    char* hs = nextToken(&args);
    uint32_t x = 0, y = 0, hold = 0;
    if (!parseU32(xs ? xs : "", &x) || !parseU32(ys ? ys : "", &y)) {
      out("tap: <x> <y> [hold_ms]");
      return;
    }
    if (hs && !parseU32(hs, &hold, kTapMaxHoldMs)) { out("tap: bad hold"); return; }
    startTap(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(hold));
  } else if (strcmp(c, "key") == 0 || strcmp(c, "press") == 0) cmdKey(args);
  else if (strcmp(c, "type") == 0 || strcmp(c, "text") == 0) startType(args);
  else if (strcmp(c, "status") == 0 || strcmp(c, "info") == 0) cmdStatus();
  else if (strcmp(c, "tree") == 0 || strcmp(c, "widgets") == 0) {
    int budget = kTreeMaxNodes;
    printTree(lv_scr_act(), 0, &budget);
    out("tree end nodes=%d", kTreeMaxNodes - budget);
  } else if (strcmp(c, "capture") == 0 || strcmp(c, "snapshot") == 0) {
    cmdCapture(args);
  } else if (strcmp(c, "setrf") == 0) cmdSetrf(args);
  else if (strcmp(c, "display") == 0) {
    char* tok = nextToken(&args);
    if (!tok) { out("display: on|off"); return; }
    if (strcmp(tok, "on") == 0) { s_ui->wakeScreen(); out("display on"); }
    else if (strcmp(tok, "off") == 0) { s_ui->sleepScreen(); out("display off"); }
    else out("display: on|off");
  } else if (strcmp(c, "lock") == 0) { s_ui->lockScreen(); out("locked"); }
  else if (strcmp(c, "unlock") == 0) { s_ui->unlockScreen(); out("unlocked"); }
  else if (strcmp(c, "wake") == 0) { s_ui->wakeScreen(); out("awake"); }
  else if (strcmp(c, "reboot") == 0 || strcmp(c, "restart") == 0) {
    out("rebooting");
    Serial.flush();
    delay(120);
    ESP.restart();
  }
}

// ── serial input ───────────────────────────────────────────────────────────
void pollSerial() {
  while (Serial.available()) {
    int b = Serial.peek();
    if (b < 0) return;
    if (!s_line_open) {
      if (b == '\r' || b == '\n') { Serial.read(); continue; }  // stray CRLF
      // Anything that cannot start a text command is a companion frame: leave
      // every byte for the mesh stack.
      if (!isCommandStart(b)) return;
      s_line_open = true;
      s_line_len  = 0;
    }
    b = Serial.read();
    if (b < 0) return;
    if (b == '\r' || b == '\n') {
      s_line[s_line_len] = '\0';
      s_line_open = false;
      s_line_len  = 0;
      execute(s_line);
      continue;
    }
    if (s_line_len + 1 < sizeof(s_line)) {
      s_line[s_line_len++] = static_cast<char>(b);
    } else {
      s_line_open = false;
      s_line_len  = 0;
      out("err: line too long");
    }
  }
}

}  // namespace

void begin(UITask* ui, MyMesh* mesh) {
  s_ui   = ui;
  s_mesh = mesh;
  out("ready build=" __DATE__ " " __TIME__);
  out("type 'help' for commands");
}

void loop() {
  if (!s_ui) return;
  const uint32_t now = millis();
  pollSerial();
  serviceTap(now);
  serviceType(now);
}

}  // namespace remote_test

#endif  // WADAMESH_REMOTE_TEST
