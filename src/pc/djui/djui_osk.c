#include <string.h>
#include "djui.h"
#include "djui_osk.h"
#include "djui_interactable.h"
#include "djui_hud_utils.h"
#include "pc/controller/controller_keyboard.h"
#include "pc/configfile.h"
#include "PR/os_cont.h"
#include "game/game_init.h"

#ifdef HANDHELD

#define OSK_ROWS 5
#define OSK_COLS 10

#define OSK_STICK_DEADZONE 20.0f
#define OSK_DIR_INITIAL_DELAY 0.25f
#define OSK_DIR_REPEAT_DELAY 0.10f
#define OSK_BS_INITIAL_DELAY 0.45f
#define OSK_BS_REPEAT_DELAY 0.08f

enum OskKeyType {
    OSK_KEY_CHAR,
    OSK_KEY_BACKSPACE,
    OSK_KEY_CARET_LEFT,
    OSK_KEY_CARET_RIGHT,
    OSK_KEY_SPACE,
    OSK_KEY_DONE,
};

enum OskPhrase {
    OSK_PHRASE_CHAR_SELECT,
    OSK_PHRASE_HAHAHA,
    OSK_PHRASE_TP,
    OSK_PHRASE_COUNT,
};

static const char* sOskPhrases[OSK_PHRASE_COUNT] = {
    [OSK_PHRASE_CHAR_SELECT] = "/char-select",
    [OSK_PHRASE_HAHAHA]      = "hahaha",
    [OSK_PHRASE_TP]          = "/tp",
};

#define OSK_PHRASE_COLS 4

struct OskKey {
    const char* label;
    u8 span;
    enum OskKeyType type;
    char text[2];
};

static const char* sOskCharRows[OSK_ROWS - 1] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl.",
    "zxcvbnm?/,",
};

static const struct OskKey sOskBottomRow[OSK_COLS] = {
    { "Delete", 2, OSK_KEY_BACKSPACE,   "" },
    { NULL,  0, OSK_KEY_CHAR,         "" },
    { "Space", 4, OSK_KEY_SPACE,      " " },
    { NULL,  0, OSK_KEY_CHAR,         "" },
    { NULL,  0, OSK_KEY_CHAR,         "" },
    { NULL,  0, OSK_KEY_CHAR,         "" },
    { "<",   1, OSK_KEY_CARET_LEFT,  "" },
    { ">",   1, OSK_KEY_CARET_RIGHT, "" },
    { "OK",  2, OSK_KEY_DONE,        "" },
    { NULL,  0, OSK_KEY_CHAR,         "" },
};

static s32 sCursorRow = 0;
static s32 sCursorCol = 0;
static u8 sLastDir = 0;
static f32 sDirTimer = 0.0f;
static bool sBSHeld = false;
static f32 sBSRepeatTimer = 0.0f;
static bool sOskActive = false;
static u16 sLastPadButton = 0;
static u16 sClosingButtons = 0;
static bool sShiftHeld = false;
static bool sPhraseMode = false;
static s32 sCursorPhrase = 0;

static char djui_osk_get_char(s32 row, s32 col) {
    char c = sOskCharRows[row][col];
    if (sShiftHeld) {
        switch (c) {
            case '1': return '!';
            case '2': return '@';
            case '3': return '#';
            case '4': return '$';
            case '5': return '%';
            case '6': return '^';
            case '7': return '&';
            case '8': return '*';
            case '9': return '(';
            case '0': return ')';
            case '.': return '-';
            case '/': return ':';
            case ',': return '_';
            case '?': return '\'';
            default: break;
        }

        if (c >= 'a' && c <= 'z') {
            c = c - ('a' - 'A');
        }
    }
    return c;
}

static bool djui_osk_is_text_input_active(void) {
    return gInteractableFocus != NULL
        && gInteractableFocus->interactable != NULL
        && gInteractableFocus->interactable->on_text_input != NULL;
}

static bool djui_osk_focus_is_chat(void) {
    if (!djui_osk_is_text_input_active()) { return false; }
    struct DjuiInputbox* inputbox = (struct DjuiInputbox*) gInteractableFocus;
    return inputbox->isChatInput;
}

static void djui_osk_get_key(s32 row, s32 col, struct OskKey* key) {
    if (row < OSK_ROWS - 1) {
        char c = djui_osk_get_char(row, col);
        key->label = NULL;
        key->span = 1;
        key->type = OSK_KEY_CHAR;
        key->text[0] = c;
        key->text[1] = '\0';
    } else {
        s32 i;
        for (i = 0; i < OSK_COLS; i++) {
            if (sOskBottomRow[i].span > 0) {
                s32 start = i;
                if (col >= start && col < start + sOskBottomRow[i].span) {
                    break;
                }
            }
        }
        if (i >= OSK_COLS) {
            for (i = 0; i < OSK_COLS; i++) {
                if (sOskBottomRow[i].span > 0 && col < i) { break; }
            }
            if (i >= OSK_COLS) { i = 0; }
        }
        *key = sOskBottomRow[i];
    }
}

static void djui_osk_activate_key(s32 row, s32 col) {
    struct OskKey key;
    djui_osk_get_key(row, col, &key);
    switch (key.type) {
        case OSK_KEY_CHAR:
            djui_interactable_on_text_input(key.text);
            break;
        case OSK_KEY_SPACE:
            djui_interactable_on_text_input(key.text);
            break;
        case OSK_KEY_BACKSPACE:
            djui_interactable_on_key_down(SCANCODE_BACKSPACE);
            break;
        case OSK_KEY_CARET_LEFT:
            djui_interactable_on_key_down(SCANCODE_LEFT);
            break;
        case OSK_KEY_CARET_RIGHT:
            djui_interactable_on_key_down(SCANCODE_RIGHT);
            break;
        case OSK_KEY_DONE:
            djui_interactable_on_key_down(SCANCODE_ENTER);
            break;
    }
}

static s32 djui_osk_next_col(s32 row, s32 col, s32 step) {
    if (row < OSK_ROWS - 1) {
        s32 newCol = col + step;
        if (newCol < 0) { newCol = OSK_COLS - 1; }
        if (newCol >= OSK_COLS) { newCol = 0; }
        return newCol;
    }
    s32 i = 0;
    for (; i < OSK_COLS; i++) {
        if (sOskBottomRow[i].span > 0 && col >= i && col < i + sOskBottomRow[i].span) { break; }
    }
    if (i >= OSK_COLS) { i = 0; }
    while (true) {
        i += step;
        if (i < 0) {
            i = OSK_COLS - 1;
            while (i >= 0 && sOskBottomRow[i].span == 0) { i--; }
            return i;
        }
        if (i >= OSK_COLS) {
            i = 0;
            while (i < OSK_COLS && sOskBottomRow[i].span == 0) { i++; }
            return i;
        }
        if (sOskBottomRow[i].span > 0) { return i; }
    }
}

static void djui_osk_move_cursor(s32 dRow, s32 dCol) {
    if (sPhraseMode) {
        if (dRow < 0) {
            sPhraseMode = false;
        } else if (dRow > 0) {
            sCursorPhrase = (sCursorPhrase + 1) % OSK_PHRASE_COUNT;
        } else if (dCol != 0) {
            sPhraseMode = false;
        }
        return;
    }

    // Enter phrase mode when pressing down from the bottom action row (chat only).
    if (dRow > 0 && sCursorRow == OSK_ROWS - 1 && djui_osk_focus_is_chat()) {
        sPhraseMode = true;
        sCursorPhrase = 0;
        return;
    }

    s32 newRow = sCursorRow + dRow;
    if (newRow < 0) { newRow = OSK_ROWS - 1; }
    if (newRow >= OSK_ROWS) { newRow = 0; }
    sCursorRow = newRow;
    if (dCol != 0) {
        sCursorCol = djui_osk_next_col(sCursorRow, sCursorCol, dCol);
    } else if (sCursorRow == OSK_ROWS - 1) {
        sCursorCol = djui_osk_next_col(sCursorRow, sCursorCol, 0);
    }
}

static u8 djui_osk_get_direction(OSContPad* pad) {
    if (pad->stick_x < -OSK_STICK_DEADZONE) { return 1; }
    if (pad->stick_x >  OSK_STICK_DEADZONE) { return 2; }
    if (pad->stick_y < -OSK_STICK_DEADZONE) { return 3; }
    if (pad->stick_y >  OSK_STICK_DEADZONE) { return 4; }
    if (pad->button & U_JPAD) { return 3; }
    if (pad->button & D_JPAD) { return 4; }
    if (pad->button & L_JPAD) { return 1; }
    if (pad->button & R_JPAD) { return 2; }
    return 0;
}

static void djui_osk_consume_input(void) {
    gInteractablePad.button = 0;
    gInteractablePad.stick_x = 0;
    gInteractablePad.stick_y = 0;
    // Also consume the keyboard-emulation buttons the OSK acted on, so the
    // later interactable pass doesn't re-deliver them. L/R triggers (shift)
    // are left intact since that's a held state owned by the keyboard module.
    djui_interactable_consume_osk_buttons();
}

void djui_osk_update(void) {
    bool wasOskActive = sOskActive;
    OSContPad pad;
    djui_interactable_get_merged_pad(&pad);
    u16 button = pad.button;

    sOskActive = djui_osk_is_text_input_active();
    if (!sOskActive) {
        sClosingButtons &= button;
        gInteractablePad.button &= ~sClosingButtons;
        sLastDir = 0;
        sBSHeld = false;
        sShiftHeld = false;
        sPhraseMode = false;
        sCursorPhrase = 0;
        sLastPadButton = button;
        return;
    }

    sShiftHeld = (button & (L_TRIG | R_TRIG)) != 0;

    if (!wasOskActive) {
        sLastPadButton = button;
        sPhraseMode = false;
        sCursorPhrase = 0;
        djui_osk_consume_input();
        return;
    }

    u16 pressed = button & ~sLastPadButton;

    u8 dir = djui_osk_get_direction(&pad);
    if (dir != sLastDir) {
        if (dir != 0) {
            switch (dir) {
                case 1: djui_osk_move_cursor(0, -1); break;
                case 2: djui_osk_move_cursor(0,  1); break;
                case 3: djui_osk_move_cursor(-1, 0); break;
                case 4: djui_osk_move_cursor(1,  0); break;
            }
            sDirTimer = OSK_DIR_INITIAL_DELAY;
        }
        sLastDir = dir;
    } else if (dir != 0) {
        sDirTimer -= 1.0f / 60.0f;
        if (sDirTimer <= 0.0f) {
            switch (dir) {
                case 1: djui_osk_move_cursor(0, -1); break;
                case 2: djui_osk_move_cursor(0,  1); break;
                case 3: djui_osk_move_cursor(-1, 0); break;
                case 4: djui_osk_move_cursor(1,  0); break;
            }
            sDirTimer += OSK_DIR_REPEAT_DELAY;
        }
    }

    if (pressed & PAD_BUTTON_A) {
        if (sPhraseMode) {
            djui_interactable_on_text_input((char*) sOskPhrases[sCursorPhrase]);
        } else {
            djui_osk_activate_key(sCursorRow, sCursorCol);
            if (!djui_osk_is_text_input_active()) {
                sOskActive = false;
                sClosingButtons = button & PAD_BUTTON_A;
                sLastPadButton = button;
                djui_osk_consume_input();
                return;
            }
        }
    }

    if (pressed & PAD_BUTTON_B) {
        if (sPhraseMode) {
            sPhraseMode = false;
            sLastPadButton = button;
            djui_osk_consume_input();
            return;
        }
        djui_interactable_on_key_down(SCANCODE_BACKSPACE);
        sBSHeld = true;
        sBSRepeatTimer = OSK_BS_INITIAL_DELAY;
    } else if (sBSHeld && (button & PAD_BUTTON_B)) {
        sBSRepeatTimer -= 1.0f / 60.0f;
        if (sBSRepeatTimer <= 0.0f) {
            djui_interactable_on_key_down(SCANCODE_BACKSPACE);
            sBSRepeatTimer += OSK_BS_REPEAT_DELAY;
        }
    } else {
        sBSHeld = false;
    }

    if (pressed & PAD_BUTTON_START) {
        djui_interactable_set_input_focus(NULL);
        sOskActive = false;
        sClosingButtons = button & PAD_BUTTON_START;
    }

    sLastPadButton = button;
    djui_osk_consume_input();
}

void djui_osk_render(void) {
    if (!sOskActive) { return; }

    u32 sw = djui_hud_get_screen_width();
    u32 sh = djui_hud_get_screen_height();
    f32 margin = sw * 0.005f;
    f32 gap = sw * 0.003f;
    f32 keyW = (sw - 2.0f * margin - (OSK_COLS - 1) * gap) / OSK_COLS;
    f32 keyH = sh * 0.055f;
    f32 pad = 4.0f;
    f32 panelH = OSK_ROWS * keyH + (OSK_ROWS - 1) * gap + 2.0f * pad;
    f32 x0 = margin;

    // Default to the top of the screen (out of the way of the bottom-aligned
    // chat box), but if the focused input itself sits in the top half of the
    // screen (e.g. a menu textbox), draw at the bottom instead so the
    // keyboard doesn't cover it. A textbox near the vertical center of the
    // screen may still end up partially covered either way.
    f32 y0 = margin;
    if (gInteractableFocus != NULL) {
        f32 focusCenterY = gInteractableFocus->comp.y + gInteractableFocus->comp.height * 0.5f;
        if (focusCenterY < sh * 0.5f) {
            y0 = sh - panelH - margin;
        }
    }

    djui_hud_set_font(FONT_ALIASED);
    djui_hud_set_text_alignment(0.5f, 0.5f);

    s32 row, col;

    for (row = 0; row < OSK_ROWS; row++) {
        s32 keyStart = 0;
        for (col = 0; col < OSK_COLS; col++) {
            struct OskKey key;
            bool isSelected = (row == sCursorRow && col == sCursorCol);
            if (row < OSK_ROWS - 1) {
                djui_osk_get_key(row, col, &key);
            } else {
                djui_osk_get_key(row, col, &key);
                if (sOskBottomRow[col].span == 0) { continue; }
            }

            f32 kx = x0 + keyStart * (keyW + gap);
            f32 ky = y0 + pad + row * (keyH + gap);
            f32 kw = keyW * key.span + gap * (key.span - 1);
            if (row < OSK_ROWS - 1) { kw = keyW; }

            struct DjuiColor bgColor = isSelected
                ? (struct DjuiColor) { 70, 70, 70, 255 }
                : (struct DjuiColor) { 35, 35, 35, 255 };
            struct DjuiColor borderColor = isSelected
                ? (struct DjuiColor) { 255, 255, 255, 255 }
                : (struct DjuiColor) { 110, 110, 110, 255 };

            djui_hud_set_color(borderColor.r, borderColor.g, borderColor.b, borderColor.a);
            djui_hud_render_rect(kx - 2.0f, ky - 2.0f, kw + 4.0f, keyH + 4.0f);
            djui_hud_set_color(bgColor.r, bgColor.g, bgColor.b, bgColor.a);
            djui_hud_render_rect(kx, ky, kw, keyH);

            keyStart += key.span;
            col += key.span - 1;
        }
    }

    djui_hud_set_text_alignment(0.5f, 0.5f);
    djui_hud_set_color(255,255,255,255);
    for (row = 0; row < OSK_ROWS; row++) {
        s32 keyStart = 0;
        for (col = 0; col < OSK_COLS; col++) {
            struct OskKey key;
            if (row < OSK_ROWS - 1) {
                djui_osk_get_key(row, col, &key);
            } else {
                djui_osk_get_key(row, col, &key);
                if (sOskBottomRow[col].span == 0) { continue; }
            }

            f32 kx = x0 + keyStart * (keyW + gap);
            f32 ky = y0 + pad + row * (keyH + gap);
            f32 kw = keyW * key.span + gap * (key.span - 1);
            if (row < OSK_ROWS - 1) { kw = keyW; }

            const char* label = key.label;
            char charLabel[2];
            if (row < OSK_ROWS - 1) {
                charLabel[0] = djui_osk_get_char(row, col);
                charLabel[1] = '\0';
                label = charLabel;
            }

            djui_hud_set_text_color(250, 250, 250, 255);
            djui_hud_print_text(label, kx + kw / 2.0f, ky + keyH / 2.0f, 1.15f, 1.15f);

            keyStart += key.span;
            col += key.span - 1;
        }
    }

    if (djui_osk_focus_is_chat()) {
        f32 phraseW = keyW * OSK_PHRASE_COLS + gap * (OSK_PHRASE_COLS - 1);
        f32 px = x0 + (OSK_COLS - OSK_PHRASE_COLS) * (keyW + gap);
        f32 py0 = y0 + pad + (OSK_ROWS - 1) * (keyH + gap) + keyH + gap;

        for (s32 i = 0; i < OSK_PHRASE_COUNT; i++) {
            f32 py = py0 + i * (keyH + gap);
            bool isSelected = sPhraseMode && sCursorPhrase == i;

            struct DjuiColor bgColor = isSelected
                ? (struct DjuiColor) { 70, 70, 70, 255 }
                : (struct DjuiColor) { 35, 35, 35, 255 };
            struct DjuiColor borderColor = isSelected
                ? (struct DjuiColor) { 255, 255, 255, 255 }
                : (struct DjuiColor) { 110, 110, 110, 255 };

            djui_hud_set_color(borderColor.r, borderColor.g, borderColor.b, borderColor.a);
            djui_hud_render_rect(px - 2.0f, py - 2.0f, phraseW + 4.0f, keyH + 4.0f);
            djui_hud_set_color(bgColor.r, bgColor.g, bgColor.b, bgColor.a);
            djui_hud_render_rect(px, py, phraseW, keyH);

            const char* label = sOskPhrases[i];

            djui_hud_set_color(255, 255, 255, 255);
            djui_hud_set_text_alignment(0.5f, 0.5f);
            djui_hud_set_text_color(250, 250, 250, 255);
            djui_hud_print_text(label, px + phraseW / 2.0f, py + keyH / 2.0f, 1.0f, 1.0f);
        }
    }
}

#endif // HANDHELD
