// The second screen's information (Android: the AYN Thor's bottom screen, android/.../DualScreen.java). Any part of
// the runtime can publish a value; the panel asks for everything a few times a second (panel_snapshot) and draws it.
// Values are grouped in sections, shown in the order a section first appears. Cheap and thread-safe; a no-op cost on
// builds without a second screen (nobody calls panel_snapshot there).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sets (or, with fmt NULL, removes) section / key. Keys keep the order they were first set in.
void panel_set(const char *section, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
// Removes a whole section (e.g. the game's data when it goes back to the start screen).
void panel_clear(const char *section);

// What the runtime is doing, for the panel's header.
enum { PANEL_START_SCREEN, PANEL_BOOTING, PANEL_PLAYING };
void panel_state(int state);

// Everything as text, one value per line: "section\tkey\tvalue\n", the built-in sections first (status, performance)
// then the published ones. Returns the length (the text is cut at size - 1).
size_t panel_snapshot(char *out, size_t size);

#ifdef __cplusplus
}
#endif
