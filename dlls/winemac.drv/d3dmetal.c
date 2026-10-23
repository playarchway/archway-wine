/*
 * Compatibility table for Apple's D3DMetal (Game Porting Toolkit)
 *
 * D3DMetal looks up an exported "macdrv_functions" table in winemac.so at
 * run time. The table and the structure layouts below follow the interface
 * that D3DMetal expects from CrossOver's Mac driver; this file provides it on
 * top of the upstream driver.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "macdrv.h"
#include "winreg.h"

/* Declared with a block parameter in cocoa_event.h; D3DMetal passes a block. */
extern void OnMainThread(void *block);

WINE_DEFAULT_DEBUG_CHANNEL(d3dmetal);

/* The window data layout D3DMetal reads (Wine 8 era macdrv_win_data). */
struct d3dmetal_win_data
{
    HWND                    hwnd;
    WineWindow             *cocoa_window;
    void                   *cocoa_view;
    WineContentView        *client_cocoa_view;
    RECT                    window_rect;
    RECT                    whole_rect;
    RECT                    client_rect;
    int                     pixel_format;
    void                   *gl_view;
    unsigned int            on_screen : 1;
    unsigned int            shaped : 1;
    unsigned int            layered : 1;
    unsigned int            ulw_layered : 1;
    unsigned int            per_pixel_alpha : 1;
    unsigned int            minimized : 1;
    struct macdrv_win_data *data;           /* the driver's data, held until release */
};
C_ASSERT(offsetof(struct d3dmetal_win_data, client_cocoa_view) == 0x18);
C_ASSERT(offsetof(struct d3dmetal_win_data, window_rect) == 0x20);
C_ASSERT(offsetof(struct d3dmetal_win_data, pixel_format) == 0x50);
C_ASSERT(offsetof(struct d3dmetal_win_data, data) == 0x68);
C_ASSERT(sizeof(struct d3dmetal_win_data) == 0x70);

/* D3DMetal renders into a client surface view of its own; keep one per window
 * for the life of the process so the view follows the window's geometry. */
static pthread_mutex_t surfaces_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { HWND hwnd; struct client_surface *client; } surfaces[64];

static struct macdrv_client_surface *d3dmetal_client_surface(HWND hwnd)
{
    struct macdrv_client_surface *surface = NULL;
    struct client_surface *client;
    unsigned int i, free_slot = ARRAY_SIZE(surfaces);

    pthread_mutex_lock(&surfaces_mutex);
    for (i = 0; i < ARRAY_SIZE(surfaces); i++)
    {
        if (surfaces[i].hwnd == hwnd)
        {
            surface = impl_from_client_surface(surfaces[i].client);
            break;
        }
        if (surfaces[i].client && !NtUserIsWindow(surfaces[i].hwnd))
        {
            client_surface_release(surfaces[i].client);
            surfaces[i].hwnd = 0;
            surfaces[i].client = NULL;
        }
        if (!surfaces[i].client && free_slot == ARRAY_SIZE(surfaces)) free_slot = i;
    }
    if (!surface && free_slot < ARRAY_SIZE(surfaces) && (client = macdrv_CreateClientSurface(hwnd, 0, FALSE)))
    {
        surfaces[free_slot].hwnd = hwnd;
        surfaces[free_slot].client = client;
        surface = impl_from_client_surface(client);
    }
    pthread_mutex_unlock(&surfaces_mutex);
    return surface;
}

void d3dmetal_update_client_view(HWND hwnd, const struct window_rects *rects)
{
    RECT frame = rects->client;
    unsigned int i;

    OffsetRect(&frame, -rects->visible.left, -rects->visible.top);

    pthread_mutex_lock(&surfaces_mutex);
    for (i = 0; i < ARRAY_SIZE(surfaces); i++)
    {
        if (surfaces[i].hwnd != hwnd || !surfaces[i].client) continue;

        TRACE("hwnd %p window %s visible %s client %s view %s\n", hwnd,
              wine_dbgstr_rect(&rects->window), wine_dbgstr_rect(&rects->visible),
              wine_dbgstr_rect(&rects->client), wine_dbgstr_rect(&frame));
        macdrv_set_view_frame(impl_from_client_surface(surfaces[i].client)->cocoa_view, cgrect_from_rect(frame));
        break;
    }
    pthread_mutex_unlock(&surfaces_mutex);
}

static void d3dmetal_init_display_devices(BOOL force)
{
    TRACE("force %u\n", force);
}

static struct d3dmetal_win_data *d3dmetal_get_win_data(HWND hwnd)
{
    struct macdrv_client_surface *surface;
    struct d3dmetal_win_data *compat;
    struct macdrv_win_data *data;

    TRACE("hwnd %p\n", hwnd);

    if (!(surface = d3dmetal_client_surface(hwnd))) return NULL;
    if (!(data = get_win_data(hwnd))) return NULL;
    if (!(compat = calloc(1, sizeof(*compat))))
    {
        release_win_data(data);
        return NULL;
    }
    compat->hwnd = data->hwnd;
    compat->cocoa_window = data->cocoa_window;
    compat->client_cocoa_view = surface->cocoa_view;
    compat->window_rect = data->rects.window;
    compat->whole_rect = data->rects.visible;
    compat->client_rect = data->rects.client;
    compat->on_screen = data->on_screen;
    compat->shaped = data->shaped;
    compat->layered = data->layered;
    compat->ulw_layered = data->ulw_layered;
    compat->per_pixel_alpha = data->per_pixel_alpha;
    compat->minimized = data->minimized;
    compat->data = data;
    return compat;
}

static void d3dmetal_release_win_data(struct d3dmetal_win_data *compat)
{
    TRACE("%p\n", compat);

    if (!compat) return;
    release_win_data(compat->data);
    free(compat);
}

static WineWindow *d3dmetal_get_cocoa_window(HWND hwnd, BOOL require_on_screen)
{
    return macdrv_get_cocoa_window(hwnd, require_on_screen);
}

/* Registry access for D3DMetal's settings, with Win32 A-function semantics. */

static HANDLE open_root_key(UINT hkey)
{
    static const WCHAR machineW[] = {'\\','R','e','g','i','s','t','r','y','\\','M','a','c','h','i','n','e',0};
    char token_buffer[256];
    TOKEN_USER *user = (TOKEN_USER *)token_buffer;
    WCHAR path[128];
    char sid[64];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE root;
    DWORD len, i;

    switch (hkey)
    {
    case (UINT)(UINT_PTR)HKEY_LOCAL_MACHINE:
        memcpy(path, machineW, sizeof(machineW));
        break;
    case (UINT)(UINT_PTR)HKEY_CURRENT_USER:
    {
        SID *s;
        int n;

        if (NtQueryInformationToken(GetCurrentThreadEffectiveToken(), TokenUser, user,
                                    sizeof(token_buffer), &len)) return 0;
        s = user->User.Sid;
        n = snprintf(sid, sizeof(sid), "S-%u-%u", s->Revision, s->IdentifierAuthority.Value[5]);
        for (i = 0; i < s->SubAuthorityCount; i++)
            n += snprintf(sid + n, sizeof(sid) - n, "-%u", (unsigned int)s->SubAuthority[i]);
        n = snprintf((char *)token_buffer, sizeof(token_buffer), "\\Registry\\User\\%s", sid);
        for (i = 0; i <= n; i++) path[i] = (unsigned char)token_buffer[i];
        break;
    }
    default:
        return UlongToHandle(hkey);
    }
    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, 0, NULL);
    if (NtOpenKeyEx(&root, MAXIMUM_ALLOWED, &attr, 0)) return 0;
    return root;
}

static void close_root_key(UINT hkey, HANDLE root)
{
    if (root && root != UlongToHandle(hkey)) NtClose(root);
}

static WCHAR *d3dmetal_ascii_to_unicode(const char *str)
{
    WCHAR *ret;
    size_t i, len = str ? strlen(str) : 0;

    if (!(ret = malloc((len + 1) * sizeof(WCHAR)))) return NULL;
    for (i = 0; i <= len; i++) ret[i] = str ? (unsigned char)str[i] : 0;
    return ret;
}

static LONG d3dmetal_reg_create_open_key(UINT hkey, const char *subkey, BOOL create, REGSAM sam,
                                         HKEY *result, DWORD *disposition)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    HANDLE root, key;
    NTSTATUS status;
    WCHAR *subkeyW;
    ULONG dispos = 0;

    if (!result) return ERROR_INVALID_PARAMETER;
    if (!(root = open_root_key(hkey))) return ERROR_INVALID_HANDLE;
    if (!(subkeyW = d3dmetal_ascii_to_unicode(subkey)))
    {
        close_root_key(hkey, root);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    RtlInitUnicodeString(&name, subkeyW);
    InitializeObjectAttributes(&attr, &name, OBJ_CASE_INSENSITIVE, root, NULL);
    if (create) status = NtCreateKey(&key, sam & ~KEY_WOW64_RES, &attr, 0, NULL, 0, &dispos);
    else status = NtOpenKeyEx(&key, sam & ~KEY_WOW64_RES, &attr, 0);
    free(subkeyW);
    close_root_key(hkey, root);
    if (status) return RtlNtStatusToDosError(status);
    *result = key;
    if (disposition) *disposition = dispos;
    return ERROR_SUCCESS;
}

static LONG WINAPI d3dmetal_RegOpenKeyExA(UINT hkey, const char *subkey, DWORD options, REGSAM sam, HKEY *result)
{
    TRACE("%#x %s %#x %#x %p\n", hkey, debugstr_a(subkey), options, sam, result);
    return d3dmetal_reg_create_open_key(hkey, subkey, FALSE, sam, result, NULL);
}

static LONG WINAPI d3dmetal_RegCreateKeyExA(UINT hkey, const char *subkey, DWORD reserved, char *class,
                                     DWORD options, REGSAM sam, SECURITY_ATTRIBUTES *sa,
                                     HKEY *result, DWORD *disposition)
{
    TRACE("%#x %s %#x %#x %p\n", hkey, debugstr_a(subkey), options, sam, result);
    return d3dmetal_reg_create_open_key(hkey, subkey, TRUE, sam, result, disposition);
}

static LONG WINAPI d3dmetal_RegQueryValueExA(UINT hkey, const char *value, DWORD *reserved, DWORD *type,
                                      BYTE *data, DWORD *count)
{
    KEY_VALUE_PARTIAL_INFORMATION *info;
    UNICODE_STRING name;
    WCHAR *valueW;
    NTSTATUS status;
    DWORD size = (count ? *count : 0) + offsetof(KEY_VALUE_PARTIAL_INFORMATION, Data) + 256;
    ULONG len;
    LONG ret = ERROR_SUCCESS;

    TRACE("%#x %s %p %p %p %p\n", hkey, debugstr_a(value), reserved, type, data, count);

    if (data && !count) return ERROR_INVALID_PARAMETER;
    if (!(valueW = d3dmetal_ascii_to_unicode(value))) return ERROR_NOT_ENOUGH_MEMORY;
    RtlInitUnicodeString(&name, valueW);
    for (;;)
    {
        if (!(info = malloc(size)))
        {
            free(valueW);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        status = NtQueryValueKey(UlongToHandle(hkey), &name, KeyValuePartialInformation, info, size, &len);
        if (status != STATUS_BUFFER_OVERFLOW && status != STATUS_BUFFER_TOO_SMALL) break;
        free(info);
        size = len;
    }
    free(valueW);
    if (status)
    {
        free(info);
        return RtlNtStatusToDosError(status);
    }
    /* String values are returned as stored; D3DMetal's settings are numeric. */
    if (type) *type = info->Type;
    if (data && *count < info->DataLength) ret = ERROR_MORE_DATA;
    else if (data) memcpy(data, info->Data, info->DataLength);
    if (count) *count = info->DataLength;
    free(info);
    return ret;
}

static LONG WINAPI d3dmetal_RegSetValueExA(UINT hkey, const char *value, DWORD reserved, DWORD type,
                                    const BYTE *data, DWORD count)
{
    UNICODE_STRING name;
    WCHAR *valueW;
    NTSTATUS status;

    TRACE("%#x %s %u %p %u\n", hkey, debugstr_a(value), type, data, count);

    if (!(valueW = d3dmetal_ascii_to_unicode(value))) return ERROR_NOT_ENOUGH_MEMORY;
    RtlInitUnicodeString(&name, valueW);
    status = NtSetValueKey(UlongToHandle(hkey), &name, 0, type, data, count);
    free(valueW);
    return RtlNtStatusToDosError(status);
}

static LONG WINAPI d3dmetal_RegCloseKey(UINT hkey)
{
    TRACE("%#x\n", hkey);
    return RtlNtStatusToDosError(NtClose(UlongToHandle(hkey)));
}

/* Window and monitor helpers, with Win32 semantics. */

static BOOL WINAPI d3dmetal_EnumDisplayMonitors(HDC hdc, RECT *rect, MONITORENUMPROC proc, LPARAM lparam)
{
    return NtUserEnumDisplayMonitors(hdc, rect, proc, lparam);
}

static BOOL WINAPI d3dmetal_GetMonitorInfoA(HMONITOR monitor, MONITORINFO *info)
{
    MONITORINFOEXW infoW;
    MONITORINFOEXA *infoA = (MONITORINFOEXA *)info;
    DWORD len;

    if (info->cbSize == sizeof(MONITORINFO)) return NtUserGetMonitorInfo(monitor, info);
    if (info->cbSize != sizeof(MONITORINFOEXA)) return FALSE;

    infoW.cbSize = sizeof(infoW);
    if (!NtUserGetMonitorInfo(monitor, (MONITORINFO *)&infoW)) return FALSE;
    infoA->rcMonitor = infoW.rcMonitor;
    infoA->rcWork = infoW.rcWork;
    infoA->dwFlags = infoW.dwFlags;
    RtlUnicodeToUTF8N(infoA->szDevice, sizeof(infoA->szDevice), &len, infoW.szDevice,
                      (wcslen(infoW.szDevice) + 1) * sizeof(WCHAR));
    return TRUE;
}

static BOOL WINAPI d3dmetal_AdjustWindowRectEx(RECT *rect, DWORD style, BOOL menu, DWORD ex_style)
{
    return NtUserAdjustWindowRect(rect, style, menu, ex_style, NtUserGetSystemDpiForProcess(NULL));
}

static LONG_PTR WINAPI d3dmetal_GetWindowLongPtrW(HWND hwnd, INT offset)
{
    return NtUserGetWindowLongPtrW(hwnd, offset);
}

static BOOL WINAPI d3dmetal_GetWindowRect(HWND hwnd, RECT *rect)
{
    return NtUserGetWindowRect(hwnd, rect, NtUserGetDpiForWindow(hwnd));
}

static BOOL WINAPI d3dmetal_MoveWindow(HWND hwnd, INT x, INT y, INT cx, INT cy, BOOL repaint)
{
    return NtUserMoveWindow(hwnd, x, y, cx, cy, repaint);
}

static BOOL WINAPI d3dmetal_SetWindowPos(HWND hwnd, HWND after, INT x, INT y, INT cx, INT cy, UINT flags)
{
    return NtUserSetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

static INT WINAPI d3dmetal_GetSystemMetrics(INT index)
{
    return NtUserGetSystemMetrics(index);
}

static LONG_PTR WINAPI d3dmetal_SetWindowLongPtrW(HWND hwnd, INT offset, LONG_PTR value)
{
    return NtUserSetWindowLongPtr(hwnd, offset, value, FALSE);
}

struct d3dmetal_macdrv_functions
{
    void (*macdrv_init_display_devices)(BOOL);
    struct d3dmetal_win_data *(*get_win_data)(HWND);
    void (*release_win_data)(struct d3dmetal_win_data *);
    WineWindow *(*macdrv_get_cocoa_window)(HWND, BOOL);
    MTLDevice_id (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(MTLDevice_id);
    WineMetalView *(*macdrv_view_create_metal_view)(WineContentView *, MTLDevice_id);
    CAMetalLayer *(*macdrv_view_get_metal_layer)(WineMetalView *);
    void (*macdrv_view_release_metal_view)(WineMetalView *);
    void (*on_main_thread)(void *);
    LONG (WINAPI *RegQueryValueExA)(UINT, const char *, DWORD *, DWORD *, BYTE *, DWORD *);
    LONG (WINAPI *RegSetValueExA)(UINT, const char *, DWORD, DWORD, const BYTE *, DWORD);
    LONG (WINAPI *RegOpenKeyExA)(UINT, const char *, DWORD, REGSAM, HKEY *);
    LONG (WINAPI *RegCreateKeyExA)(UINT, const char *, DWORD, char *, DWORD, REGSAM, SECURITY_ATTRIBUTES *, HKEY *, DWORD *);
    LONG (WINAPI *RegCloseKey)(UINT);
    BOOL (WINAPI *EnumDisplayMonitors)(HDC, RECT *, MONITORENUMPROC, LPARAM);
    BOOL (WINAPI *GetMonitorInfoA)(HMONITOR, MONITORINFO *);
    BOOL (WINAPI *AdjustWindowRectEx)(RECT *, DWORD, BOOL, DWORD);
    LONG_PTR (WINAPI *GetWindowLongPtrW)(HWND, INT);
    BOOL (WINAPI *GetWindowRect)(HWND, RECT *);
    BOOL (WINAPI *MoveWindow)(HWND, INT, INT, INT, INT, BOOL);
    BOOL (WINAPI *SetWindowPos)(HWND, HWND, INT, INT, INT, INT, UINT);
    INT (WINAPI *GetSystemMetrics)(INT);
    LONG_PTR (WINAPI *SetWindowLongPtrW)(HWND, INT, LONG_PTR);
};
C_ASSERT(sizeof(struct d3dmetal_macdrv_functions) == 24 * sizeof(void *));

DECLSPEC_EXPORT const struct d3dmetal_macdrv_functions macdrv_functions =
{
    d3dmetal_init_display_devices,
    d3dmetal_get_win_data,
    d3dmetal_release_win_data,
    d3dmetal_get_cocoa_window,
    macdrv_create_metal_device,
    macdrv_release_metal_device,
    macdrv_view_create_metal_view,
    macdrv_view_get_metal_layer,
    macdrv_view_release_metal_view,
    OnMainThread,
    d3dmetal_RegQueryValueExA,
    d3dmetal_RegSetValueExA,
    d3dmetal_RegOpenKeyExA,
    d3dmetal_RegCreateKeyExA,
    d3dmetal_RegCloseKey,
    d3dmetal_EnumDisplayMonitors,
    d3dmetal_GetMonitorInfoA,
    d3dmetal_AdjustWindowRectEx,
    d3dmetal_GetWindowLongPtrW,
    d3dmetal_GetWindowRect,
    d3dmetal_MoveWindow,
    d3dmetal_SetWindowPos,
    d3dmetal_GetSystemMetrics,
    d3dmetal_SetWindowLongPtrW,
};
