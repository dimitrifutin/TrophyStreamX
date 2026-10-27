#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <obs-module.h>

#include <util/platform.h>
#include <media-io/video-io.h>

#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <urlmon.h>
#include <wincodec.h>
#include <bcrypt.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "bcrypt.lib")

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("trophy-stream", "en-US")

/*
 * ============================================================
 * TrophyStream
 * Source vidéo native OBS
 *
 * Backend :
 *     ws://127.0.0.1:3002
 *
 * Message attendu :
 *
 * {
 *   "type": "trophy",
 *   "unlocked": 43,
 *   "total": 87,
 *   "percent": 49.425
 * }
 * ============================================================
 */

#define TROPHY_WIDTH  360
#define TROPHY_HEIGHT 150

#define TROPHY_WS_HOST "127.0.0.1"
#define TROPHY_WS_PORT 3002

/*
 * ------------------------------------------------------------
 * Etat global du plugin
 * ------------------------------------------------------------
 */

static CRITICAL_SECTION progress_lock;

static int notion_platinum_games = 0;
static int notion_games_played = 0;
static bool notion_stats_valid = false;
static bool trophy_stream_display_enabled = true;

static float current_progress = 0.50f;
static char current_trophy_type[32] = "gold";
static char current_trophy_name[256] = "TrophyStream";
static char current_trophy_detail[512] = "En attente...";

static int current_unlocked = 0;
static int current_total = 0;

#define TROPHY_MAX_REMAINING 128

static int current_remaining_count = 0;
static int current_remaining_index = 0;
static uint64_t remaining_rotation_next_tick = 0;
static char current_remaining_names[TROPHY_MAX_REMAINING][256];
static char current_remaining_details[TROPHY_MAX_REMAINING][512];
static char current_remaining_types[TROPHY_MAX_REMAINING][32];
static char current_remaining_icons[TROPHY_MAX_REMAINING][1024];

static bool current_trophy_visible = false;
static uint64_t current_trophy_duration = 10000;

/*
 * true uniquement lorsqu'un jeu possède un état de trophées valide.
 * Le backend utilise 0/0 pour signaler qu'aucun jeu n'est détecté.
 */
static bool current_game_active = false;

/*
 * Identifiant du jeu actuellement affiché.
 *
 * Format :
 *   npCommunicationId:npServiceName
 *
 * Exemple :
 *   NPWR29894_00:trophy2
 *
 * Permet de détecter immédiatement un changement de jeu.
 * de jeu et de nettoyer l'ancienne animation.
 */
static char current_game_id[256] = "";
static char current_game_title[256] = "";

/*
 * Ecran final permanent apres obtention du platine.
 * Il apparait uniquement apres la fin de l animation du platine
 * et disparait des qu un changement de jeu est detecte.
 */
static bool platinum_final_visible = false;
static bool current_trophy_is_platinum_completion = false;

/*
 * Animation de changement de jeu.
 */
static bool game_change_visible = false;
static uint64_t game_change_show_time = 0;

/*
 * Donnees FIGEES de l'animation "NOUVEAU JEU".
 * Les STATE et les trophees qui arrivent ensuite ne peuvent pas
 * modifier ce que l'animation est en train d'afficher.
 */
static char game_change_display_title[256] = "";
static uint8_t game_change_icon_pixels[128 * 128 * 4];
static int game_change_icon_width = 0;
static int game_change_icon_height = 0;
static bool game_change_icon_valid = false;

static uint64_t trophy_show_time = 0;
static bool trophy_paused_for_game_change = false;
static uint64_t trophy_paused_elapsed = 0;
static char trophy_paused_icon_url[1024] = "";

/*
 * Image réelle du trophée PlayStation.
 */
static CRITICAL_SECTION icon_lock;
static char current_trophy_icon_url[1024] = "";
static uint8_t current_icon_pixels[128 * 128 * 4];
static int current_icon_width = 0;
static int current_icon_height = 0;
static bool current_icon_valid = false;

/*
 * File d'attente des trophées.
 *
 * Si plusieurs trophées arrivent pendant une animation,
 * ils sont conservés dans l'ordre d'arrivée et joués
 * un par un, sans en perdre aucun.
 */
#define TROPHY_ANIMATION_QUEUE_MAX 32

typedef struct trophy_animation_item {
    char type[32];
    char name[256];
    char detail[512];
    char icon_url[1024];
    int unlocked;
    int total;
    float progress;
    uint64_t duration;
} trophy_animation_item;

static trophy_animation_item
    trophy_animation_queue[TROPHY_ANIMATION_QUEUE_MAX];

static int trophy_animation_queue_count = 0;

static HANDLE websocket_thread = NULL;
static HANDLE websocket_stop_event = NULL;

static volatile LONG plugin_running = 0;

/*
 * ------------------------------------------------------------
 * Petit parser JSON
 * ------------------------------------------------------------
 *
 * On ne dépend volontairement d'aucune bibliothèque JSON.
 *
 * Nous cherchons simplement :
 *
 *     "percent": 49.425
 *
 * puis, si absent :
 *
 *     "unlocked": 43
 *     "total": 87
 * ------------------------------------------------------------
 */

static bool json_get_number(
    const char *json,
    const char *key,
    double *value)
{
    if (!json || !key || !value)
        return false;

    char search[128];

    snprintf(
        search,
        sizeof(search),
        "\"%s\"",
        key
    );

    const char *position =
        strstr(json, search);

    if (!position)
        return false;

    position += strlen(search);

    while (*position == ' ' ||
           *position == '\t' ||
           *position == '\r' ||
           *position == '\n' ||
           *position == ':') {
        position++;
    }

    char *end = NULL;

    double result =
        strtod(position, &end);

    if (end == position)
        return false;

    *value = result;

    return true;
}

/*
 * ============================================================
 * RESET LORS D'UN CHANGEMENT DE JEU
 * ============================================================
 *
 * Lorsqu'un nouveau jeu est détecté :
 *
 * - l'ancienne animation disparaît ;
 * - la file d'attente est vidée ;
 * - les anciens trophées restants sont supprimés ;
 * - l'ancien état visuel n'est plus utilisé.
 *
 * On ne détruit pas le système d'animation.
 * On remet simplement son état à zéro.
 */
static void trophy_stream_reset_for_new_game_locked(void)
{
    current_game_title[0] = '\0';
    current_game_active = false;
    current_trophy_visible = false;
    platinum_final_visible = false;
    current_trophy_is_platinum_completion = false;

    trophy_animation_queue_count = 0;

    current_remaining_count = 0;
    current_remaining_index = 0;
    remaining_rotation_next_tick = 0;

    memset(
        current_remaining_names,
        0,
        sizeof(current_remaining_names)
    );

    memset(
        current_remaining_details,
        0,
        sizeof(current_remaining_details)
    );

    memset(
        current_remaining_types,
        0,
        sizeof(current_remaining_types)
    );

    memset(
        current_remaining_icons,
        0,
        sizeof(current_remaining_icons)
    );

    current_unlocked = 0;
    current_total = 0;
    current_progress = 0.0f;

    current_trophy_type[0] = '\0';
    current_trophy_name[0] = '\0';
    current_trophy_detail[0] = '\0';

    trophy_show_time = 0;

    blog(
        LOG_INFO,
        "[TrophyStream] Reset complet pour changement de jeu"
    );
}
/*
 * ------------------------------------------------------------
 * Mise à jour de la progression
 * ------------------------------------------------------------
 */

static void trophy_stream_set_progress(
    double percent)
{
    if (percent < 0.0)
        percent = 0.0;

    if (percent > 100.0)
        percent = 100.0;

    float progress =
        (float)(percent / 100.0);

    EnterCriticalSection(
        &progress_lock
    );

    current_progress =
        progress;

    LeaveCriticalSection(
        &progress_lock
    );

    blog(
        LOG_INFO,
        "[TrophyStream] Progression OBS : %.2f%%",
        percent
    );
}

/*
 * ------------------------------------------------------------
 * Traitement d'un message WebSocket
 * ------------------------------------------------------------
 */

static bool json_get_string_range(
    const char *begin,
    const char *end,
    const char *key,
    char *out,
    size_t out_size)
{
    if (!begin || !end || !key || !out ||
        out_size == 0 || begin >= end)
        return false;

    char search[128];

    snprintf(
        search,
        sizeof(search),
        "\"%s\"",
        key
    );

    const char *found = strstr(begin, search);

    if (!found || found >= end)
        return false;

    const char *colon =
        strchr(found + strlen(search), ':');

    if (!colon || colon >= end)
        return false;

    colon++;

    while (colon < end &&
           (*colon == ' ' ||
            *colon == '\t' ||
            *colon == '\r' ||
            *colon == '\n')) {
        colon++;
    }

    if (colon >= end || *colon != '"')
        return false;

    colon++;

    const char *value_end = colon;

    while (value_end < end) {
        if (*value_end == '"' &&
            (value_end == colon ||
             *(value_end - 1) != '\\')) {
            break;
        }

        value_end++;
    }

    if (value_end >= end)
        return false;

    size_t length =
        (size_t)(value_end - colon);

    if (length >= out_size)
        length = out_size - 1;

    memcpy(out, colon, length);
    out[length] = '\0';

    return true;
}

static bool trophy_stream_utf8_to_wide(
    const char *utf8,
    wchar_t *wide,
    int wide_count)
{
    if (!utf8 || !wide || wide_count <= 0)
        return false;

    int result =
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8,
            -1,
            wide,
            wide_count
        );

    if (result > 0)
        return true;

    return MultiByteToWideChar(
               CP_UTF8,
               0,
               utf8,
               -1,
               wide,
               wide_count
           ) > 0;
}

static void trophy_stream_draw_text_utf8(
    HDC dc,
    const char *text,
    RECT *rect,
    UINT format)
{
    if (!dc || !text || !rect)
        return;

    wchar_t wide[1024];

    if (!trophy_stream_utf8_to_wide(
            text,
            wide,
            (int)(sizeof(wide) / sizeof(wide[0]))))
        return;

    DrawTextW(
        dc,
        wide,
        -1,
        rect,
        format
    );
}

static bool trophy_stream_download_icon(const char *url)
{
    if (!url || !url[0])
        return false;

    /* COM is initialized per-thread. This function is called from both
     * the network worker and OBS's video thread, so initialize it here. */
    HRESULT com_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const bool uninitialize_com = (com_hr == S_OK || com_hr == S_FALSE);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE)
        return false;

    bool success = false;
    wchar_t wide_url[2048];
    wchar_t temp_dir[MAX_PATH];
    wchar_t temp_file[MAX_PATH];
    bool temp_file_created = false;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    IWICBitmapScaler *scaler = NULL;
    uint8_t decoded[128 * 128 * 4];
    uint8_t packed[128 * 128 * 4];
    UINT width = 0, height = 0;
    HRESULT hr = E_FAIL;

    memset(decoded, 0, sizeof(decoded));
    memset(packed, 0, sizeof(packed));

    if (!trophy_stream_utf8_to_wide(
            url, wide_url,
            (int)(sizeof(wide_url) / sizeof(wide_url[0]))))
        goto cleanup;

    DWORD temp_len = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_len == 0 || temp_len >= MAX_PATH)
        goto cleanup;

    if (!GetTempFileNameW(temp_dir, L"TS", 0, temp_file))
        goto cleanup;
    temp_file_created = true;

    hr = URLDownloadToFileW(NULL, wide_url, temp_file, 0, NULL);
    if (FAILED(hr))
        goto cleanup;

    hr = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr))
        goto cleanup;

    hr = IWICImagingFactory_CreateDecoderFromFilename(
        factory, temp_file, NULL, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
        goto cleanup;

    hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (FAILED(hr))
        goto cleanup;

    hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (FAILED(hr))
        goto cleanup;

    hr = IWICFormatConverter_Initialize(
        converter, (IWICBitmapSource *)frame,
        &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
        goto cleanup;

    UINT source_width = 0, source_height = 0;
    hr = IWICBitmapSource_GetSize(
        (IWICBitmapSource *)converter, &source_width, &source_height);
    if (FAILED(hr) || source_width == 0 || source_height == 0) {
        hr = E_FAIL;
        goto cleanup;
    }

    width = source_width > 128 ? 128 : source_width;
    height = source_height > 128 ? 128 : source_height;
    IWICBitmapSource *bitmap_source = (IWICBitmapSource *)converter;

    if (source_width != width || source_height != height) {
        hr = IWICImagingFactory_CreateBitmapScaler(factory, &scaler);
        if (FAILED(hr))
            goto cleanup;
        hr = IWICBitmapScaler_Initialize(
            scaler, (IWICBitmapSource *)converter, width, height,
            WICBitmapInterpolationModeHighQualityCubic);
        if (FAILED(hr))
            goto cleanup;
        bitmap_source = (IWICBitmapSource *)scaler;
    }

    /* Decode to a fixed stride first, then pack rows tightly. GDI expects
     * a stride of width*4 for the BITMAPINFO width passed to StretchDIBits. */
    hr = IWICBitmapSource_CopyPixels(
        bitmap_source, NULL, 128 * 4, (UINT)sizeof(decoded), decoded);
    if (FAILED(hr))
        goto cleanup;

    for (UINT y = 0; y < height; ++y) {
        memcpy(packed + (size_t)y * width * 4,
               decoded + (size_t)y * 128 * 4,
               (size_t)width * 4);
    }

    EnterCriticalSection(&icon_lock);
    memset(current_icon_pixels, 0, sizeof(current_icon_pixels));
    memcpy(current_icon_pixels, packed, (size_t)width * height * 4);
    current_icon_width = (int)width;
    current_icon_height = (int)height;
    snprintf(current_trophy_icon_url, sizeof(current_trophy_icon_url), "%s", url);
    current_icon_valid = true;
    LeaveCriticalSection(&icon_lock);
    success = true;

cleanup:
    if (scaler)
        IWICBitmapScaler_Release(scaler);
    if (converter)
        IWICFormatConverter_Release(converter);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (decoder)
        IWICBitmapDecoder_Release(decoder);
    if (factory)
        IWICImagingFactory_Release(factory);
    if (temp_file_created)
        DeleteFileW(temp_file);
    if (uninitialize_com)
        CoUninitialize();
    return success;
}

static void trophy_stream_set_icon_url(
    const char *url)
{
    EnterCriticalSection(&icon_lock);

    if (!url || !url[0]) {
        current_trophy_icon_url[0] = '\0';
        current_icon_width = 0;
        current_icon_height = 0;
        current_icon_valid = false;

        memset(
            current_icon_pixels,
            0,
            sizeof(current_icon_pixels)
        );

        LeaveCriticalSection(&icon_lock);
        return;
    }

    bool already_loaded =
        current_icon_valid &&
        strcmp(
            current_trophy_icon_url,
            url
        ) == 0;

    LeaveCriticalSection(&icon_lock);

    if (already_loaded)
        return;

    if (!trophy_stream_download_icon(url)) {
        blog(
            LOG_WARNING,
            "[TrophyStream] Impossible de charger l'icone : %s",
            url
        );
    }
}

static bool trophy_stream_draw_game_change_frozen_icon(
    HDC dc,
    int x,
    int y,
    int width,
    int height)
{
    uint8_t pixels[128 * 128 * 4];
    int icon_width = 0;
    int icon_height = 0;
    bool valid = false;

    EnterCriticalSection(&icon_lock);

    valid = game_change_icon_valid;
    icon_width = game_change_icon_width;
    icon_height = game_change_icon_height;

    if (valid) {
        memcpy(
            pixels,
            game_change_icon_pixels,
            sizeof(pixels)
        );
    }

    LeaveCriticalSection(&icon_lock);

    if (!valid || icon_width <= 0 || icon_height <= 0)
        return false;

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));

    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = icon_width;
    info.bmiHeader.biHeight = -icon_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);

    StretchDIBits(
        dc,
        x,
        y,
        width,
        height,
        0,
        0,
        icon_width,
        icon_height,
        pixels,
        &info,
        DIB_RGB_COLORS,
        SRCCOPY
    );

    return true;
}

static bool trophy_stream_draw_cached_icon(
    HDC dc,
    int x,
    int y,
    int width,
    int height)
{
    uint8_t pixels[128 * 128 * 4];
    int icon_width = 0;
    int icon_height = 0;

    EnterCriticalSection(&icon_lock);

    if (!current_icon_valid) {
        LeaveCriticalSection(&icon_lock);
        return false;
    }

    icon_width = current_icon_width;
    icon_height = current_icon_height;

    memcpy(
        pixels,
        current_icon_pixels,
        sizeof(pixels)
    );

    LeaveCriticalSection(&icon_lock);

    if (icon_width <= 0 || icon_height <= 0)
        return false;

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));

    info.bmiHeader.biSize =
        sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth =
        icon_width;
    info.bmiHeader.biHeight =
        -icon_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    /*
     * Mise à l'échelle de haute qualité pour l'icône.
     * On évite le rendu par défaut qui peut créer des artefacts
     * lorsque l'icône PlayStation est réduite.
     */
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);

    StretchDIBits(
        dc,
        x,
        y,
        width,
        height,
        0,
        0,
        icon_width,
        icon_height,
        pixels,
        &info,
        DIB_RGB_COLORS,
        SRCCOPY
    );

    return true;
}

static void trophy_stream_clear_remaining_locked(void)
{
    current_remaining_count = 0;

    memset(
        current_remaining_names,
        0,
        sizeof(current_remaining_names)
    );

    memset(
        current_remaining_details,
        0,
        sizeof(current_remaining_details)
    );

    memset(
        current_remaining_types,
        0,
        sizeof(current_remaining_types)
    );
    memset(
        current_remaining_icons,
        0,
        sizeof(current_remaining_icons)
    );
}

static void trophy_stream_parse_remaining_locked(
    const char *message)
{
    if (!message)
        return;

    const char *remaining =
        strstr(message, "\"remaining\"");

    /*
     * Le backend peut envoyer plusieurs STATE successifs :
     *
     *   STATE 0/12 + remaining absent/[]
     *   STATE 0/12 + remaining=[...12 trophées...]
     *
     * Un paquet temporaire sans liste complète ne doit JAMAIS
     * supprimer la liste déjà chargée dans OBS.
     *
     * On parse donc dans des tableaux temporaires et on ne remplace
     * current_remaining_* que si au moins un trophée valide a été
     * réellement trouvé.
     */
    if (!remaining)
        return;

    const char *array =
        strchr(remaining, '[');

    if (!array)
        return;

    char new_names[TROPHY_MAX_REMAINING][256];
    char new_details[TROPHY_MAX_REMAINING][512];
    char new_types[TROPHY_MAX_REMAINING][32];
    char new_icons[TROPHY_MAX_REMAINING][1024];

    memset(new_names, 0, sizeof(new_names));
    memset(new_details, 0, sizeof(new_details));
    memset(new_types, 0, sizeof(new_types));
    memset(new_icons, 0, sizeof(new_icons));

    int new_count = 0;

    const char *cursor = array + 1;

    while (*cursor &&
           new_count < TROPHY_MAX_REMAINING) {

        const char *object_start =
            strchr(cursor, '{');

        if (!object_start)
            break;

        const char *object_end = object_start;
        int brace_depth = 0;
        bool in_string = false;
        bool escaped = false;

        for (; *object_end; object_end++) {
            char c = *object_end;

            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }

            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                brace_depth++;
            } else if (c == '}') {
                brace_depth--;

                if (brace_depth == 0)
                    break;
            }
        }

        if (!*object_end || brace_depth != 0)
            break;

        int index = new_count;

        json_get_string_range(
            object_start,
            object_end,
            "trophyName",
            new_names[index],
            sizeof(new_names[index])
        );

        json_get_string_range(
            object_start,
            object_end,
            "trophyDetail",
            new_details[index],
            sizeof(new_details[index])
        );

        bool has_type =
            json_get_string_range(
                object_start,
                object_end,
                "trophy_type",
                new_types[index],
                sizeof(new_types[index])
            );

        if (!has_type || !new_types[index][0]) {
            json_get_string_range(
                object_start,
                object_end,
                "trophyType",
                new_types[index],
                sizeof(new_types[index])
            );
        }

        json_get_string_range(
            object_start,
            object_end,
            "trophyIconUrl",
            new_icons[index],
            sizeof(new_icons[index])
        );

        if (new_names[index][0] != '\0')
            new_count++;

        cursor = object_end + 1;

        if (*cursor == ']')
            break;
    }

    /*
     * Tableau vide / réponse incomplète :
     *
     * surtout ne pas effacer la liste précédente.
     */
    if (new_count <= 0) {
        blog(
            LOG_INFO,
            "[TrophyStream] Liste remaining vide/incomplete : conservation de %d trophees",
            current_remaining_count
        );
        return;
    }

    /*
     * Sauvegarder le trophée actuellement affiché.
     * L'index de rotation ne doit pas repartir à zéro à chaque
     * rafraîchissement du backend.
     */
    int previous_count = current_remaining_count;
    int previous_index = current_remaining_index;
    bool previous_animation_visible = current_trophy_visible;

    char previous_name[256] = "";

    if (previous_count > 0 &&
        previous_index >= 0 &&
        previous_index < previous_count) {

        snprintf(
            previous_name,
            sizeof(previous_name),
            "%s",
            current_remaining_names[previous_index]
        );
    }

    /*
     * Remplacement atomique de la liste complète.
     */
    current_remaining_count = new_count;

    memcpy(
        current_remaining_names,
        new_names,
        sizeof(current_remaining_names)
    );

    memcpy(
        current_remaining_details,
        new_details,
        sizeof(current_remaining_details)
    );

    memcpy(
        current_remaining_types,
        new_types,
        sizeof(current_remaining_types)
    );

    memcpy(
        current_remaining_icons,
        new_icons,
        sizeof(current_remaining_icons)
    );

    /*
     * Première liste : commencer au premier trophée.
     */
    if (previous_count <= 0) {

        current_remaining_index = 0;

        /*
         * Le premier trophée doit rester visible pendant 5 secondes
         * avant le premier changement.
         */
        remaining_rotation_next_tick =
            GetTickCount64() + 5000ULL;
    }
    else {

        /*
         * Si le trophée actuellement affiché existe encore dans la
         * nouvelle liste, on conserve exactement celui-là.
         */
        int preserved_index = -1;

        if (previous_name[0]) {

            for (int i = 0; i < new_count; i++) {

                if (strcmp(
                        current_remaining_names[i],
                        previous_name
                    ) == 0) {

                    preserved_index = i;
                    break;
                }
            }
        }

        if (preserved_index >= 0) {

            current_remaining_index =
                preserved_index;

        } else if (previous_index >= 0 &&
                   previous_index < new_count) {

            /*
             * Le trophée courant a disparu parce qu'il a été obtenu.
             * On garde le même emplacement dans la nouvelle liste
             * afin de continuer naturellement avec le suivant.
             */
            current_remaining_index =
                previous_index;

        } else {

            current_remaining_index = 0;
        }

        /*
         * IMPORTANT :
         * Le chrono de rotation est conservé.
         * Un refresh backend ne rallonge ni ne raccourcit les 5 secondes.
         */
    }

    /*
     * Une liste de trophées manquants n'est pas une animation.
     * On conserve uniquement l'état d'une éventuelle animation
     * réellement en cours.
     */
    current_trophy_visible =
        previous_animation_visible;

    blog(
        LOG_INFO,
        "[TrophyStream] %d trophees manquants parses, index rotation=%d",
        current_remaining_count,
        current_remaining_index
    );
}

static bool trophy_stream_message_type_is(
    const char *message,
    const char *expected)
{
    char type[32];

    if (!json_get_string_range(
            message,
            message + strlen(message),
            "type",
            type,
            sizeof(type))) {
        return false;
    }

    return _stricmp(type, expected) == 0;
}

/*
 * ------------------------------------------------------------
 * Sons des trophées
 * ------------------------------------------------------------
 *
 * Les MP3 originaux du projet sont convertis en WAV pour
 * utiliser l'API native PlaySound de Windows.
 *
 * Les fichiers doivent être installés à côté du plugin dans :
 *
 *   obs-plugins\64bit\trophy-stream-sounds\
 *
 *   bronze.wav
 *   silver.wav
 *   gold.wav
 *   platinum.wav
 *
 * Le son est lancé uniquement quand une animation démarre.
 * Ainsi, une file de plusieurs trophées joue bien :
 *
 *   animation 1 + son 1
 *   animation 2 + son 2
 *   animation 3 + son 3
 *
 * sans lancer tous les sons au moment de leur réception.
 */
static bool trophy_stream_play_sound_for_type(
    const char *trophy_type)
{
    if (!trophy_type || !trophy_type[0])
        return false;

    const char *filename = NULL;

    if (_stricmp(trophy_type, "bronze") == 0) {
        filename = "bronze.wav";
    }
    else if (_stricmp(trophy_type, "silver") == 0) {
        filename = "silver.wav";
    }
    else if (_stricmp(trophy_type, "gold") == 0) {
        filename = "gold.wav";
    }
    else if (_stricmp(trophy_type, "platinum") == 0) {
        filename = "platinum.wav";
    }
    else {
        return false;
    }

    HMODULE module = NULL;

    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)&trophy_stream_play_sound_for_type,
            &module)) {
        blog(
            LOG_WARNING,
            "[TrophyStream] Impossible de trouver le dossier du plugin pour le son"
        );
        return false;
    }

    wchar_t module_path[MAX_PATH];

    DWORD length =
        GetModuleFileNameW(
            module,
            module_path,
            MAX_PATH
        );

    if (length == 0 || length >= MAX_PATH)
        return false;

    wchar_t *slash =
        wcsrchr(module_path, L'\\');

    if (!slash)
        return false;

    *(slash + 1) = L'\0';

    wchar_t sound_path[MAX_PATH];

    _snwprintf(
        sound_path,
        MAX_PATH,
        L"%strophy-stream-sounds\\%hs",
        module_path,
        filename
    );

    sound_path[MAX_PATH - 1] = L'\0';

    /*
     * Arrête un éventuel son précédent avant de lancer
     * celui du trophée actuel.
     */
    PlaySoundW(
        NULL,
        NULL,
        0
    );

    BOOL played =
        PlaySoundW(
            sound_path,
            NULL,
            SND_FILENAME |
            SND_ASYNC |
            SND_NODEFAULT
        );

    if (!played) {
        blog(
            LOG_WARNING,
            "[TrophyStream] Son introuvable ou impossible a lire : %hs",
            filename
        );
        return false;
    }

    blog(
        LOG_INFO,
        "[TrophyStream] Son joué : %hs (%hs)",
        filename,
        trophy_type
    );

    return true;
}

static void trophy_stream_start_item_locked(
    const trophy_animation_item *item)
{
    if (!item)
        return;

    snprintf(
        current_trophy_type,
        sizeof(current_trophy_type),
        "%s",
        item->type
    );

    snprintf(
        current_trophy_name,
        sizeof(current_trophy_name),
        "%s",
        item->name
    );

    snprintf(
        current_trophy_detail,
        sizeof(current_trophy_detail),
        "%s",
        item->detail
    );

    current_unlocked = item->unlocked;
    current_total = item->total;
    current_progress = item->progress;
    current_trophy_duration = item->duration;

    current_trophy_is_platinum_completion =
        (_stricmp(item->type, "platinum") == 0 &&
         item->total > 0 &&
         item->unlocked >= item->total);

    /* Une nouvelle animation remplace l ecran final. */
    platinum_final_visible = false;
    current_trophy_visible = true;
    trophy_show_time = GetTickCount64();
}

static bool trophy_stream_queue_item_locked(
    const trophy_animation_item *item)
{
    if (!item)
        return false;

    if (trophy_animation_queue_count >=
        TROPHY_ANIMATION_QUEUE_MAX) {

        blog(
            LOG_WARNING,
            "[TrophyStream] File d'attente pleine : trophee ignore (%s)",
            item->name
        );

        return false;
    }

    trophy_animation_queue[
        trophy_animation_queue_count
    ] = *item;

    trophy_animation_queue_count++;

    blog(
        LOG_INFO,
        "[TrophyStream] Trophée mis en file : %s (%d en attente)",
        item->name,
        trophy_animation_queue_count
    );

    return true;
}

static bool trophy_stream_pop_next_item_locked(
    trophy_animation_item *item)
{
    if (!item ||
        trophy_animation_queue_count <= 0) {
        return false;
    }

    *item = trophy_animation_queue[0];

    for (int i = 1;
         i < trophy_animation_queue_count;
         i++) {

        trophy_animation_queue[i - 1] =
            trophy_animation_queue[i];
    }

    trophy_animation_queue_count--;

    return true;
}

static void trophy_stream_process_message(
    const char *message)
{
    if (!message)
        return;
    if (trophy_stream_message_type_is(message, "connected")) {
        EnterCriticalSection(&progress_lock);
        trophy_stream_display_enabled = true;
        LeaveCriticalSection(&progress_lock);
        blog(LOG_INFO, "[TrophyStream] Affichage reactive apres connexion");
        return;
    }

    if (trophy_stream_message_type_is(message, "trophystream-stopped")) {
        EnterCriticalSection(&progress_lock);
        trophy_stream_display_enabled = false;
        trophy_stream_reset_for_new_game_locked();
        game_change_visible = false;
        game_change_display_title[0] = 0;
        LeaveCriticalSection(&progress_lock);
        trophy_stream_set_icon_url("");
        blog(LOG_INFO, "[TrophyStream] Affichage masque sur demande d'arret");
        return;
    }

blog(
    LOG_INFO,
    "[TrophyStream] MESSAGE WS RECU : %.300s",
    message
);

    /*
     * ========================================================
     * STATS NOTION
     * ========================================================
     *
     * {
     *   "type": "stats",
     *   "platinumGames": 19,
     *   "gamesPlayed": 47
     * }
     */
    if (trophy_stream_message_type_is(
            message,
            "stats")) {

        double platinum_games = 0.0;
        double games_played = 0.0;

        bool has_platinum =
            json_get_number(
                message,
                "platinumGames",
                &platinum_games
            );

        bool has_played =
            json_get_number(
                message,
                "gamesPlayed",
                &games_played
            );

        if (has_platinum && has_played) {

            EnterCriticalSection(&progress_lock);

            notion_platinum_games = (int)platinum_games;
            notion_games_played = (int)games_played;
            notion_stats_valid = true;

            LeaveCriticalSection(&progress_lock);

            blog(
                LOG_INFO,
                "[TrophyStream] STATS NOTION : %d / %d",
                (int)platinum_games,
                (int)games_played
            );
        }

        return;
    }

    /*
     * ========================================================
     * GAME-CHANGE : animation de changement de jeu
     * ========================================================
     *
     * Le backend envoie cet evenement avant le STATE du nouveau jeu.
     * Il permet d'afficher une animation dediee sans attendre le
     * prochain refresh de progression.
     */
    if (trophy_stream_message_type_is(
            message,
            "game-change")) {

        /*
         * Un changement de jeu est une transition exclusive.
         * Pendant les 10 s de l'animation, on ignore les GAME-CHANGE
         * parasites/oscillants provenant d'etats intermediaires du backend.
         * Le STATE met ensuite a jour les statistiques, mais ne peut
         * jamais modifier le titre ou l'image deja figes de l'animation.
         */
        EnterCriticalSection(&progress_lock);
        bool game_animation_already_active = game_change_visible;
        LeaveCriticalSection(&progress_lock);

        if (game_animation_already_active) {
            blog(
                LOG_INFO,
                "[TrophyStream] GAME-CHANGE ignore pendant animation active"
            );
            return;
        }

        char game_title[256] = "";
        char cover_url[1024] = "";
        char np_communication_id[128] = "";
        char np_service_name[64] = "";
        char new_game_id[256] = "";

        json_get_string_range(
            message,
            message + strlen(message),
            "titleName",
            game_title,
            sizeof(game_title)
        );

        json_get_string_range(
            message,
            message + strlen(message),
            "coverUrl",
            cover_url,
            sizeof(cover_url)
        );

        json_get_string_range(
            message,
            message + strlen(message),
            "npCommunicationId",
            np_communication_id,
            sizeof(np_communication_id)
        );

        json_get_string_range(
            message,
            message + strlen(message),
            "npServiceName",
            np_service_name,
            sizeof(np_service_name)
        );

        if (np_communication_id[0]) {
            if (np_service_name[0]) {
                snprintf(
                    new_game_id,
                    sizeof(new_game_id),
                    "%s:%s",
                    np_communication_id,
                    np_service_name
                );
            } else {
                snprintf(
                    new_game_id,
                    sizeof(new_game_id),
                    "%s",
                    np_communication_id
                );
            }
        }

        /*
         * IMPORTANT :
         * Le backend a deja decide qu'il s'agit d'un changement de jeu.
         * On ne refait PAS ici un test avec current_game_id :
         * le STATE peut etre arrive avant GAME-CHANGE et avoir deja
         * rempli current_game_id.
         *
         * Donc : chaque GAME-CHANGE recu lance l'animation.
         */
        EnterCriticalSection(&progress_lock);

        /* Commentaire technique corrigé. */
        if (current_trophy_visible) {
            uint64_t now = GetTickCount64();
            trophy_paused_elapsed = now - trophy_show_time;
            trophy_paused_for_game_change = true;
            EnterCriticalSection(&icon_lock);
            snprintf(
                trophy_paused_icon_url,
                sizeof(trophy_paused_icon_url),
                "%s",
                current_trophy_icon_url
            );
            LeaveCriticalSection(&icon_lock);
        } else {
            trophy_paused_elapsed = 0;
            trophy_paused_for_game_change = false;
            trophy_paused_icon_url[0] = '\0';
        }

        if (new_game_id[0]) {
            snprintf(
                current_game_id,
                sizeof(current_game_id),
                "%s",
                new_game_id
            );
        }

        if (game_title[0]) {
            snprintf(
                current_game_title,
                sizeof(current_game_title),
                "%s",
                game_title
            );
        } else {
            snprintf(
                current_game_title,
                sizeof(current_game_title),
                "Nouveau jeu"
            );
        }

        /*
         * FIGER le titre AVANT de laisser les prochains STATE
         * modifier current_game_title.
         */
        snprintf(
            game_change_display_title,
            sizeof(game_change_display_title),
            "%s",
            current_game_title
        );

        current_game_active = true;
        platinum_final_visible = false;
        current_trophy_is_platinum_completion = false;

        game_change_visible = true;
        game_change_show_time = GetTickCount64();

        blog(
            LOG_INFO,
            "[TrophyStream] ANIMATION GAME-CHANGE DECLENCHEE : %s",
            game_change_display_title
        );

        LeaveCriticalSection(&progress_lock);

        /*
         * Charger la vraie jaquette PSN.
         * trophy_stream_set_icon_url() est synchrone : a son retour,
         * l'image est disponible dans current_icon_pixels.
         */
        if (cover_url[0]) {
            trophy_stream_set_icon_url(cover_url);

            blog(
                LOG_INFO,
                "[TrophyStream] JAQUETTE JEU : %s",
                cover_url
            );
        }

        /*
         * FIGER l'image de l'animation.
         * Ainsi un trophee recu pendant l'animation ne peut jamais
         * remplacer la jaquette du jeu.
         */
        EnterCriticalSection(&icon_lock);

        game_change_icon_width = current_icon_width;
        game_change_icon_height = current_icon_height;
        game_change_icon_valid = current_icon_valid;

        memcpy(
            game_change_icon_pixels,
            current_icon_pixels,
            sizeof(game_change_icon_pixels)
        );

        LeaveCriticalSection(&icon_lock);

        blog(
            LOG_INFO,
            "[TrophyStream] ANIMATION FIGEE : titre=%s | icone=%s | %dx%d",
            game_change_display_title,
            game_change_icon_valid ? "OK" : "NON",
            game_change_icon_width,
            game_change_icon_height
        );

        /*
         * NE PAS remettre game_change_visible a partir de game_title :
         * cela annulait l'animation lorsque le parser renvoyait un titre
         * vide dans un paquet intermediaire.
         */
        return;
    }

    /*
     * ========================================================
     * STATE : overlay permanent
     * ========================================================
     */
   if (trophy_stream_message_type_is(
        message,
        "state")) {

    char state_icon_url[1024] = "";
    char state_cover_url[1024] = "";

    /*
     * --------------------------------------------------------
     * Identification du jeu
     * --------------------------------------------------------
     *
     * Le backend envoie :
     *
     *   npCommunicationId
     *   npServiceName
     *
     * On combine les deux pour obtenir un identifiant
     */
    char np_communication_id[128] = "";
    char np_service_name[64] = "";
    char game_title[256] = "";

    json_get_string_range(
        message,
        message + strlen(message),
        "titleName",
        game_title,
        sizeof(game_title)
    );

    json_get_string_range(
        message,
        message + strlen(message),
        "coverUrl",
        state_cover_url,
        sizeof(state_cover_url)
    );

    json_get_string_range(
        message,
        message + strlen(message),
        "npCommunicationId",
        np_communication_id,
        sizeof(np_communication_id)
    );

    json_get_string_range(
        message,
        message + strlen(message),
        "npServiceName",
        np_service_name,
        sizeof(np_service_name)
    );

    char new_game_id[256] = "";

    if (np_communication_id[0]) {

        if (np_service_name[0]) {

            snprintf(
                new_game_id,
                sizeof(new_game_id),
                "%s:%s",
                np_communication_id,
                np_service_name
            );

        } else {

            snprintf(
                new_game_id,
                sizeof(new_game_id),
                "%s",
                np_communication_id
            );
        }
    }

    /* Commentaire technique corrigé. */
bool state_should_start_game_change = false;

if (new_game_id[0] && state_cover_url[0] && !game_change_visible) {
    if (!current_game_id[0] || strcmp(current_game_id, new_game_id) != 0) {
        state_should_start_game_change = true;
    }
}

if (state_should_start_game_change) {
    EnterCriticalSection(&progress_lock);

    if (current_trophy_visible) {
        uint64_t now = GetTickCount64();
        trophy_paused_elapsed = now - trophy_show_time;
        trophy_paused_for_game_change = true;
        EnterCriticalSection(&icon_lock);
        snprintf(trophy_paused_icon_url, sizeof(trophy_paused_icon_url), "%s", current_trophy_icon_url);
        LeaveCriticalSection(&icon_lock);
    } else {
        trophy_paused_elapsed = 0;
        trophy_paused_for_game_change = false;
        trophy_paused_icon_url[0] = '\0';
    }

    snprintf(current_game_id, sizeof(current_game_id), "%s", new_game_id);
    snprintf(current_game_title, sizeof(current_game_title), "%s", game_title[0] ? game_title : "Nouveau jeu");
    snprintf(game_change_display_title, sizeof(game_change_display_title), "%s", current_game_title);
    current_game_active = true;
    platinum_final_visible = false;
    current_trophy_is_platinum_completion = false;
    game_change_visible = true;
    game_change_show_time = GetTickCount64();

    blog(LOG_INFO, "[TrophyStream] ANIMATION V10 GAME-CHANGE : %s", game_change_display_title);
    LeaveCriticalSection(&progress_lock);

    trophy_stream_set_icon_url(state_cover_url);
    EnterCriticalSection(&icon_lock);
    game_change_icon_width = current_icon_width;
    game_change_icon_height = current_icon_height;
    game_change_icon_valid = current_icon_valid;
    memcpy(game_change_icon_pixels, current_icon_pixels, sizeof(game_change_icon_pixels));
    LeaveCriticalSection(&icon_lock);
    blog(LOG_INFO, "[TrophyStream] JAQUETTE V10 FIGEE : OK");
}

    /*
     * cette partie, ce qui expliquait les logs 0/0 et l'absence
     */
    double percent = 0.0;
    double unlocked = 0.0;
    double total = 0.0;

    bool has_percent =
        json_get_number(message, "percent", &percent);

    bool has_unlocked =
        json_get_number(message, "unlocked", &unlocked);

    bool has_total =
        json_get_number(message, "total", &total);

    if (!has_percent &&
        has_unlocked &&
        has_total &&
        total > 0.0) {
        percent = (unlocked / total) * 100.0;
    }

    if (percent < 0.0)
        percent = 0.0;

    if (percent > 100.0)
        percent = 100.0;

    EnterCriticalSection(&progress_lock);

    /*
     * aux valeurs utilisees par l animation. Sinon la barre peut faire
     * 100 -> 0 -> 100 lorsque plusieurs STATE arrivent rapidement.
     * Le prochain STATE apres l animation reprendra naturellement
     * la progression reelle du jeu.
     */
    if (!current_trophy_visible) {
        if (has_unlocked)
            current_unlocked = (int)unlocked;

        if (has_total)
            current_total = (int)total;
    }

    if (current_total > 0)
        current_game_active = true;

    /*
     * pendant les 10 secondes du Game Change.
     */
    /*
 * Un STATE 0/0 ne doit jamais couper une animation
 *
 * Si aucune animation n'est active, on peut nettoyer
 */
if (!current_game_active &&
    !game_change_visible &&
    !current_trophy_visible &&
    trophy_animation_queue_count == 0) {

    trophy_stream_clear_remaining_locked();
}

    if (!current_trophy_visible)
        current_progress = (float)(percent / 100.0);

    trophy_stream_parse_remaining_locked(message);

    /*
     * Jeu entierement termine : on arrete la rotation des trophees
     * manquants, mais on ne touche JAMAIS a l animation en cours.
     * directement l ecran final une fois la transition de jeu terminee.
     */
    if (!game_change_visible &&
    current_game_active &&
    current_total > 0 &&
    current_unlocked >= current_total) {

    current_remaining_count = 0;
    current_remaining_index = 0;
    remaining_rotation_next_tick = 0;

    /*
     * IMPORTANT :
     *
     * trophy_stream_generate_frame().
     *
     */
}

    if (!game_change_visible &&
        !current_trophy_visible &&
        current_remaining_count > 0) {

        int index =
            current_remaining_index % current_remaining_count;

        snprintf(
            current_trophy_name,
            sizeof(current_trophy_name),
            "%s",
            current_remaining_names[index]
        );

        snprintf(
            current_trophy_detail,
            sizeof(current_trophy_detail),
            "%s",
            current_remaining_details[index]
        );

        snprintf(
            current_trophy_type,
            sizeof(current_trophy_type),
            "%s",
            current_remaining_types[index][0]
                ? current_remaining_types[index]
                : "bronze"
        );

        snprintf(
            state_icon_url,
            sizeof(state_icon_url),
            "%s",
            current_remaining_icons[index]
        );
    }

    LeaveCriticalSection(&progress_lock);

    blog(
        LOG_INFO,
        "[TrophyStream] STATE : %d/%d - %d restants",
        current_unlocked,
        current_total,
        current_remaining_count
    );

    return;
    }

    /*
     * ========================================================
     * TROPHY : animation
     * ========================================================
     */
    /*
     * Le chemin normal utilise type="trophy".
     * Certains tests manuels peuvent envoyer directement
     */
    bool is_trophy_message =
        trophy_stream_message_type_is(
            message,
            "trophy"
        );

    bool is_platinum_message =
        trophy_stream_message_type_is(
            message,
            "platinum"
        );

    if (!is_trophy_message &&
        !is_platinum_message) {
        return;
    }

    double percent = 0.0;
    double unlocked = 0.0;
    double total = 0.0;

    bool has_percent =
        json_get_number(
            message,
            "percent",
            &percent
        );

    bool has_unlocked =
        json_get_number(
            message,
            "unlocked",
            &unlocked
        );

    bool has_total =
        json_get_number(
            message,
            "total",
            &total
        );

    if (!has_percent &&
        has_unlocked &&
        has_total &&
        total > 0.0) {

        percent =
            (unlocked / total) * 100.0;
    }

    if (percent < 0.0)
        percent = 0.0;

    if (percent > 100.0)
        percent = 100.0;

    double duration = 10000.0;

    json_get_number(
        message,
        "duration",
        &duration
    );

    if (duration < 0.0)
        duration = 0.0;

    if (duration > 60000.0)
        duration = 60000.0;

    trophy_animation_item item;

    memset(
        &item,
        0,
        sizeof(item)
    );

    /*
     * Le backend peut envoyer le type sous l'un ou l'autre nom.
     */
    bool has_trophy_type =
        json_get_string_range(
            message,
            message + strlen(message),
            "trophy_type",
            item.type,
            sizeof(item.type)
        );

    if (!has_trophy_type || !item.type[0]) {
        json_get_string_range(
            message,
            message + strlen(message),
            "trophyType",
            item.type,
            sizeof(item.type)
        );
    }

    json_get_string_range(
        message,
        message + strlen(message),
        "trophyName",
        item.name,
        sizeof(item.name)
    );

    json_get_string_range(
        message,
        message + strlen(message),
        "trophyDetail",
        item.detail,
        sizeof(item.detail)
    );

    json_get_string_range(
        message,
        message + strlen(message),
        "trophyIconUrl",
        item.icon_url,
        sizeof(item.icon_url)
    );

    /*
     * aucun type. Normalement, bronze/silver/gold/platinum est
     */
    if (!item.type[0]) {

        if (is_platinum_message) {

            snprintf(
                item.type,
                sizeof(item.type),
                "platinum"
            );

        } else {

            snprintf(
                item.type,
                sizeof(item.type),
                "gold"
            );
        }
    }

    if (has_unlocked)
        item.unlocked = (int)unlocked;
    else
        item.unlocked = current_unlocked;

    if (has_total)
        item.total = (int)total;
    else
        item.total = current_total;

    item.progress =
        (float)(percent / 100.0);

    item.duration =
        (uint64_t)duration;

    bool play_now = false;

    EnterCriticalSection(
        &progress_lock
    );

    /*
     * Ne pas modifier la progression de l animation actuellement
     * mis en file ; ses valeurs seront appliquees lorsqu il demarrera.
     */
    current_game_active = true;

    /*
     *
     */
    if (!current_trophy_visible &&
        !game_change_visible) {

        trophy_stream_start_item_locked(
            &item
        );

        play_now = true;

    } else {
        /*
         * Pendant une animation de changement de jeu, le trophee
         * reste en file et ne peut jamais prendre visuellement la main.
         */

        trophy_stream_queue_item_locked(
            &item
        );
    }

    int queued =
        trophy_animation_queue_count;

    LeaveCriticalSection(
        &progress_lock
    );

    if (play_now) {

        trophy_stream_play_sound_for_type(
            item.type
        );
    }

    if (play_now &&
        item.icon_url[0]) {

        trophy_stream_set_icon_url(
            item.icon_url
        );
    }

    blog(
        LOG_INFO,
        "[TrophyStream] TROPHÉE : %s (%s) %d/%d | file=%d",
        item.name,
        item.type,
        item.unlocked,
        item.total,
        queued
    );
}

/*
 * ------------------------------------------------------------
 * Envoi complet d’une chaîne TCP
 * ------------------------------------------------------------
 */

/*
 * ------------------------------------------------------------
 * Réception du handshake HTTP WebSocket
 * ------------------------------------------------------------
 */

static bool trophy_stream_send_all(
    SOCKET socket_handle,
    const char *data,
    int length)
{
    int sent = 0;
    while (sent < length) {
        int result = send(socket_handle, data + sent, length - sent, 0);
        if (result == SOCKET_ERROR || result == 0) return false;
        sent += result;
    }
    return true;
}

static bool trophy_stream_receive_http(
    SOCKET socket_handle,
    char *buffer,
    size_t buffer_size)
{
    size_t total = 0;

    while (total < buffer_size - 1) {

        fd_set read_set;

        FD_ZERO(&read_set);
        FD_SET(socket_handle, &read_set);

        struct timeval timeout;

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready =
            select(
                0,
                &read_set,
                NULL,
                NULL,
                &timeout
            );

        if (ready == SOCKET_ERROR)
            return false;

        if (ready == 0) {

            if (WaitForSingleObject(
                    websocket_stop_event,
                    0
                ) == WAIT_OBJECT_0) {

                return false;
            }

            continue;
        }

        int received =
            recv(
                socket_handle,
                buffer + total,
                (int)(
                    buffer_size -
                    1 -
                    total
                ),
                0
            );

        if (received <= 0)
            return false;

        total +=
            (size_t)received;

        buffer[total] =
            '\0';

        /*
         * Fin du handshake HTTP.
         */
        if (strstr(
                buffer,
                "\r\n\r\n"
            )) {

            return true;
        }
    }

    return false;
}
/*
 * ------------------------------------------------------------
 * Connexion WebSocket
 * ------------------------------------------------------------
 *
 * Nous utilisons directement le protocole WebSocket au-dessus
 * de TCP.
 * ------------------------------------------------------------
 */

static SOCKET trophy_stream_connect(void)
{
    SOCKET socket_handle =
        INVALID_SOCKET;

    struct sockaddr_in address;

    socket_handle =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );

    if (socket_handle == INVALID_SOCKET) {

        blog(
            LOG_WARNING,
            "[TrophyStream] socket() echoue : %d",
            WSAGetLastError()
        );

        return INVALID_SOCKET;
    }

    memset(
        &address,
        0,
        sizeof(address)
    );

    address.sin_family =
    AF_INET;

address.sin_port =
    htons(TROPHY_WS_PORT);

if (InetPtonA(
        AF_INET,
        TROPHY_WS_HOST,
        &address.sin_addr) != 1) {

    closesocket(socket_handle);

    return INVALID_SOCKET;
}

if (connect(
        socket_handle,
        (struct sockaddr *)&address,
        sizeof(address)) == SOCKET_ERROR) {

    closesocket(socket_handle);

    return INVALID_SOCKET;
}

    /*
     *
     * Elle est valide pour notre connexion locale.
     */
    const char *websocket_key =
        "dGhlIHNhbXBsZSBub25jZQ==";

    char request[1024];

    int request_length =
        snprintf(
            request,
            sizeof(request),

            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1:3002\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n",

            websocket_key
        );

    if (!trophy_stream_send_all(
            socket_handle,
            request,
            request_length)) {

        closesocket(socket_handle);

        return INVALID_SOCKET;
    }

    char response[4096];

    memset(
        response,
        0,
        sizeof(response)
    );

    if (!trophy_stream_receive_http(
            socket_handle,
            response,
            sizeof(response))) {

        closesocket(socket_handle);

        return INVALID_SOCKET;
    }

    /*
     *
     * HTTP/1.1 101 Switching Protocols
     */
    if (!strstr(
            response,
            "101 Switching Protocols")) {

        blog(
            LOG_WARNING,
            "[TrophyStream] Handshake WebSocket refuse"
        );

        closesocket(socket_handle);

        return INVALID_SOCKET;
    }

    blog(
        LOG_INFO,
        "[TrophyStream] WebSocket connecte a 127.0.0.1:3002"
    );

    return socket_handle;
}

/*
 * ------------------------------------------------------------
 * Lecture exacte de N octets
 * ------------------------------------------------------------
 */

static bool trophy_stream_recv_exact(
    SOCKET socket,
    uint8_t *buffer,
    size_t length)
{
    size_t received_total = 0;

    while (received_total < length) {

        fd_set read_set;

        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);

        struct timeval timeout;

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready =
            select(
                0,
                &read_set,
                NULL,
                NULL,
                &timeout
            );

        if (ready == SOCKET_ERROR)
            return false;

        if (ready == 0) {

            if (WaitForSingleObject(
                    websocket_stop_event,
                    0) == WAIT_OBJECT_0) {

                return false;
            }

            continue;
        }

        int result =
            recv(
                socket,
                (char *)buffer + received_total,
                (int)(length - received_total),
                0
            );

        if (result <= 0)
            return false;

        received_total +=
            (size_t)result;
    }

    return true;
}

/* WebSocket clients must mask every frame they send, including PONG. */
static bool trophy_stream_send_control_frame(
    SOCKET socket_handle, uint8_t opcode,
    const uint8_t *payload, size_t payload_length)
{
    if (payload_length > 125)
        return false;

    uint8_t header[2] = {
        (uint8_t)(0x80 | (opcode & 0x0F)),
        (uint8_t)(0x80 | payload_length)
    };
    uint8_t mask[4];
    uint8_t masked_payload[125];

    if (BCryptGenRandom(NULL, mask, (ULONG)sizeof(mask),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        /* Fall back to a per-frame changing mask if the system RNG fails. */
        uint64_t seed = GetTickCount64() ^ (uint64_t)(uintptr_t)payload;
        for (size_t i = 0; i < sizeof(mask); ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            mask[i] = (uint8_t)seed;
        }
    }

    for (size_t i = 0; i < payload_length; ++i)
        masked_payload[i] = (uint8_t)(payload[i] ^ mask[i % 4]);

    if (!trophy_stream_send_all(socket_handle, (const char *)header, 2) ||
        !trophy_stream_send_all(socket_handle, (const char *)mask, 4))
        return false;

    return payload_length == 0 ||
        trophy_stream_send_all(socket_handle,
                               (const char *)masked_payload,
                               (int)payload_length);
}

/*
 * ------------------------------------------------------------
 * Lecture d'un frame WebSocket
 * ------------------------------------------------------------
 */

static bool trophy_stream_receive_frame(
    SOCKET socket_handle,
    char *message,
    size_t message_size)
{
    /* The function is called by one worker thread, so this reassembly
     * state is intentionally thread-local to that connection loop. */
    static char fragments[65536];
    static size_t fragment_length = 0;
    static bool fragment_active = false;

    uint8_t header[2];
    if (!trophy_stream_recv_exact(socket_handle, header, sizeof(header)))
        return false;

    const bool fin = (header[0] & 0x80) != 0;
    const uint8_t opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_length = header[1] & 0x7F;

    /* RSV bits must be zero because no WebSocket extensions are negotiated. */
    if ((header[0] & 0x70) != 0) {
        blog(LOG_WARNING, "[TrophyStream] Bits RSV inattendus dans une trame WebSocket");
        return false;
    }

    if (payload_length == 126) {
        uint8_t extended[2];
        if (!trophy_stream_recv_exact(socket_handle, extended, sizeof(extended)))
            return false;
        payload_length = ((uint64_t)extended[0] << 8) | extended[1];
    } else if (payload_length == 127) {
        uint8_t extended[8];
        if (!trophy_stream_recv_exact(socket_handle, extended, sizeof(extended)))
            return false;
        if (extended[0] & 0x80) {
            blog(LOG_WARNING, "[TrophyStream] Longueur WebSocket invalide");
            return false;
        }
        payload_length = 0;
        for (size_t i = 0; i < sizeof(extended); ++i)
            payload_length = (payload_length << 8) | extended[i];
    }

    const bool control_frame = opcode >= 0x8;
    if (control_frame && (!fin || payload_length > 125)) {
        blog(LOG_WARNING, "[TrophyStream] Trame de contrôle WebSocket invalide");
        return false;
    }
    if (payload_length >= message_size || payload_length > sizeof(fragments)) {
        blog(LOG_WARNING,
             "[TrophyStream] Message WebSocket trop grand : %llu octets",
             (unsigned long long)payload_length);
        return false;
    }
    if (!(opcode == 0x0 || opcode == 0x1 || opcode == 0x2 ||
          opcode == 0x8 || opcode == 0x9 || opcode == 0xA)) {
        blog(LOG_WARNING, "[TrophyStream] Opcode WebSocket inconnu : %u", opcode);
        return false;
    }

    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked && !trophy_stream_recv_exact(socket_handle, mask, sizeof(mask)))
        return false;
    if (payload_length > 0 &&
        !trophy_stream_recv_exact(socket_handle, (uint8_t *)message,
                                  (size_t)payload_length))
        return false;

    for (uint64_t i = 0; masked && i < payload_length; ++i)
        message[i] = (char)((uint8_t)message[i] ^ mask[i % 4]);
    message[payload_length] = '\0';

    if (opcode == 0x9) { /* PING -> masked PONG */
        if (!trophy_stream_send_control_frame(
                socket_handle, 0xA, (const uint8_t *)message,
                (size_t)payload_length))
            return false;
        return true;
    }
    if (opcode == 0xA)
        return true;
    if (opcode == 0x8) {
        blog(LOG_INFO, "[TrophyStream] CLOSE WebSocket reçu");
        return false;
    }
    if (opcode == 0x2) {
        /* Binary frames are not part of the backend JSON protocol. */
        fragment_active = false;
        fragment_length = 0;
        return true;
    }

    if (opcode == 0x1) {
        if (fragment_active) {
            /* A new data message before the prior fragmented message ended. */
            fragment_active = false;
            fragment_length = 0;
        }
        if (fin) {
            trophy_stream_process_message(message);
            return true;
        }
        memcpy(fragments, message, (size_t)payload_length);
        fragment_length = (size_t)payload_length;
        fragment_active = true;
        return true;
    }

    /* Continuation frame */
    if (opcode == 0x0) {
        if (!fragment_active) {
            blog(LOG_WARNING, "[TrophyStream] Continuation sans message initial");
            return false;
        }
        if (fragment_length + (size_t)payload_length >= sizeof(fragments)) {
            blog(LOG_WARNING, "[TrophyStream] Message fragmenté trop grand");
            fragment_active = false;
            fragment_length = 0;
            return false;
        }
        memcpy(fragments + fragment_length, message, (size_t)payload_length);
        fragment_length += (size_t)payload_length;
        if (fin) {
            fragments[fragment_length] = '\0';
            trophy_stream_process_message(fragments);
            fragment_active = false;
            fragment_length = 0;
        }
    }
    return true;
}

/*
 * ------------------------------------------------------------
 * Thread WebSocket
 * ------------------------------------------------------------
 */

static DWORD WINAPI trophy_stream_websocket_thread(
    LPVOID parameter)
{
    UNUSED_PARAMETER(parameter);

    HRESULT com_result =
        CoInitializeEx(
            NULL,
            COINIT_MULTITHREADED
        );

    while (
        InterlockedCompareExchange(
            &plugin_running,
            0,
            0) != 0) {

        SOCKET socket_handle =
            trophy_stream_connect();

        if (socket_handle ==
            INVALID_SOCKET) {

            /*
             *
             * On attend 2 secondes avant
             */
            WaitForSingleObject(
                websocket_stop_event,
                2000
            );

            continue;
        }

        while (
            InterlockedCompareExchange(
                &plugin_running,
                0,
                0) != 0) {

            /*
             *
             */
            char message[65536];

            memset(
                message,
                0,
                sizeof(message)
            );

            if (!trophy_stream_receive_frame(
                    socket_handle,
                    message,
                    sizeof(message))) {

                break;
            }
        }

        closesocket(
            socket_handle
        );

        /*
         * A transient WebSocket disconnect is not a game change.
         * Keep the last known progress, missing trophies and active
         * animation; an explicit "trophystream-stopped" or a new
         * game state is responsible for changing the visible state.
         */
        blog(
            LOG_WARNING,
            "[TrophyStream] Connexion WebSocket interrompue; conservation du dernier état"
        );

        if (
            InterlockedCompareExchange(
                &plugin_running,
                0,
                0) != 0) {

            WaitForSingleObject(
                websocket_stop_event,
                1000
            );
        }
    }

    blog(
        LOG_INFO,
        "[TrophyStream] Thread WebSocket arrete"
    );

    if (SUCCEEDED(com_result))
        CoUninitialize();

    return 0;
}

/*
 * ============================================================
 * SOURCE OBS
 * ============================================================
 */

struct trophy_stream_source {

    obs_source_t *source;

    uint32_t width;
    uint32_t height;

    uint8_t *frame_data;

    uint32_t frame_linesize;

    uint64_t frame_timestamp;
};

/*
 * ------------------------------------------------------------
 * Nom
 * ------------------------------------------------------------
 */

static const char *trophy_stream_get_name(
    void *unused)
{
    UNUSED_PARAMETER(unused);

    return "TrophyStream";
}

/*
 * ------------------------------------------------------------
 * Dessin de l'overlay permanent
 * ------------------------------------------------------------
 *
 * Surface OBS : 360 x 150.
 * ------------------------------------------------------------
 */
static HBITMAP notion_platinum_bitmap = NULL;
static int notion_platinum_bitmap_width = 0;
static int notion_platinum_bitmap_height = 0;
static bool notion_platinum_bitmap_tried = false;

static bool trophy_stream_load_platinum_icon(void)
{
    if (notion_platinum_bitmap_tried)
        return notion_platinum_bitmap != NULL;

    notion_platinum_bitmap_tried = true;

    char *icon_path = obs_module_file("platinum-trophy.png");
    if (!icon_path) {
        blog(LOG_WARNING,
             "[TrophyStream] Icone Platine PlayStation introuvable.");
        return false;
    }

    HRESULT co_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool should_uninitialize = (co_hr == S_OK || co_hr == S_FALSE);

    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    HBITMAP bitmap = NULL;

    HRESULT hr =
        CoCreateInstance(
            &CLSID_WICImagingFactory,
            NULL,
            CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory,
            (void **)&factory
        );

    if (SUCCEEDED(hr)) {
        wchar_t wide_path[MAX_PATH];
        int converted =
            MultiByteToWideChar(
                CP_UTF8,
                0,
                icon_path,
                -1,
                wide_path,
                MAX_PATH
            );

        if (!converted) {
            converted =
                MultiByteToWideChar(
                    CP_ACP,
                    0,
                    icon_path,
                    -1,
                    wide_path,
                    MAX_PATH
                );
        }

        if (converted) {
            hr =
                IWICImagingFactory_CreateDecoderFromFilename(
                    factory,
                    wide_path,
                    NULL,
                    GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad,
                    &decoder
                );
        } else {
            hr = E_FAIL;
        }
    }

    if (SUCCEEDED(hr)) {
        hr =
            IWICBitmapDecoder_GetFrame(
                decoder,
                0,
                &frame
            );
    }

    UINT source_width = 0;
    UINT source_height = 0;

    if (SUCCEEDED(hr)) {
        hr =
            IWICBitmapFrameDecode_GetSize(
                frame,
                &source_width,
                &source_height
            );
    }

    if (SUCCEEDED(hr)) {
        hr =
            IWICImagingFactory_CreateFormatConverter(
                factory,
                &converter
            );
    }

    if (SUCCEEDED(hr)) {
        hr =
            IWICFormatConverter_Initialize(
                converter,
                (IWICBitmapSource *)frame,
                &GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                NULL,
                0.0,
                WICBitmapPaletteTypeCustom
            );
    }

    if (SUCCEEDED(hr) && source_width > 0 && source_height > 0) {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));

        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = (LONG)source_width;
        bi.bmiHeader.biHeight = -(LONG)source_height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void *bits = NULL;

        bitmap =
            CreateDIBSection(
                NULL,
                &bi,
                DIB_RGB_COLORS,
                &bits,
                NULL,
                0
            );

        if (bitmap && bits) {
            const UINT stride = source_width * 4;
            const UINT buffer_size = stride * source_height;

            hr =
                IWICFormatConverter_CopyPixels(
                    converter,
                    NULL,
                    stride,
                    buffer_size,
                    (BYTE *)bits
                );

            if (FAILED(hr)) {
                DeleteObject(bitmap);
                bitmap = NULL;
            }
        }
    }

    if (converter)
        IWICFormatConverter_Release(converter);
    if (frame)
        IWICBitmapFrameDecode_Release(frame);
    if (decoder)
        IWICBitmapDecoder_Release(decoder);
    if (factory)
        IWICImagingFactory_Release(factory);

    bfree(icon_path);

    if (should_uninitialize)
        CoUninitialize();

    if (!bitmap) {
        blog(LOG_WARNING,
             "[TrophyStream] Impossible de charger platinum-trophy.png.");
        return false;
    }

    notion_platinum_bitmap = bitmap;
    notion_platinum_bitmap_width = (int)source_width;
    notion_platinum_bitmap_height = (int)source_height;

    blog(
        LOG_INFO,
        "[TrophyStream] Icone Platine PlayStation chargee : %dx%d",
        notion_platinum_bitmap_width,
        notion_platinum_bitmap_height
    );

    return true;
}

static void trophy_stream_draw_notion_stats(
    HDC dc,
    uint32_t width,
    uint32_t height)
{
    (void)height;

    if (!dc)
        return;

    int platinum_games = 0;
    int games_played = 0;
    bool valid = false;

    EnterCriticalSection(&progress_lock);

    platinum_games = notion_platinum_games;
    games_played = notion_games_played;
    valid = notion_stats_valid;

    LeaveCriticalSection(&progress_lock);

    if (!valid)
        return;

    char stats_text[64];

    snprintf(
        stats_text,
        sizeof(stats_text),
        "%d / %d",
        platinum_games,
        games_played
    );

    /*
     * V15 :
     * - vrai PNG du trophée Platine PlayStation
     * - icône légèrement agrandie
     * - meilleur alignement vertical avec le compteur
     * - aucun fond ni aucune bordure
     */
    const int margin_right = 12;
    const int top = 4;
    const int icon_size = 38;
    const int gap = 6;

    HFONT font =
        CreateFontW(
            22,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    HGDIOBJ old_font = SelectObject(dc, font);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(245, 245, 248));

    RECT measure_rect = { 0, 0, 200, 40 };

    DrawTextA(
        dc,
        stats_text,
        -1,
        &measure_rect,
        DT_SINGLELINE | DT_CALCRECT
    );

    const int text_width =
        measure_rect.right - measure_rect.left;

    const int total_width =
        icon_size + gap + text_width;

    const int start_x =
        (int)width - margin_right - total_width;

    /*
 * Vrai PNG du trophée Platine PlayStation.
     */
    if (trophy_stream_load_platinum_icon()) {
        int draw_width = icon_size;
        int draw_height = icon_size;

        if (notion_platinum_bitmap_width > 0 &&
            notion_platinum_bitmap_height > 0) {

            if (notion_platinum_bitmap_width >=
                notion_platinum_bitmap_height) {

                draw_height =
                    (icon_size *
                     notion_platinum_bitmap_height) /
                    notion_platinum_bitmap_width;
            } else {
                draw_width =
                    (icon_size *
                     notion_platinum_bitmap_width) /
                    notion_platinum_bitmap_height;
            }
        }

        const int icon_x =
            start_x + (icon_size - draw_width) / 2;

        const int icon_y =
            top + (icon_size - draw_height) / 2;

        HDC icon_dc = CreateCompatibleDC(dc);

        if (icon_dc) {
            HGDIOBJ old_bitmap =
                SelectObject(
                    icon_dc,
                    notion_platinum_bitmap
                );

            BLENDFUNCTION blend;
            blend.BlendOp = AC_SRC_OVER;
            blend.BlendFlags = 0;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;

            AlphaBlend(
                dc,
                icon_x,
                icon_y,
                draw_width,
                draw_height,
                icon_dc,
                0,
                0,
                notion_platinum_bitmap_width,
                notion_platinum_bitmap_height,
                blend
            );

            SelectObject(icon_dc, old_bitmap);
            DeleteDC(icon_dc);
        }
    }

    RECT text_rect = {
        start_x + icon_size + gap,
        top,
        (int)width - margin_right,
        top + icon_size
    };

    DrawTextA(
        dc,
        stats_text,
        -1,
        &text_rect,
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(dc, old_font);
    DeleteObject(font);
}

static void trophy_stream_draw_platinum_final_overlay(
    HDC dc,
    const char *game_title,
    int width,
    int height)
{
    (void)width;
    (void)height;

    if (!dc)
        return;

    /*
     * Ecran final dans le meme langage visuel que l overlay actuel :
     * fond 100 % transparent, texte blanc, accent bleu/argent,
     * petites lignes HUD et aucune barre de progression.
     */
    COLORREF accent = RGB(205, 230, 255);
    COLORREF accent_bright = RGB(245, 250, 255);
    COLORREF shadow_color = RGB(0, 0, 0);

    SetBkMode(dc, TRANSPARENT);

    /* Petit trait HUD superieur. */
    HPEN hud_pen = CreatePen(PS_SOLID, 2, accent);
    HGDIOBJ old_pen = SelectObject(dc, hud_pen);

    MoveToEx(dc, 28, 42, NULL);
    LineTo(dc, 145, 42);

    SelectObject(dc, old_pen);
    DeleteObject(hud_pen);

    /* Titre discret. */
    HFONT small_font = CreateFontW(
        20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    HGDIOBJ old_font = SelectObject(dc, small_font);
    RECT top_rect = {25, 25, 480, 55};

    SetTextColor(dc, shadow_color);
    RECT top_shadow = top_rect;
    OffsetRect(&top_shadow, 2, 2);
    trophy_stream_draw_text_utf8(
        dc, "TOUS LES TROPHEES OBTENUS", &top_shadow,
        DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, accent);
    trophy_stream_draw_text_utf8(
        dc, "TOUS LES TROPHEES OBTENUS", &top_rect,
        DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, old_font);
    DeleteObject(small_font);

    /* Message principal. */
    HFONT title_font = CreateFontW(
        42, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    old_font = SelectObject(dc, title_font);
    RECT title_rect = {25, 65, 480, 120};
    RECT title_shadow = title_rect;
    OffsetRect(&title_shadow, 2, 2);

    SetTextColor(dc, shadow_color);
    trophy_stream_draw_text_utf8(
        dc, "PLATINE OBTENU", &title_shadow,
        DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, accent_bright);
    trophy_stream_draw_text_utf8(
        dc, "PLATINE OBTENU", &title_rect,
        DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, old_font);
    DeleteObject(title_font);

    /* Nom du jeu. */
    HFONT game_font = CreateFontW(
        24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    old_font = SelectObject(dc, game_font);
    RECT game_rect = {25, 125, 480, 158};
    RECT game_shadow = game_rect;
    OffsetRect(&game_shadow, 1, 1);

    SetTextColor(dc, shadow_color);
    trophy_stream_draw_text_utf8(
        dc,
        game_title && game_title[0] ? game_title : "Jeu termine",
        &game_shadow,
        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SetTextColor(dc, RGB(245, 245, 245));
    trophy_stream_draw_text_utf8(
        dc,
        game_title && game_title[0] ? game_title : "Jeu termine",
        &game_rect,
        DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(dc, old_font);
    DeleteObject(game_font);

    /* Pourcentage final, sans barre de progression. */
    HFONT percent_font = CreateFontW(
        25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    old_font = SelectObject(dc, percent_font);
    RECT percent_rect = {150, 165, 350, 198};

    SetTextColor(dc, accent);
    trophy_stream_draw_text_utf8(
        dc, "100 %", &percent_rect,
        DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, old_font);
    DeleteObject(percent_font);

    /* Ligne de fermeture, toujours sans fond. */
    hud_pen = CreatePen(PS_SOLID, 2, accent);
    old_pen = SelectObject(dc, hud_pen);
    MoveToEx(dc, 100, 210, NULL);
    LineTo(dc, 350, 210);
    SelectObject(dc, old_pen);
    DeleteObject(hud_pen);
}

static void trophy_stream_draw_permanent_overlay(
    HDC dc,
    const char *trophy_name,
    const char *trophy_detail,
    const char *trophy_type,
    int unlocked,
    int total,
    float progress)
{
    if (!dc)
        return;

    /*
     * OVERLAY FIXE :
     * toujours la couleur de l'ancien overlay.
     */
    COLORREF accent = RGB(242, 200, 74);

    SetBkMode(dc, TRANSPARENT);

    /*
     *
     */
    const int tx = 18;
    const int ty = 18;

    HBRUSH trophy_brush =
        CreateSolidBrush(accent);

    HPEN trophy_pen =
        CreatePen(
            PS_SOLID,
            1,
            accent
        );

    HGDIOBJ old_brush =
        SelectObject(
            dc,
            trophy_brush
        );

    HGDIOBJ old_pen =
        SelectObject(
            dc,
            trophy_pen
        );

    RoundRect(
        dc,
        tx + 4,
        ty,
        tx + 16,
        ty + 10,
        3,
        3
    );

    MoveToEx(
        dc,
        tx + 2,
        ty + 2,
        NULL
    );

    LineTo(
        dc,
        tx + 2,
        ty + 8
    );

    LineTo(
        dc,
        tx + 5,
        ty + 10
    );

    MoveToEx(
        dc,
        tx + 18,
        ty + 2,
        NULL
    );

    LineTo(
        dc,
        tx + 18,
        ty + 8
    );

    LineTo(
        dc,
        tx + 15,
        ty + 10
    );

    Rectangle(
        dc,
        tx + 9,
        ty + 10,
        tx + 11,
        ty + 16
    );

    Rectangle(
        dc,
        tx + 5,
        ty + 16,
        tx + 15,
        ty + 18
    );

    SelectObject(
        dc,
        old_pen
    );

    SelectObject(
        dc,
        old_brush
    );

    DeleteObject(trophy_pen);
    DeleteObject(trophy_brush);

    char counter[64];

    snprintf(
        counter,
        sizeof(counter),
        "%d / %d",
        unlocked,
        total
    );

    HFONT counter_font =
        CreateFontW(
            25,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    HGDIOBJ old_font =
        SelectObject(dc, counter_font);

    SetTextColor(dc, RGB(245, 245, 245));

    RECT counter_rect = {
        54,
        14,
        185,
        40
    };

    trophy_stream_draw_text_utf8(
        dc,
        counter,
        &counter_rect,
        DT_LEFT | DT_SINGLELINE
    );

    SelectObject(dc, old_font);
    DeleteObject(counter_font);

    /*
     * Barre de progression.
     */
    const int bar_x = 50;
    const int bar_y = 40;
    const int bar_width = 245;
    const int bar_height = 9;

    HBRUSH bar_bg =
        CreateSolidBrush(RGB(95, 99, 108));

    RECT bar_rect = {
        bar_x,
        bar_y,
        bar_x + bar_width,
        bar_y + bar_height
    };

    FillRect(dc, &bar_rect, bar_bg);
    DeleteObject(bar_bg);

    float p = progress;

    if (p < 0.0f)
        p = 0.0f;

    if (p > 1.0f)
        p = 1.0f;

    int filled =
        (int)((float)bar_width * p);

    if (filled > 0) {
        HBRUSH bar_fill =
            CreateSolidBrush(accent);

        RECT fill_rect = {
            bar_x,
            bar_y,
            bar_x + filled,
            bar_y + bar_height
        };

        FillRect(dc, &fill_rect, bar_fill);
        DeleteObject(bar_fill);
    }

    HFONT title_font =
        CreateFontW(
            21,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    old_font = SelectObject(dc, title_font);

    SetTextColor(dc, RGB(255, 255, 255));

    RECT title_rect = {
        22,
        54,
        355,
        80
    };

    trophy_stream_draw_text_utf8(
        dc,
        trophy_name,
        &title_rect,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SelectObject(dc, old_font);
    DeleteObject(title_font);

    /*
     * Description.
     */
    HFONT detail_font =
        CreateFontW(
            19,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    old_font = SelectObject(dc, detail_font);

    /*
     * le texte lisible sur n'importe quelle image du jeu.
     */
    RECT detail_rect = {
        22,
        84,
        355,
        126
    };

    SetTextColor(dc, RGB(0, 0, 0));

    RECT detail_shadow_rect = detail_rect;
    OffsetRect(&detail_shadow_rect, 1, 1);

    trophy_stream_draw_text_utf8(
        dc,
        trophy_detail,
        &detail_shadow_rect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX
    );

    SetTextColor(dc, RGB(245, 245, 245));

    trophy_stream_draw_text_utf8(
        dc,
        trophy_detail,
        &detail_rect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX
    );

    SelectObject(dc, old_font);
    DeleteObject(detail_font);
}

/*
 * ------------------------------------------------------------
 * ------------------------------------------------------------
 */

static void trophy_stream_draw_game_change_overlay(
    HDC dc,
    const char *game_title,
    float animation,
    float opacity)
{
    if (!dc || !game_title || !game_title[0])
        return;

    /*
     * GAME CHANGE - rendu premium
     *
     * animation :
     * 0.0 = debut
     * 1.0 = affichage complet
     * puis retour vers 0.0 lors de la sortie.
     */

    if (animation < 0.0f)
        animation = 0.0f;

    if (animation > 1.0f)
        animation = 1.0f;

    float enter = animation * animation * (3.0f - 2.0f * animation);

    /*
     * Mouvement principal :
     * le panneau arrive depuis la droite.
     */
    int slide_x =
        (int)((1.0f - enter) * 150.0f);

    /*
     * Petit effet de profondeur pendant l'arrivee.
     */
    int lift_y =
        (int)((1.0f - enter) * 12.0f);

    int base_x = 20 + slide_x;
    int base_y = 25 + lift_y;

    const int panel_width = 560;
    const int panel_height = 140;

    RECT panel = {
        base_x,
        base_y,
        base_x + panel_width,
        base_y + panel_height
    };

    COLORREF accent = RGB(90, 170, 255);
    COLORREF accent_bright = RGB(150, 215, 255);
    COLORREF panel_bg = RGB(10, 23, 42);

    /*
     * Ombre externe.
     */
    HBRUSH shadow =
        CreateSolidBrush(RGB(0, 0, 0));

    RECT shadow_rect = {
        panel.left + 9,
        panel.top + 10,
        panel.right + 9,
        panel.bottom + 10
    };

    FillRect(dc, &shadow_rect, shadow);
    DeleteObject(shadow);

    /*
     * Glow externe en plusieurs couches.
     */
    HPEN glow_outer =
        CreatePen(PS_SOLID, 7, RGB(25, 65, 105));

    HGDIOBJ old_pen =
        SelectObject(dc, glow_outer);

    HGDIOBJ old_brush =
        SelectObject(
            dc,
            GetStockObject(NULL_BRUSH)
        );

    RoundRect(
        dc,
        panel.left - 3,
        panel.top - 3,
        panel.right + 3,
        panel.bottom + 3,
        23,
        23
    );

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(glow_outer);

    /*
     * Fond principal.
     */
    HBRUSH background =
        CreateSolidBrush(panel_bg);

    old_brush =
        SelectObject(dc, background);

    old_pen =
        SelectObject(
            dc,
            GetStockObject(NULL_PEN)
        );

    RoundRect(
        dc,
        panel.left,
        panel.top,
        panel.right,
        panel.bottom,
        20,
        20
    );

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(background);

    /*
     * Bordure principale.
     */
    HPEN border =
        CreatePen(PS_SOLID, 2, accent);

    old_pen =
        SelectObject(dc, border);

    old_brush =
        SelectObject(
            dc,
            GetStockObject(NULL_BRUSH)
        );

    RoundRect(
        dc,
        panel.left,
        panel.top,
        panel.right,
        panel.bottom,
        20,
        20
    );

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    /*
     * Ligne lumineuse superieure.
     */
    HPEN top_line =
        CreatePen(PS_SOLID, 3, accent_bright);

    old_pen =
        SelectObject(dc, top_line);

    MoveToEx(
        dc,
        panel.left + 24,
        panel.top + 2,
        NULL
    );

    LineTo(
        dc,
        panel.right - 24,
        panel.top + 2
    );

    SelectObject(dc, old_pen);
    DeleteObject(top_line);

    /*
     * Jaquette :
     * petit zoom progressif pendant l'entree.
     */
    const int base_icon_size = 64;

    float icon_scale =
        0.82f + (0.18f * enter);

    int icon_size =
        (int)(base_icon_size * icon_scale);

    if (icon_size < 60)
        icon_size = 60;

    int icon_x =
        base_x + 18 +
        (base_icon_size - icon_size) / 2;

    int icon_y =
        base_y + 18 +
        (base_icon_size - icon_size) / 2;

    /*
     * Cadre lumineux autour de la jaquette.
     */
    HPEN icon_glow =
        CreatePen(
            PS_SOLID,
            5,
            RGB(35, 90, 145)
        );

    old_pen =
        SelectObject(dc, icon_glow);

    old_brush =
        SelectObject(
            dc,
            GetStockObject(NULL_BRUSH)
        );

    RoundRect(
        dc,
        icon_x - 3,
        icon_y - 3,
        icon_x + icon_size + 3,
        icon_y + icon_size + 3,
        13,
        13
    );

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(icon_glow);

    /*
     * Fond de secours sous la jaquette.
     */
    HBRUSH icon_bg =
        CreateSolidBrush(RGB(20, 35, 58));

    old_brush =
        SelectObject(dc, icon_bg);

    old_pen =
        SelectObject(
            dc,
            GetStockObject(NULL_PEN)
        );

    RoundRect(
        dc,
        icon_x,
        icon_y,
        icon_x + icon_size,
        icon_y + icon_size,
        10,
        10
    );

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(icon_bg);

    /*
     * Vraie jaquette PSN figee.
     */
    trophy_stream_draw_game_change_frozen_icon(
        dc,
        icon_x,
        icon_y,
        icon_size,
        icon_size
    );

    SetBkMode(dc, TRANSPARENT);

    /*
     * Petit indicateur lumineux.
     */
    HPEN accent_line =
        CreatePen(
            PS_SOLID,
            3,
            accent_bright
        );

    old_pen =
        SelectObject(dc, accent_line);

    MoveToEx(
        dc,
        base_x + 100,
        base_y + 27,
        NULL
    );

    LineTo(
        dc,
        base_x + 145,
        base_y + 27
    );

    SelectObject(dc, old_pen);
    DeleteObject(accent_line);

    /*
     * "NOUVEAU JEU"
     */
    HFONT label_font =
        CreateFontW(
            18,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    HGDIOBJ saved_font =
        SelectObject(dc, label_font);

    RECT label_rect = {
        base_x + 100,
        base_y + 18,
        base_x + 535,
        base_y + 42
    };

    RECT label_shadow = label_rect;
    OffsetRect(&label_shadow, 2, 2);

    SetTextColor(dc, RGB(0, 0, 0));

    trophy_stream_draw_text_utf8(
        dc,
        "NOUVEAU JEU",
        &label_shadow,
        DT_LEFT | DT_SINGLELINE
    );

    SetTextColor(dc, accent_bright);

    trophy_stream_draw_text_utf8(
        dc,
        "NOUVEAU JEU",
        &label_rect,
        DT_LEFT | DT_SINGLELINE
    );

    SelectObject(dc, saved_font);
    DeleteObject(label_font);

    /*
     * Titre du jeu.
     */
    HFONT title_font =
        CreateFontW(
            32,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    saved_font =
        SelectObject(dc, title_font);

    RECT title_rect = {
        base_x + 100,
        base_y + 45,
        base_x + 535,
        base_y + 100
    };

    RECT title_shadow = title_rect;
    OffsetRect(&title_shadow, 3, 3);

    SetTextColor(dc, RGB(0, 0, 0));

    trophy_stream_draw_text_utf8(
        dc,
        game_title,
        &title_shadow,
        DT_LEFT |
        DT_WORDBREAK |
        DT_END_ELLIPSIS |
        DT_NOPREFIX
    );

    SetTextColor(dc, RGB(255, 255, 255));

    trophy_stream_draw_text_utf8(
        dc,
        game_title,
        &title_rect,
        DT_LEFT |
        DT_WORDBREAK |
        DT_END_ELLIPSIS |
        DT_NOPREFIX
    );

    SelectObject(dc, saved_font);
    DeleteObject(title_font);

    /*
     * Petite ligne decorative inferieure.
     */
    HPEN bottom_line =
        CreatePen(
            PS_SOLID,
            2,
            RGB(45, 95, 145)
        );

    old_pen =
        SelectObject(dc, bottom_line);

    SelectObject(dc, old_pen);
    DeleteObject(bottom_line);

    UNUSED_PARAMETER(opacity);
}

static void trophy_stream_generate_frame(
    struct trophy_stream_source *source)
{
    if (!source || !source->frame_data)
        return;

    const uint32_t width = source->width;
    const uint32_t height = source->height;

    EnterCriticalSection(&progress_lock);
    bool display_enabled = trophy_stream_display_enabled;
    LeaveCriticalSection(&progress_lock);

    if (!display_enabled) {
        memset(source->frame_data, 0, (size_t)source->frame_linesize * height);
        return;
    }

    /*
     * --------------------------------------------------------
     * Animation de changement de jeu
     * --------------------------------------------------------
     *
     * 600 ms entree
     * 1800 ms affichage
     * 600 ms sortie
     *
     * Elle passe prioritairement devant les animations de trophees.
     */
    char game_change_title[256] = "";
    bool game_change_active = false;
    bool restore_paused_trophy_icon = false;
    char paused_trophy_icon_url[1024] = "";
    uint64_t game_change_elapsed = 0;
    trophy_animation_item game_change_next_item;
    bool game_change_next_item_valid = false;

    memset(
        &game_change_next_item,
        0,
        sizeof(game_change_next_item)
    );

    EnterCriticalSection(&progress_lock);

    if (game_change_visible) {
        uint64_t now = GetTickCount64();
        game_change_elapsed = now - game_change_show_time;

        /*
         * Animation totale : 600 ms entree + 8800 ms affichage
         * + 600 ms sortie = 10000 ms.
         */
        if (game_change_elapsed >= 10000ULL) {
            game_change_visible = false;

            if (trophy_paused_for_game_change && current_trophy_visible) {
                uint64_t now = GetTickCount64();
                trophy_show_time =
                    now >= trophy_paused_elapsed
                        ? now - trophy_paused_elapsed
                        : now;
                trophy_paused_for_game_change = false;
                if (trophy_paused_icon_url[0]) {
                    snprintf(
                        paused_trophy_icon_url,
                        sizeof(paused_trophy_icon_url),
                        "%s",
                        trophy_paused_icon_url
                    );
                    restore_paused_trophy_icon = true;
                }
                trophy_paused_icon_url[0] = '\0';
                blog(
                    LOG_INFO,
                    "[TrophyStream] Trophée repris après GAME-CHANGE"
                );
            } else if (!current_trophy_visible &&
                       trophy_stream_pop_next_item_locked(
                           &game_change_next_item)) {
                trophy_stream_start_item_locked(
                    &game_change_next_item
                );
                game_change_next_item_valid = true;
            }

        } else {
            game_change_active = true;

            snprintf(
                game_change_title,
                sizeof(game_change_title),
                "%s",
                game_change_display_title
            );
        }
    }

    LeaveCriticalSection(&progress_lock);

    if (restore_paused_trophy_icon && paused_trophy_icon_url[0]) {
        trophy_stream_set_icon_url(paused_trophy_icon_url);
    }

    if (game_change_next_item_valid) {
        trophy_stream_play_sound_for_type(
            game_change_next_item.type
        );

        if (game_change_next_item.icon_url[0]) {
            trophy_stream_set_icon_url(
                game_change_next_item.icon_url
            );
        }

        blog(
            LOG_INFO,
            "[TrophyStream] Trophée en attente lancé après GAME-CHANGE : %s",
            game_change_next_item.name
        );
    }

    if (game_change_active) {
        uint64_t elapsed = game_change_elapsed;

        const uint64_t enter_time = 600;
        const uint64_t visible_time = 8800;
        const uint64_t exit_time = 600;

        float animation = 1.0f;
        float opacity = 1.0f;

        if (elapsed < enter_time) {
            float t =
                (float)elapsed /
                (float)enter_time;

            float inv = 1.0f - t;
            animation =
                1.0f - inv * inv * inv;
        } else if (elapsed > enter_time + visible_time) {
            uint64_t exit_elapsed =
                elapsed - enter_time - visible_time;

            float t =
                (float)exit_elapsed /
                (float)exit_time;

            if (t > 1.0f)
                t = 1.0f;

            float eased = t * t * t;
            animation = 1.0f - eased;
        }

        HDC game_dc =
            CreateCompatibleDC(NULL);

        if (!game_dc)
            return;

        BITMAPINFO game_info;
        memset(&game_info, 0, sizeof(game_info));

        game_info.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);
        game_info.bmiHeader.biWidth =
            (LONG)width;
        game_info.bmiHeader.biHeight =
            -(LONG)height;
        game_info.bmiHeader.biPlanes = 1;
        game_info.bmiHeader.biBitCount = 32;
        game_info.bmiHeader.biCompression = BI_RGB;

        void *game_bits = NULL;

        HBITMAP game_bitmap =
            CreateDIBSection(
                game_dc,
                &game_info,
                DIB_RGB_COLORS,
                &game_bits,
                NULL,
                0
            );

        if (!game_bitmap || !game_bits) {
            if (game_bitmap)
                DeleteObject(game_bitmap);

            DeleteDC(game_dc);
            return;
        }

        HGDIOBJ game_old_bitmap =
            SelectObject(
                game_dc,
                game_bitmap
            );

        memset(
            game_bits,
            0,
            (size_t)width * height * 4
        );

        SetGraphicsMode(
            game_dc,
            GM_ADVANCED
        );

        XFORM game_scale;
        game_scale.eM11 = 0.60f;
        game_scale.eM12 = 0.0f;
        game_scale.eM21 = 0.0f;
        game_scale.eM22 = 0.60f;
        game_scale.eDx = 0.0f;
        game_scale.eDy = 0.0f;

        SetWorldTransform(
            game_dc,
            &game_scale
        );

        trophy_stream_draw_game_change_overlay(
            game_dc,
            game_change_title,
            animation,
            opacity
        );

        /*
         * Stats Notion toujours visibles,
         */
        ModifyWorldTransform(
            game_dc,
            NULL,
            MWT_IDENTITY
        );

        GdiFlush();

        memcpy(
            source->frame_data,
            game_bits,
            (size_t)width * height * 4
        );

        uint8_t *game_pixels =
            source->frame_data;

        uint8_t game_alpha =
            (uint8_t)(255.0f * opacity);

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t *pixel =
                    game_pixels +
                    (size_t)y *
                    source->frame_linesize +
                    x * 4;

                if (pixel[0] != 0 ||
                    pixel[1] != 0 ||
                    pixel[2] != 0) {
                    pixel[3] = game_alpha;
                }
            }
        }

        SelectObject(
            game_dc,
            game_old_bitmap
        );

        DeleteObject(game_bitmap);
        DeleteDC(game_dc);

        return;
    }

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    char trophy_name[256];
    char trophy_detail[512];
    char trophy_type[32];
    char trophy_icon_url[1024];
    bool visible;
    bool final_platinum_visible;
    int unlocked;
    int total;
    float progress;
    char game_title[256];
    char rotated_icon_url[1024] = "";
    bool remaining_rotation_active = false;

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     *
     * Toutes les 5 secondes, on passe au suivant.
     *
     * l'index avance circulairement :
     *
     *   1 -> 2 -> 3 -> ... -> N -> 1 -> ...
     *
     */
    EnterCriticalSection(&progress_lock);

    if (current_remaining_count > 0 &&
    !current_trophy_visible &&
    !platinum_final_visible) {

        remaining_rotation_active = true;

        uint64_t now = GetTickCount64();

        if (current_remaining_index < 0 ||
            current_remaining_index >= current_remaining_count) {
            current_remaining_index = 0;
        }

        if (remaining_rotation_next_tick == 0) {
            
            remaining_rotation_next_tick =
                now + 5000ULL;
        }

        uint64_t elapsed = now - remaining_rotation_next_tick;

        if (elapsed >= 5000ULL) {
            uint64_t steps = elapsed / 5000ULL;

            current_remaining_index =
                (current_remaining_index +
                 (int)(steps % (uint64_t)current_remaining_count)) %
                current_remaining_count;

            remaining_rotation_next_tick += steps * 5000ULL;

            blog(
                LOG_INFO,
                "[TrophyStream] Rotation manque : %d/%d - %s",
                current_remaining_index + 1,
                current_remaining_count,
                current_remaining_names[current_remaining_index]
            );
        }

        snprintf(
            current_trophy_name,
            sizeof(current_trophy_name),
            "%s",
            current_remaining_names[current_remaining_index]
        );

        snprintf(
            current_trophy_detail,
            sizeof(current_trophy_detail),
            "%s",
            current_remaining_details[current_remaining_index]
        );

        snprintf(
            current_trophy_type,
            sizeof(current_trophy_type),
            "%s",
            current_remaining_types[current_remaining_index][0]
                ? current_remaining_types[current_remaining_index]
                : "bronze"
        );

        snprintf(
            rotated_icon_url,
            sizeof(rotated_icon_url),
            "%s",
            current_remaining_icons[current_remaining_index]
        );
    }

    snprintf(
        trophy_name,
        sizeof(trophy_name),
        "%s",
        current_trophy_name
    );

    snprintf(
        trophy_detail,
        sizeof(trophy_detail),
        "%s",
        current_trophy_detail
    );

    snprintf(
        trophy_type,
        sizeof(trophy_type),
        "%s",
        current_trophy_type
    );

    EnterCriticalSection(&icon_lock);

    snprintf(
        trophy_icon_url,
        sizeof(trophy_icon_url),
        "%s",
        current_trophy_icon_url
    );

    LeaveCriticalSection(&icon_lock);

    visible = current_trophy_visible;
    final_platinum_visible = platinum_final_visible;
    unlocked = current_unlocked;
    total = current_total;
    progress = current_progress;
    snprintf(game_title, sizeof(game_title), "%s", current_game_title);

    LeaveCriticalSection(&progress_lock);

    if (rotated_icon_url[0])
        trophy_stream_set_icon_url(rotated_icon_url);

    /* Commentaire technique corrigé. */
    memset(
        source->frame_data,
        0,
        (size_t)source->frame_linesize * height
    );

    /*
     * l overlay permanent et supprime la barre de progression.
     */
    if (final_platinum_visible && !game_change_active) {
        HDC final_dc = CreateCompatibleDC(NULL);

        if (!final_dc)
            return;

        BITMAPINFO final_info;
        memset(&final_info, 0, sizeof(final_info));
        final_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        final_info.bmiHeader.biWidth = (LONG)width;
        final_info.bmiHeader.biHeight = -(LONG)height;
        final_info.bmiHeader.biPlanes = 1;
        final_info.bmiHeader.biBitCount = 32;
        final_info.bmiHeader.biCompression = BI_RGB;

        void *final_bits = NULL;
        HBITMAP final_bitmap = CreateDIBSection(
            final_dc, &final_info, DIB_RGB_COLORS,
            &final_bits, NULL, 0);

        if (!final_bitmap || !final_bits) {
            if (final_bitmap)
                DeleteObject(final_bitmap);
            DeleteDC(final_dc);
            return;
        }

        HGDIOBJ final_old_bitmap = SelectObject(final_dc, final_bitmap);
        memset(final_bits, 0, (size_t)width * height * 4);

        SetGraphicsMode(final_dc, GM_ADVANCED);
        XFORM final_scale;
        final_scale.eM11 = 0.5f;
        final_scale.eM12 = 0.0f;
        final_scale.eM21 = 0.0f;
        final_scale.eM22 = 0.5f;
        final_scale.eDx = 0.0f;
        final_scale.eDy = 0.0f;
        SetWorldTransform(final_dc, &final_scale);

        trophy_stream_draw_platinum_final_overlay(
            final_dc, game_title, width, height);

        ModifyWorldTransform(final_dc, NULL, MWT_IDENTITY);

        GdiFlush();

        memcpy(source->frame_data, final_bits,
               (size_t)width * height * 4);

        uint8_t *final_pixels = source->frame_data;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t *pixel = final_pixels +
                    (size_t)y * source->frame_linesize + x * 4;
                if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)
                    pixel[3] = 255;
            }
        }

        SelectObject(final_dc, final_old_bitmap);
        DeleteObject(final_bitmap);
        DeleteDC(final_dc);
        return;
    }
    /*
     * Aucun backend / aucun jeu actif :
     * la source OBS reste totalement transparente.
     */

if (current_remaining_count > 0 &&
    !visible &&
    !final_platinum_visible)
    remaining_rotation_active = true;

    if (!current_game_active &&
        !remaining_rotation_active &&
        !notion_stats_valid)
        return;

    /*
     * --------------------------------------------------------
     * Overlay permanent
     * --------------------------------------------------------
     *
     * uniquement le HUD transparent compact.
     */
    if (!visible || remaining_rotation_active) {
        HDC normal_dc = CreateCompatibleDC(NULL);

        if (!normal_dc)
            return;

        BITMAPINFO normal_info;
        memset(&normal_info, 0, sizeof(normal_info));

        normal_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        normal_info.bmiHeader.biWidth = (LONG)width;
        normal_info.bmiHeader.biHeight = -(LONG)height;
        normal_info.bmiHeader.biPlanes = 1;
        normal_info.bmiHeader.biBitCount = 32;
        normal_info.bmiHeader.biCompression = BI_RGB;

        void *normal_bits = NULL;

        HBITMAP normal_bitmap =
            CreateDIBSection(
                normal_dc,
                &normal_info,
                DIB_RGB_COLORS,
                &normal_bits,
                NULL,
                0
            );

        if (!normal_bitmap || !normal_bits) {
            if (normal_bitmap)
                DeleteObject(normal_bitmap);

            DeleteDC(normal_dc);
            return;
        }

        HGDIOBJ normal_old_bitmap =
            SelectObject(normal_dc, normal_bitmap);

        memset(
            normal_bits,
            0,
            (size_t)width * height * 4
        );

        if (current_game_active ||
            remaining_rotation_active) {

            trophy_stream_draw_permanent_overlay(
                normal_dc,
                trophy_name,
                trophy_detail,
                trophy_type,
                unlocked,
                total,
                progress
            );
        }

        if (!current_trophy_visible &&
            !game_change_active &&
            !platinum_final_visible) {
            trophy_stream_draw_notion_stats(
                normal_dc,
                width,
                height
            );
        }

        GdiFlush();

        memcpy(
            source->frame_data,
            normal_bits,
            (size_t)width * height * 4
        );

        /*
         * GDI dessine correctement les couleurs mais laisse
         * Pour l'overlay permanent, cela rendrait tout le HUD
         * invisible dans OBS. On rend donc opaques uniquement
         */
        uint8_t *normal_pixels =
            source->frame_data;

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t *pixel =
                    normal_pixels +
                    (size_t)y *
                    source->frame_linesize +
                    x * 4;

                if (pixel[0] != 0 ||
                    pixel[1] != 0 ||
                    pixel[2] != 0) {
                    pixel[3] = 255;
                }
            }
        }

        SelectObject(
            normal_dc,
            normal_old_bitmap
        );

        DeleteObject(normal_bitmap);
        DeleteDC(normal_dc);

        return;
    }

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     *
     * 10000 ms affichage
     * 700 ms sortie
     *
     */
    uint64_t now = GetTickCount64();
    uint64_t elapsed = now - trophy_show_time;

    const uint64_t enter_time = 700;
    const uint64_t visible_time = current_trophy_duration;
    const uint64_t exit_time = 700;

    float animation = 1.0f;
    float opacity = 1.0f;

    if (!remaining_rotation_active && elapsed < enter_time) {
        float t =
            (float)elapsed /
            (float)enter_time;

        /* easeOutCubic */
        float inv = 1.0f - t;
        animation = 1.0f - inv * inv * inv;
    }
    else if (!remaining_rotation_active && elapsed > enter_time + visible_time) {
        uint64_t exit_elapsed =
            elapsed - enter_time - visible_time;

        if (exit_elapsed >= exit_time) {

            trophy_animation_item next_item;
            bool has_next = false;

            EnterCriticalSection(
                &progress_lock
            );

            has_next =
                trophy_stream_pop_next_item_locked(
                    &next_item
                );

            if (has_next) {

                /*
                 * l'overlay permanent.
                 */
                trophy_stream_start_item_locked(
                    &next_item
                );

            } else {

                /*
                 * Le platine garde son animation habituelle jusqu a
                 * la fin des 10 secondes. Seulement ensuite, si c etait
                 * le platine final du jeu, on passe a l ecran permanent.
                 */
                bool show_platinum_final =
                    current_trophy_is_platinum_completion;

                current_trophy_visible = false;
                current_trophy_is_platinum_completion = false;

                if (show_platinum_final) {
                    platinum_final_visible = true;
                    blog(
                        LOG_INFO,
                        "[TrophyStream] PLATINE FINAL : ecran permanent active"
                    );
                }
            }

            LeaveCriticalSection(
                &progress_lock
            );

            if (has_next) {

                trophy_stream_play_sound_for_type(
                    next_item.type
                );

                if (next_item.icon_url[0]) {
                    trophy_stream_set_icon_url(
                        next_item.icon_url
                    );
                }

                blog(
                    LOG_INFO,
                    "[TrophyStream] Trophée suivant lancé : %s",
                    next_item.name
                );

            }

            return;
        }

        float t =
            (float)exit_elapsed /
            (float)exit_time;

        /*
         * Sortie :
         *
         *
         */
        float eased = t * t * t;

        animation = 1.0f - eased;
        opacity = 1.0f;
    }

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    HDC dc = CreateCompatibleDC(NULL);

    if (!dc)
        return;

    BITMAPINFO bitmap_info;
    memset(&bitmap_info, 0, sizeof(bitmap_info));

    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = (LONG)width;
    bitmap_info.bmiHeader.biHeight = -(LONG)height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;

    HBITMAP bitmap =
        CreateDIBSection(
            dc,
            &bitmap_info,
            DIB_RGB_COLORS,
            &bits,
            NULL,
            0
        );

    if (!bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);

        DeleteDC(dc);
        return;
    }

    HGDIOBJ old_bitmap =
        SelectObject(dc, bitmap);

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     *
     * La source OBS est volontairement compacte (360x150).
     *
     * au lieu d'englober une grande surface transparente.
     * --------------------------------------------------------
     */
    SetGraphicsMode(dc, GM_ADVANCED);

    XFORM trophy_scale;
    trophy_scale.eM11 = 0.5f;
    trophy_scale.eM12 = 0.0f;
    trophy_scale.eM21 = 0.0f;
    trophy_scale.eM22 = 0.5f;
    trophy_scale.eDx = 0.0f;
    trophy_scale.eDy = 0.0f;

    SetWorldTransform(
        dc,
        &trophy_scale
    );

    /* Commentaire technique corrigé. */
    memset(
        bits,
        0,
        (size_t)width * height * 4
    );

    /*
     * --------------------------------------------------------
     * Position du petit bandeau
     * --------------------------------------------------------
     *
     */
    int slide_x =
        (int)((1.0f - animation) * 120.0f);

    int base_x = 30 + slide_x;
    int base_y = 45;

    const int canvas_width = 960;
const int canvas_height = 540;

const int panel_width = 650;
const int panel_height = 160;

RECT panel = {
    base_x,
    base_y,
    base_x + panel_width,
    base_y + panel_height
};

    /*
     * --------------------------------------------------------
     * COULEUR UNIQUE DE L'OVERLAY
     * --------------------------------------------------------
     *
     * IMPORTANT :
     * Le panneau permanent ne change PLUS de couleur selon
     * bronze / argent / or / platine.
     *
     *
     * Ainsi :
     *
     *   OVERLAY permanent
     *
     *   ANIMATION
     */
    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     *
     * L'overlay permanent reste uniforme.
     * Ici, uniquement le bandeau de l'animation change de
     * couleur selon trophy_type.
     */
    COLORREF accent = RGB(242, 200, 74);
    COLORREF accent_dark = RGB(145, 102, 18);
    COLORREF panel_bg = RGB(55, 42, 10);

    if (trophy_type != NULL) {

        if (_stricmp(trophy_type, "bronze") == 0) {
            /* Bronze */
            accent = RGB(255, 145, 55);
            accent_dark = RGB(150, 70, 20);
            panel_bg = RGB(55, 28, 8);
        }
        else if (_stricmp(trophy_type, "silver") == 0) {
            /* Argent */
            accent = RGB(220, 232, 255);
            accent_dark = RGB(100, 110, 125);
            panel_bg = RGB(32, 38, 48);
        }
        else if (_stricmp(trophy_type, "gold") == 0) {
            /* Or */
            accent = RGB(255, 205, 70);
            accent_dark = RGB(145, 102, 18);
            panel_bg = RGB(55, 42, 10);
        }
        else if (_stricmp(trophy_type, "platinum") == 0) {
            /* Platine */
            accent = RGB(225, 245, 255);
            accent_dark = RGB(100, 125, 145);
            panel_bg = RGB(28, 38, 48);
        }
    }

    /*
     *
     *
     */
    const char *type_label = "TROPH" "\xC3\x89" "E";

    if (trophy_type != NULL) {
        if (_stricmp(trophy_type, "bronze") == 0) {
            type_label = "TROPH" "\xC3\x89" "E BRONZE";
        }
        else if (_stricmp(trophy_type, "silver") == 0) {
            type_label = "TROPH" "\xC3\x89" "E ARGENT";
        }
        else if (_stricmp(trophy_type, "gold") == 0) {
            type_label = "TROPH" "\xC3\x89" "E OR";
        }
        else if (_stricmp(trophy_type, "platinum") == 0) {
            type_label = "TROPH" "\xC3\x89" "E PLATINE";
        }
    }

    /*
     * --------------------------------------------------------
     * Ombre du petit panneau
     * --------------------------------------------------------
     */
    HBRUSH shadow =
        CreateSolidBrush(RGB(0, 0, 0));

    RECT shadow_rect = {
        panel.left + 7,
        panel.top + 8,
        panel.right + 7,
        panel.bottom + 8
    };

    FillRect(dc, &shadow_rect, shadow);
    DeleteObject(shadow);

    /*
     * --------------------------------------------------------
     * Fond brun du bandeau
     * --------------------------------------------------------
     */
    HBRUSH background =
        CreateSolidBrush(panel_bg);

    HGDIOBJ old_brush =
        SelectObject(dc, background);

    HGDIOBJ old_pen =
        SelectObject(
            dc,
            GetStockObject(NULL_PEN)
        );

    RoundRect(
        dc,
        panel.left,
        panel.top,
        panel.right,
        panel.bottom,
        20,
        20
    );

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(background);

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    HPEN border =
        CreatePen(PS_SOLID, 2, accent);

    old_pen = SelectObject(dc, border);
    old_brush = SelectObject(
        dc,
        GetStockObject(NULL_BRUSH)
    );

    RoundRect(
        dc,
        panel.left,
        panel.top,
        panel.right,
        panel.bottom,
        20,
        20
    );

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    HPEN top_line =
        CreatePen(PS_SOLID, 2, accent);

    old_pen = SelectObject(dc, top_line);

    MoveToEx(
        dc,
        panel.left + 18,
        panel.top + 2,
        NULL
    );

    LineTo(
        dc,
        panel.right - 18,
        panel.top + 2
    );

    SelectObject(dc, old_pen);
    DeleteObject(top_line);

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */

    const int icon_x = base_x + 28;
    const int icon_y = base_y + 25;
    const int icon_size = 82;

    HBRUSH icon_bg =
        CreateSolidBrush(RGB(24, 18, 8));

    old_brush = SelectObject(dc, icon_bg);
    old_pen = SelectObject(
        dc,
        GetStockObject(NULL_PEN)
    );

    RoundRect(
        dc,
        icon_x,
        icon_y,
        icon_x + icon_size,
        icon_y + icon_size,
        8,
        8
    );

    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(icon_bg);

    HPEN icon_border =
        CreatePen(PS_SOLID, 2, accent_dark);

    old_pen = SelectObject(dc, icon_border);
    old_brush = SelectObject(
        dc,
        GetStockObject(NULL_BRUSH)
    );

    Rectangle(
        dc,
        icon_x + 2,
        icon_y + 2,
        icon_x + icon_size - 2,
        icon_y + icon_size - 2
    );

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(icon_border);

    const int trophy_w = 58;
    const int trophy_h = 58;

    int trophy_x =
        icon_x + (icon_size - trophy_w) / 2;

    int trophy_y =
        icon_y + (icon_size - trophy_h) / 2;

    bool icon_drawn =
        trophy_stream_draw_cached_icon(
            dc,
            trophy_x,
            trophy_y,
            trophy_w,
            trophy_h
        );

    if (!icon_drawn) {

        HBRUSH trophy_brush =
            CreateSolidBrush(accent);

        HPEN trophy_pen =
            CreatePen(PS_SOLID, 1, accent_dark);

        old_brush =
            SelectObject(
                dc,
                trophy_brush
            );

        old_pen =
            SelectObject(
                dc,
                trophy_pen
            );

        RECT cup = {
            trophy_x + 14,
            trophy_y + 3,
            trophy_x + 44,
            trophy_y + 35
        };

        RoundRect(
            dc,
            cup.left,
            cup.top,
            cup.right,
            cup.bottom,
            6,
            6
        );

        Rectangle(
            dc,
            trophy_x + 27,
            trophy_y + 34,
            trophy_x + 31,
            trophy_y + 49
        );

        Rectangle(
            dc,
            trophy_x + 19,
            trophy_y + 47,
            trophy_x + 39,
            trophy_y + 53
        );

        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);

        DeleteObject(trophy_pen);
        DeleteObject(trophy_brush);
    }

    /*
     * --------------------------------------------------------
     * --------------------------------------------------------
     */
    SetBkMode(dc, TRANSPARENT);

    /* Commentaire technique corrigé. */
    HFONT type_font =
        CreateFontW(
            18,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    HFONT old_font =
        SelectObject(dc, type_font);

    COLORREF text_accent = RGB(242, 200, 74);

    if (_stricmp(trophy_type, "bronze") == 0)
        text_accent = RGB(205, 127, 50);
    else if (_stricmp(trophy_type, "silver") == 0)
        text_accent = RGB(220, 225, 235);
    else if (_stricmp(trophy_type, "gold") == 0)
        text_accent = RGB(242, 200, 74);
    else if (_stricmp(trophy_type, "platinum") == 0)
        text_accent = RGB(156, 123, 255);

    RECT type_rect = {
        base_x + 132,
        base_y + 25,
        base_x + 610,
        base_y + 48
    };

    RECT type_shadow_rect = type_rect;
    OffsetRect(&type_shadow_rect, 2, 2);

    SetTextColor(dc, RGB(0, 0, 0));

    trophy_stream_draw_text_utf8(
        dc,
        type_label,
        &type_shadow_rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SetTextColor(dc, text_accent);

    trophy_stream_draw_text_utf8(
        dc,
        type_label,
        &type_rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SelectObject(dc, old_font);
    DeleteObject(type_font);

    /* Titre : blanc, compact, gras. */
    HFONT title_font =
        CreateFontW(
            32,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    old_font = SelectObject(dc, title_font);

    SetTextColor(dc, RGB(255, 255, 255));

    RECT title_rect = {
        base_x + 132,
        base_y + 47,
        base_x + 615,
        base_y + 82
    };

    trophy_stream_draw_text_utf8(
        dc,
        trophy_name,
        &title_rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SelectObject(dc, old_font);
    DeleteObject(title_font);

    /* Description : claire et lisible, avec ombre. */
    HFONT detail_font =
        CreateFontW(
            18,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

    old_font = SelectObject(dc, detail_font);

    /*
     * Cela conserve le style actuel mais la rend lisible sur le jeu.
     */
    /*
     * Description multi-ligne :
     */
    RECT detail_rect = {
        base_x + 132,
        base_y + 86,
        base_x + 615,
        base_y + 148
    };

    SetTextColor(dc, RGB(0, 0, 0));

    RECT detail_shadow_rect = detail_rect;
    OffsetRect(&detail_shadow_rect, 2, 2);

    trophy_stream_draw_text_utf8(
        dc,
        trophy_detail,
        &detail_shadow_rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SetTextColor(dc, RGB(245, 245, 245));

    trophy_stream_draw_text_utf8(
        dc,
        trophy_detail,
        &detail_rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS
    );

    SelectObject(dc, old_font);
    DeleteObject(detail_font);

    /*
     * --------------------------------------------------------
     * Stats Notion
     * --------------------------------------------------------
     *
     */
    ModifyWorldTransform(
        dc,
        NULL,
        MWT_IDENTITY
    );

    GdiFlush();

    /*
     * --------------------------------------------------------
     * Copie du DIB dans le buffer OBS
     * --------------------------------------------------------
     */
    memcpy(
        source->frame_data,
        bits,
        (size_t)width * height * 4
    );

    uint8_t *pixels = source->frame_data;

    uint8_t alpha =
        (uint8_t)(255.0f * opacity);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t *pixel =
                pixels +
                (size_t)y * source->frame_linesize +
                x * 4;

            if (pixel[0] != 0 ||
                pixel[1] != 0 ||
                pixel[2] != 0) {
                pixel[3] = alpha;
            }
        }
    }

    /* Nettoyage GDI. */
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

/*
 * ------------------------------------------------------------
 * ------------------------------------------------------------
 */

static void trophy_stream_output_frame(
    struct trophy_stream_source *source)
{
    if (!source ||
        !source->frame_data) {

        return;
    }

    struct obs_source_frame frame;

    memset(
        &frame,
        0,
        sizeof(frame)
    );

    frame.data[0] =
        source->frame_data;

    frame.linesize[0] =
        source->frame_linesize;

    frame.width =
        source->width;

    frame.height =
        source->height;

    frame.format =
        VIDEO_FORMAT_BGRA;

    frame.full_range =
        true;

    frame.timestamp =
        source->frame_timestamp;

    obs_source_output_video(
        source->source,
        &frame
    );
}

/*
 * ------------------------------------------------------------
 * Tick vidéo OBS
 * ------------------------------------------------------------
 */

static void trophy_stream_video_tick(
    void *data,
    float seconds)
{
    struct trophy_stream_source *source =
        data;

    UNUSED_PARAMETER(seconds);

    if (!source)
        return;

    trophy_stream_generate_frame(
        source
    );

    source->frame_timestamp =
        obs_get_video_frame_time();

    trophy_stream_output_frame(
        source
    );
}

/*
 * ------------------------------------------------------------
 * Création
 * ------------------------------------------------------------
 */

static void *trophy_stream_create(
    obs_data_t *settings,
    obs_source_t *source)
{
    UNUSED_PARAMETER(settings);

    struct trophy_stream_source *data =
        bzalloc(
            sizeof(
                struct trophy_stream_source
            )
        );

    if (!data)
        return NULL;

    data->source =
        source;

    data->width =
        TROPHY_WIDTH;

    data->height =
        TROPHY_HEIGHT;

    data->frame_linesize =
        data->width * 4;

    size_t frame_size =
        (size_t)data->frame_linesize *
        data->height;

    data->frame_data =
        bzalloc(frame_size);

    if (!data->frame_data) {

        bfree(data);

        return NULL;
    }

    trophy_stream_generate_frame(
        data
    );

    blog(
        LOG_INFO,
        "[TrophyStream] Source créée"
    );

    return data;
}

/*
 * ------------------------------------------------------------
 * Destruction
 * ------------------------------------------------------------
 */

static void trophy_stream_destroy(
    void *data)
{
    struct trophy_stream_source *source =
        data;

    if (!source)
        return;

    if (source->frame_data) {

        bfree(
            source->frame_data
        );

        source->frame_data = NULL;
    }

    blog(
        LOG_INFO,
        "[TrophyStream] Source détruite"
    );

    bfree(source);
}

/*
 * ------------------------------------------------------------
 * Taille
 * ------------------------------------------------------------
 */

static uint32_t trophy_stream_get_width(
    void *data)
{
    struct trophy_stream_source *source =
        data;

    if (!source)
        return 0;

    return source->width;
}

static uint32_t trophy_stream_get_height(
    void *data)
{
    struct trophy_stream_source *source =
        data;

    if (!source)
        return 0;

    return source->height;
}

/*
 * ------------------------------------------------------------
 * Informations OBS
 * ------------------------------------------------------------
 */

static struct obs_source_info trophy_stream_source_info = {

    .id =
        "trophy_stream_source",

    .type =
        OBS_SOURCE_TYPE_INPUT,

    .output_flags =
        OBS_SOURCE_ASYNC_VIDEO,

    .get_name =
        trophy_stream_get_name,

    .create =
        trophy_stream_create,

    .destroy =
        trophy_stream_destroy,

    .get_width =
        trophy_stream_get_width,

    .get_height =
        trophy_stream_get_height,

    .video_tick =
        trophy_stream_video_tick,
};

/*
 * ============================================================
 * Chargement
 * ============================================================
 */

bool obs_module_load(void)
{
    blog(
        LOG_INFO,
        "[TrophyStream] Plugin charge"
    );

    /*
     * Initialisation du verrou.
     */
    InitializeCriticalSection(
        &progress_lock
    );

    InitializeCriticalSection(
        &icon_lock
    );

    /*
     * Winsock.
     */
    WSADATA wsa_data;

    int result =
        WSAStartup(
            MAKEWORD(2, 2),
            &wsa_data
        );

    if (result != 0) {

        blog(
            LOG_ERROR,
            "[TrophyStream] Échec de WSAStartup : %d",
            result
        );

        DeleteCriticalSection(
            &icon_lock
        );

        DeleteCriticalSection(
            &progress_lock
        );

        return false;
    }

    /*
     * Événement d’arrêt.
     */
    websocket_stop_event =
        CreateEventA(
            NULL,
            TRUE,
            FALSE,
            NULL
        );

    if (!websocket_stop_event) {

        WSACleanup();

        DeleteCriticalSection(
            &icon_lock
        );

        DeleteCriticalSection(
            &progress_lock
        );

        return false;
    }

    InterlockedExchange(
        &plugin_running,
        1
    );

    websocket_thread =
        CreateThread(
            NULL,
            0,
            trophy_stream_websocket_thread,
            NULL,
            0,
            NULL
        );

    if (!websocket_thread) {

        InterlockedExchange(
            &plugin_running,
            0
        );

        CloseHandle(
            websocket_stop_event
        );

        websocket_stop_event =
            NULL;

        WSACleanup();

        DeleteCriticalSection(
            &progress_lock
        );

        return false;
    }

    /*
     * Enregistrement OBS.
     */
    obs_register_source(
        &trophy_stream_source_info
    );

    blog(
        LOG_INFO,
        "[TrophyStream] Source OBS enregistrée"
    );

    return true;
}

/*
 * ============================================================
 * Déchargement
 * ============================================================
 */

void obs_module_unload(void)
{
    blog(
        LOG_INFO,
        "[TrophyStream] Arrêt du plugin"
    );

    InterlockedExchange(
        &plugin_running,
        0
    );

    if (websocket_stop_event) {

        SetEvent(
            websocket_stop_event
        );
    }

    /*
     * The network worker checks the stop event during every receive
     * wait. Wait until it has actually exited before destroying shared
     * critical sections; timing out here would permit a use-after-delete.
     */
    if (websocket_thread) {
        WaitForSingleObject(websocket_thread, INFINITE);
        CloseHandle(websocket_thread);
        websocket_thread = NULL;
    }

    if (websocket_stop_event) {

        CloseHandle(
            websocket_stop_event
        );

        websocket_stop_event =
            NULL;
    }

    WSACleanup();

    DeleteCriticalSection(
        &icon_lock
    );

    DeleteCriticalSection(
        &progress_lock
    );

    blog(
        LOG_INFO,
        "[TrophyStream] Plugin déchargé"
    );
}

