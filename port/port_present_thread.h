#ifndef PORT_PRESENT_THREAD_H
#define PORT_PRESENT_THREAD_H

/*
 * Off-thread window blit for the SDL_Renderer backend.
 *
 * Why this exists: on a PortMaster/muOS handheld the game draws through
 * Xwayland to a weston compositor, and that X socket has no MIT-SHM (the
 * kernel is built without SysV shared memory -- `ipcs -m` says so and there
 * is no /proc/sysvipc). Every present therefore copies the whole window
 * through the socket. Measured on an RG35XX SP: ~5 ms while frames are
 * streaming and up to ~17 ms once the compositor has gone idle. Against a
 * 16.67 ms tick that is the single largest item in the frame, it lands on
 * the game thread, and no amount of pacing can hide it -- the decoupled
 * pacer's cost-fit check just refuses the next present, which makes the one
 * after that dearer still.
 *
 * So the copy moves to its own thread. The game thread rasters and prescales
 * as before, hands the finished pixels over, and returns immediately; the
 * worker owns the texture and does upload -> clear -> compose -> present.
 * Handoff is latest-wins: if the worker is still busy the pending frame is
 * overwritten rather than queued, so the engine never blocks on the display
 * and a slow present costs display rate, never game speed.
 *
 * Threading rules this module relies on:
 *   - SDL_Renderer is not thread-safe. Exactly one thread touches it at a
 *     time: the worker while a job is in flight, the main thread otherwise.
 *     Any main-thread renderer use must Drain() first.
 *   - Overlays (ImGui, soft slots, touch controls) draw through the same
 *     renderer AND read/write game state, so they can never run on the
 *     worker. A frame that needs one falls back to a synchronous present --
 *     see Port_PPU_PresentFrame.
 */

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One frame's worth of work. `pixels` is consumed (copied into the module's
 * own staging buffer) before Submit returns, so the caller may reuse its
 * scratch immediately. */
typedef struct PortPresentJob {
    const uint32_t* pixels;
    int w;
    int h;
    int pitchBytes;
    SDL_FRect stage; /* aspect-fit area; background fill applies inside it */
    SDL_FRect dst;   /* where the game frame itself lands */
    SDL_ScaleMode scale;
    int bgFill; /* PortBgFill */
    uint8_t bgR, bgG, bgB;
} PortPresentJob;

/* Spin the worker up against an existing renderer. Safe to call twice (the
 * second call is a no-op). Returns false if the thread could not start, in
 * which case every Submit will refuse and the caller keeps presenting
 * synchronously. */
bool Port_PresentThread_Start(SDL_Renderer* renderer);

/* Park the worker and join it. Drains any in-flight present first, so the
 * renderer is safe for the main thread to destroy on return. */
void Port_PresentThread_Stop(void);

/* True when the worker is running and able to take frames. */
bool Port_PresentThread_Active(void);

/* Hand a frame over. Returns false if the worker is not running -- present
 * it synchronously in that case. Never blocks on the display: at worst it
 * blocks for the staging memcpy. */
bool Port_PresentThread_Submit(const PortPresentJob* job);

/* Block until nothing is in flight. Required before the main thread touches
 * the renderer for any reason (synchronous present, vsync change, teardown).
 * Cheap and safe when the worker is idle or not running. */
void Port_PresentThread_Drain(void);

/* Rolling cost of the worker's own present, in nanoseconds, for the
 * TMC_PACE_LOG / TMC_PROFILE reports. This is the cost the game thread no
 * longer pays. Zero before the first present. */
uint64_t Port_PresentThread_LastPresentNs(void);

/* Frames handed over that the worker never showed because a newer one
 * arrived first. A steady climb means the display is the bottleneck and the
 * engine is (correctly) running ahead of it. */
uint64_t Port_PresentThread_DroppedFrames(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PRESENT_THREAD_H */
