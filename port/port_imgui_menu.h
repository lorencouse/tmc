#pragma once

#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void Port_ImGui_Init(SDL_Window* window, SDL_Renderer* renderer);
void Port_ImGui_Shutdown(void);

bool Port_ImGui_CanPresent(void);
bool Port_ImGui_WantsTextInput(void);
void Port_ImGui_HandleEvent(const SDL_Event* event);

/* True when the settings menu draws its console/handheld shell (one
 * category at a time, D-pad + A/B) instead of the desktop ribbon. */
bool Port_ImGui_ConsoleMode(void);

/* Feed one SDL event through the player's *own* button bindings so the
 * settings menu can be driven with A / B / D-pad / L / R. Returns true
 * when the event was consumed by the menu and must not reach the game.
 * Only call while the menu is open. */
bool Port_ImGui_HandleGameInputEvent(const SDL_Event* event);

bool Port_ImGui_IsEnabled(void);
bool Port_ImGui_RibbonEnabled(void);
void Port_ImGui_SetRibbonEnabled(bool enabled);

bool Port_ImGui_QuitConfirmed(void);
void Port_ImGui_RequestQuitModal(void);

bool Port_ImGui_Render(void);
bool Port_ImGui_RenderPrelaunch(bool romPresent,
                                const char* version,
                                const char* romName,
                                bool* outPlay,
                                bool* outChangeRom);

#ifdef TMC_GPU_RENDERER
void Port_ImGui_PrepareDrawDataGpu(SDL_GPUCommandBuffer* cmd);
void Port_ImGui_RenderDrawDataGpu(SDL_GPUCommandBuffer* cmd,
                                  SDL_GPURenderPass* renderPass);
#endif

#ifdef __cplusplus
}
#endif
