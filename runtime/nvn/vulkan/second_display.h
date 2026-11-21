// A second panel beside the game's window, on a thread of its own: second_display.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef MHGU_SECOND_DISPLAY

// Once, from backend_init, with the game's GPU (vendorID, deviceID). enabled false ([Dual Screen] SecondDisplay Off,
// MHGU_SECOND_DISPLAY=0): returns at once and nothing else in this file ever runs. Otherwise it starts the thread and
// returns at once too: the lease and all the Vulkan work happen there, and a failure only means no second panel.
void second_display_init(bool enabled, uint32_t vendor, uint32_t device);


// The touch reader's thread (third_party/azahar-drm-lease): a finger at panel pixel x, y, or lifted.
void second_display_touch(bool down, unsigned x, unsigned y);

// The wallpaper to show now, w x h (upright, RGBA as the bottom picture, scaled to cover), from wallpapers/ beside the
// program: the [Dual Screen] Wallpaper, or when it is empty each BMP there in turn, 30 s each, fading over 1 s into the
// next. All of them load once, when the setting changes (checked here). NULL: Background is not Wallpaper or no file.
// *changed: not the last call's (every call during a fade).
// One thread only (the panel's, or the test mode's without a panel).
const uint32_t *second_display_wallpaper(uint32_t w, uint32_t h, bool *changed);

// The picture: the game's HUD groups drawn again into an image of the panel's size (bottom_hud.c), handed over as host
// memory the GPU has finished writing, never by waiting.
#endif
