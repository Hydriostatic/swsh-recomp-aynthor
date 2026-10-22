// The look and the settings pages shared by the start screen (runtime/ui/launcher_lg.mm) and the F12 menu
// (runtime/ui/menu.mm), so both look and work alike: the same colours and fonts, and the same rows for the graphics,
// the image and sound, and the advanced settings. Each screen has its own Dear ImGui context; these act on the
// current one. The pages edit g_settings directly (in game they apply at once); the caller saves them.
#pragma once
#include "imgui.h"

// The language: 0 English, 1 Portuguese, 2 Russian (LARECOMP_LANG=en|pt|ru, else the system's)
extern int g_ui_lang;
void ui_pick_language(void);
#define UI_T(en, pt, ru) (g_ui_lang == 2 ? (ru) : g_ui_lang == 1 ? (pt) : (en))

// Fonts into the current context's atlas, built for a 1080 px tall window (ui_scale scales them): Segoe UI Semibold for
// the text (Latin-1 and Cyrillic) at 34 px, a bigger one, and Baloo 2 for titles at 50 (no Cyrillic: Segoe UI for Russian).
struct UiFonts { ImFont *body, *big, *title; };
UiFonts ui_load_fonts(void);
// The look of the game's own menus (its Options screen): dark text on white pills over a lime background with gears,
// the setting under the cursor yellow with a bobbing arrow beside it. ui_style: the colours, rounding and spacing (for
// 1080 px).
void ui_style(void);
// The window's height over 1080 as the scale of the fonts and of the style's sizes (from the unscaled style, kept here)
struct UiScaler { ImGuiStyle base; float scale = -1; };
float ui_scale(UiScaler &s, float window_h);

extern const ImVec4 UI_GOLD, UI_GREEN_TEXT, UI_RED_TEXT, UI_TEXT, UI_HEADING;
// The lime background with gears, stripes and dots (animated), in a rounded rect / behind the current window (right
// after its Begin, with ImGuiCol_WindowBg transparent)
void ui_backdrop(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding);
void ui_window_backdrop(void);
void ui_info_box(ImDrawList *dl, ImVec2 a, ImVec2 b);  // a pastel box with a rainbow frame (the game's descriptions)
void ui_pill(ImDrawList *dl, ImVec2 a, ImVec2 b, bool hot);  // a white pill, or yellow: the cursor's
// Once per frame after the windows, before ImGui::Render: the cursor's arrow beside the row or item it is on
void ui_frame_end(void);
void ui_title(ImFont *title, const char *text);  // a screen's title
void ui_heading(const char *text);               // a group's heading inside a page

// The grid, for 1080 px (scaled with the style): every pill (rows, buttons, the description box) as tall as a frame
// and spanning the page from its left edge to its right one; their text inset by ui_inset from the pill's left end, and
// the page's other text (headings, notes) on that same column. Rows 12 px apart, groups 24 (ui_group_gap). The
// cursor's arrow overlaps a pill's left end and hangs out by about half a frame: keep that much margin outside a page.
float ui_inset(void);
// A page: a child window whose pills reach both its edges and whose items start on the text column
void ui_page_begin(const char *id, ImVec2 size);
void ui_page_end(void);
void ui_group_gap(void);  // between two groups of rows (in place of a separator line)
// A label and the control beside it (the next widget fills the rest of the line), the controls lined up at x - the
// pill's right half, or after the longest label when one doesn't fit - on a pill that turns yellow under the cursor
float ui_label_x(const char *const *labels, int n);
void ui_row(const char *label, float x);
// A choice in a ui_row, as the game's: the value centred, left / right step through them, A opens the list
bool ui_combo(const char *id, int *v, const char *const *items, int n);
bool ui_toggle(const char *id, const char *label, float x, bool *v);  // a ui_row with Off / On
// A number in a ui_row: a slim track with its knob, the value at the row's right end; left / right step by `step`
bool ui_slider(const char *id, float *v, float lo, float hi, const char *fmt, float step);
// A value in a ui_row that is clicked (a key binding): the text centred, `col` its colour (0: the text's)
bool ui_value_button(const char *id, const char *text, ImU32 col);
// A button as a pill on the grid: its left end on the rows' left end, its text on the text column (width 0: the
// text's, -1: the whole row)
bool ui_button(const char *label, float width = 0);
// A row's pill on the current line with no label (the caller lays out its own items on it); true when it has the cursor
bool ui_pill_row(void);
// Whether a pill drawn by the caller has the cursor (the arrow then points at it)
bool ui_hot(ImVec2 a, ImVec2 b);

// The last item's description: a tooltip under the mouse, or (g_ui_tips_in_box) kept for the screen's description box,
// ui_tip_last giving the one of the item under the cursor last frame (NULL: none)
extern bool g_ui_tips_in_box;
void ui_tip(const char *text);
const char *ui_tip_last(void);
// L1 / R1 on a controller: the tab (page) before / after, once per frame
void ui_tabs_begin(int *current, int count);

// The pages. known: the game's build has the address tables the frame rate modes hook (runtime/fnhook.c).
// ui_graphics_x: the column for the graphics page's rows plus the caller's own (extra labels) drawn above it.
float ui_graphics_x(const char *const *extra, int n_extra);
void ui_page_graphics(float x, bool known);
void ui_page_image_sound(void);
void ui_page_advanced(void);
