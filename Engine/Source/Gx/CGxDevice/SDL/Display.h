#ifndef ENGINE_SOURCE_GX_CGXDEVICE_SDL_DISPLAY_H
#define ENGINE_SOURCE_GX_CGXDEVICE_SDL_DISPLAY_H

// CHANGED: not in the original client. The monitor the game uses on SDL platforms: the WOW_DISPLAY environment
// variable (an SDL display index, 0 first) when set, otherwise display 0 unless it is in portrait and a
// landscape display exists, in which case the first landscape one. Without this the game always used display
// 0 and opened at desktop position (0, 0), which is whichever monitor sits at the top left.
int GxSdlDisplay();

#endif
