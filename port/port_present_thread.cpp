#include "port_present_thread.h"

#include "port_runtime_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* Two staging buffers, not one: the worker reads whichever it took while the
 * game thread writes the other. A third would only help if the game could
 * produce two frames inside one present, which would mean the display is
 * more than a full frame behind -- at that point dropping is the right
 * answer anyway, so latest-wins over two buffers is enough. */
constexpr int kStagingCount = 2;

struct Staging {
    uint32_t* pixels = nullptr;
    size_t capacityBytes = 0;
    int w = 0;
    int h = 0;
};

SDL_Renderer* sRenderer = nullptr;
SDL_Thread* sThread = nullptr;
SDL_Mutex* sMutex = nullptr;
SDL_Condition* sWake = nullptr; /* worker waits for a job */
SDL_Condition* sIdle = nullptr; /* Drain waits for the worker */

Staging sStaging[kStagingCount];
PortPresentJob sPending;      /* geometry of the frame waiting to go out */
int sPendingIdx = -1;         /* staging slot holding it, -1 = nothing pending */
int sWorkerIdx = -1;          /* slot the worker is reading right now */
bool sRunning = false;
bool sBusy = false;

/* Worker-owned. The main thread must never touch these. */
SDL_Texture* sTex = nullptr;
int sTexW = 0;
int sTexH = 0;
SDL_ScaleMode sTexScale = SDL_SCALEMODE_NEAREST;

uint64_t sLastPresentNs = 0;
uint64_t sDropped = 0;

void FreeStaging(void) {
    for (Staging& s : sStaging) {
        SDL_free(s.pixels);
        s.pixels = nullptr;
        s.capacityBytes = 0;
        s.w = s.h = 0;
    }
}

/* Worker thread only. */
bool EnsureTexture(int w, int h) {
    if (sTex != nullptr && sTexW == w && sTexH == h) {
        return true;
    }
    if (sTex != nullptr) {
        SDL_DestroyTexture(sTex);
        sTex = nullptr;
    }
    sTex = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (sTex == nullptr) {
        std::fprintf(stderr, "[present] SDL_CreateTexture(%dx%d) failed: %s\n", w, h, SDL_GetError());
        sTexW = sTexH = 0;
        return false;
    }
    sTexW = w;
    sTexH = h;
    /* Freshly created textures default to LINEAR; force the cache to
     * disagree so the first frame always sets the mode it actually wants. */
    sTexScale = SDL_SCALEMODE_LINEAR;
    SDL_SetTextureScaleMode(sTex, SDL_SCALEMODE_LINEAR);
    return true;
}

void SetScaleModeCached(SDL_ScaleMode mode) {
    if (sTexScale != mode) {
        sTexScale = mode;
        SDL_SetTextureScaleMode(sTex, mode);
    }
}

/* Worker thread only: the whole compose + blit, mirroring the synchronous
 * path in Port_PPU_PresentFrame. Kept in step with it by construction --
 * both consume the same PortPresentJob geometry. */
void PresentJob(const PortPresentJob& job, const Staging& src) {
    if (!EnsureTexture(job.w, job.h)) {
        return;
    }
    /* Timed from the upload, not from the blit: the whole of this function
     * is what the game thread used to pay for. */
    const uint64_t t0 = SDL_GetTicksNS();
    SDL_UpdateTexture(sTex, nullptr, src.pixels, job.w * (int)sizeof(uint32_t));

    /* Clear-to-black covers everything outside the stage rect; inside it the
     * chosen background fill applies, and the sharp frame lands on top. */
    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);

    if (job.bgFill == PORT_BG_FILL_SOLID_COLOR) {
        SDL_SetRenderDrawColor(sRenderer, job.bgR, job.bgG, job.bgB, 255);
        SDL_RenderFillRect(sRenderer, &job.stage);
    } else if (job.bgFill == PORT_BG_FILL_BLURRED_FRAME) {
        SetScaleModeCached(SDL_SCALEMODE_LINEAR);
        SDL_RenderTexture(sRenderer, sTex, nullptr, &job.stage);
    }

    SetScaleModeCached(job.scale);
    SDL_RenderTexture(sRenderer, sTex, nullptr, &job.dst);

    if (!SDL_RenderPresent(sRenderer)) {
        static uint32_t sLastErrLog = 0;
        const uint32_t now = SDL_GetTicks();
        if (now - sLastErrLog > 2000) {
            sLastErrLog = now;
            std::fprintf(stderr, "[present] SDL_RenderPresent FAILED: %s\n", SDL_GetError());
        }
    }
    sLastPresentNs = SDL_GetTicksNS() - t0;
}

int SDLCALL WorkerMain(void* /*unused*/) {
    for (;;) {
        PortPresentJob job;
        int idx;

        SDL_LockMutex(sMutex);
        while (sRunning && sPendingIdx < 0) {
            SDL_WaitCondition(sWake, sMutex);
        }
        if (!sRunning) {
            SDL_UnlockMutex(sMutex);
            break;
        }
        job = sPending;
        idx = sPendingIdx;
        sPendingIdx = -1;
        sWorkerIdx = idx;
        sBusy = true;
        SDL_UnlockMutex(sMutex);

        PresentJob(job, sStaging[idx]);

        SDL_LockMutex(sMutex);
        sBusy = false;
        sWorkerIdx = -1;
        SDL_BroadcastCondition(sIdle);
        SDL_UnlockMutex(sMutex);
    }

    /* Textures belong to the thread that created them as far as this module
     * is concerned; drop it here rather than leaving the main thread to. */
    if (sTex != nullptr) {
        SDL_DestroyTexture(sTex);
        sTex = nullptr;
        sTexW = sTexH = 0;
    }
    return 0;
}

} /* namespace */

bool Port_PresentThread_Start(SDL_Renderer* renderer) {
    if (sThread != nullptr) {
        return true;
    }
    if (renderer == nullptr) {
        return false;
    }

    sRenderer = renderer;
    sMutex = SDL_CreateMutex();
    sWake = SDL_CreateCondition();
    sIdle = SDL_CreateCondition();
    if (sMutex == nullptr || sWake == nullptr || sIdle == nullptr) {
        std::fprintf(stderr, "[present] sync primitives failed: %s\n", SDL_GetError());
        Port_PresentThread_Stop();
        return false;
    }

    sRunning = true;
    sPendingIdx = -1;
    sWorkerIdx = -1;
    sBusy = false;
    sThread = SDL_CreateThread(WorkerMain, "tmc-present", nullptr);
    if (sThread == nullptr) {
        std::fprintf(stderr, "[present] SDL_CreateThread failed: %s\n", SDL_GetError());
        sRunning = false;
        Port_PresentThread_Stop();
        return false;
    }
    std::fprintf(stderr, "[present] worker thread started (window blit is off the game thread)\n");
    return true;
}

void Port_PresentThread_Stop(void) {
    if (sThread != nullptr) {
        SDL_LockMutex(sMutex);
        sRunning = false;
        SDL_BroadcastCondition(sWake);
        SDL_UnlockMutex(sMutex);
        SDL_WaitThread(sThread, nullptr);
        sThread = nullptr;
    }
    sRunning = false;

    if (sIdle != nullptr) {
        SDL_DestroyCondition(sIdle);
        sIdle = nullptr;
    }
    if (sWake != nullptr) {
        SDL_DestroyCondition(sWake);
        sWake = nullptr;
    }
    if (sMutex != nullptr) {
        SDL_DestroyMutex(sMutex);
        sMutex = nullptr;
    }
    FreeStaging();
    sRenderer = nullptr;
    sPendingIdx = -1;
    sWorkerIdx = -1;
    sBusy = false;
}

bool Port_PresentThread_Active(void) {
    return sThread != nullptr && sRunning;
}

bool Port_PresentThread_Submit(const PortPresentJob* job) {
    if (job == nullptr || job->pixels == nullptr || job->w <= 0 || job->h <= 0) {
        return false;
    }
    if (!Port_PresentThread_Active()) {
        return false;
    }

    SDL_LockMutex(sMutex);

    /* Any slot the worker is not reading. With two slots and a single
     * producer this always finds one. A frame still pending here has not
     * been shown and never will be -- newer pixels supersede it. */
    int idx = 0;
    for (int i = 0; i < kStagingCount; ++i) {
        if (i != sWorkerIdx) {
            idx = i;
            break;
        }
    }
    if (sPendingIdx >= 0) {
        ++sDropped;
    }

    Staging& s = sStaging[idx];
    const size_t needBytes = (size_t)job->w * (size_t)job->h * sizeof(uint32_t);
    if (s.capacityBytes < needBytes) {
        uint32_t* grown = (uint32_t*)SDL_realloc(s.pixels, needBytes);
        if (grown == nullptr) {
            SDL_UnlockMutex(sMutex);
            return false; /* caller falls back to a synchronous present */
        }
        s.pixels = grown;
        s.capacityBytes = needBytes;
    }
    s.w = job->w;
    s.h = job->h;

    /* Tight rows: the worker uploads with a pitch of w*4 regardless of what
     * the source used, so pack while copying. */
    const int rowBytes = job->w * (int)sizeof(uint32_t);
    if (job->pitchBytes == rowBytes) {
        std::memcpy(s.pixels, job->pixels, needBytes);
    } else {
        const uint8_t* src = (const uint8_t*)job->pixels;
        uint8_t* dst = (uint8_t*)s.pixels;
        for (int y = 0; y < job->h; ++y) {
            std::memcpy(dst + (size_t)y * (size_t)rowBytes, src + (size_t)y * (size_t)job->pitchBytes,
                        (size_t)rowBytes);
        }
    }

    sPending = *job;
    sPending.pixels = nullptr; /* the staging slot owns the pixels now */
    sPending.pitchBytes = rowBytes;
    sPendingIdx = idx;
    SDL_SignalCondition(sWake);
    SDL_UnlockMutex(sMutex);
    return true;
}

void Port_PresentThread_Drain(void) {
    if (sMutex == nullptr) {
        return;
    }
    SDL_LockMutex(sMutex);
    while (sRunning && (sPendingIdx >= 0 || sBusy)) {
        SDL_WaitCondition(sIdle, sMutex);
    }
    SDL_UnlockMutex(sMutex);
}

uint64_t Port_PresentThread_LastPresentNs(void) {
    return sLastPresentNs;
}

uint64_t Port_PresentThread_DroppedFrames(void) {
    return sDropped;
}
