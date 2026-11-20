#pragma once
// ============================================================================
// wadamesh remote-test controller — public entry points.
//
// Entirely additive: nothing here exists unless the build sets
// -D WADAMESH_REMOTE_TEST=1 (env LilyGo_TDeck_companion_radio_touch_remote).
// See docs/REMOTE_TEST.md for the protocol and the three upstream hook sites.
// ============================================================================
#include <stdint.h>

#if defined(WADAMESH_REMOTE_TEST) && WADAMESH_REMOTE_TEST

class UITask;
class MyMesh;

namespace remote_test {

// Register the controller. Call once, after ui_task.begin().
void begin(UITask* ui, MyMesh* mesh);

// Service serial input + long-running jobs. Call once per main-loop pass.
void loop();

}  // namespace remote_test

#endif  // WADAMESH_REMOTE_TEST
