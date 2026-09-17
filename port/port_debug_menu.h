#ifndef PORT_DEBUG_MENU_H
#define PORT_DEBUG_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Renderer;

/* Toggle the debug menu overlay (typically bound to F8). */
void Port_DebugMenu_Toggle(void);

/* Open the overlay straight on the full-screen save-state picker (slot
 * previews, timestamps, load/save on one button each). Bound to
 * PORT_INPUT_STATE_MENU; the picker is a page of this same overlay, so
 * Port_DebugMenu_IsOpen() is true while it is up and closing it is an
 * ordinary Toggle. */
void Port_DebugMenu_OpenStatePicker(int saveMode);
bool Port_DebugMenu_StatePickerOpen(void);
bool Port_DebugMenu_StatePickerIsSave(void);

/* True while the debug menu is on screen. While open, GBA input is masked
 * and key events are routed to the menu instead of the game. */
bool Port_DebugMenu_IsOpen(void);

/* SDL keycode of the just-pressed key. Returns true if the menu consumed
 * the event (caller should suppress further handling). */
bool Port_DebugMenu_HandleKey(int sdlKey);

/* Renders the menu overlay using SDL_RenderDebugText. Call after the
 * game frame texture has been drawn but before SDL_RenderPresent. */
void Port_DebugMenu_Render(struct SDL_Renderer* renderer, int windowWidth, int windowHeight);

/* Lightweight toast/status line used by external subsystems (bug-report,
 * shader preset loader, etc.) without exposing the full page-stack API. */
void Port_DebugMenu_ToastFromExternal(const char* msg);

#ifdef __cplusplus
}
#endif

#endif /* PORT_DEBUG_MENU_H */
