/*
 * Unit tests for DCE support
 *
 * Copyright 2005 Alexandre Julliard
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

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"

#include "wine/test.h"

#ifndef DCX_USESTYLE
#define DCX_USESTYLE         0x00010000
#endif

static HWND hwnd_cache, hwnd_owndc, hwnd_classdc, hwnd_classdc2, hwnd_parent, hwnd_parentdc;

static LRESULT CALLBACK detached_gdi_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    PAINTSTRUCT ps;

    if (msg == WM_PAINT)
    {
        BeginPaint( hwnd, &ps );
        EndPaint( hwnd, &ps );
        return 0;
    }
    return DefWindowProcA( hwnd, msg, wparam, lparam );
}

static BOOL paint_gdi_bands_dc( HDC hdc )
{
    RECT left = { 0, 0, 32, 64 }, right = { 32, 0, 64, 64 };
    HBRUSH red, blue;
    BOOL ret = FALSE;

    red = CreateSolidBrush( RGB(255, 0, 0) );
    blue = CreateSolidBrush( RGB(0, 0, 255) );
    ok( !!red && !!blue, "CreateSolidBrush failed\n" );
    if (red && blue)
    {
        ret = FillRect( hdc, &left, red ) && FillRect( hdc, &right, blue );
        ok( ret, "FillRect failed\n" );
        ok( GdiFlush(), "GdiFlush failed\n" );
    }
    if (red) DeleteObject( red );
    if (blue) DeleteObject( blue );
    return ret;
}

static BOOL paint_gdi_bands( HWND hwnd )
{
    HDC hdc = GetDC( hwnd );
    BOOL ret;

    ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
    if (!hdc) return FALSE;
    ret = paint_gdi_bands_dc( hdc );
    ok( ReleaseDC( hwnd, hdc ), "ReleaseDC failed\n" );
    return ret;
}

static BOOL check_gdi_bands( HWND hwnd )
{
    COLORREF left, right;
    HDC hdc = GetDC( hwnd );

    ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
    if (!hdc) return FALSE;
    left = GetPixel( hdc, 16, 32 );
    right = GetPixel( hdc, 48, 32 );
    ok( left == RGB(255, 0, 0), "left pixel is %#lx\n", left );
    ok( right == RGB(0, 0, 255), "right pixel is %#lx\n", right );
    ok( ReleaseDC( hwnd, hdc ), "ReleaseDC failed\n" );
    return left == RGB(255, 0, 0) && right == RGB(0, 0, 255);
}

static BOOL check_gdi_pixel( HDC hdc, int x, int y, COLORREF expected )
{
    COLORREF color = GetPixel( hdc, x, y );

    ok( color == expected, "pixel at %d,%d is %#lx, expected %#lx\n", x, y, color, expected );
    return color == expected;
}

static DWORD wait_for_gdi_objects( DWORD count, const HANDLE *objects )
{
    DWORD start = GetTickCount(), ret, elapsed;
    MSG msg;

    for (;;)
    {
        elapsed = GetTickCount() - start;
        if (elapsed >= 5000) return WAIT_TIMEOUT;
        ret = MsgWaitForMultipleObjects( count, objects, FALSE, 5000 - elapsed, QS_ALLINPUT );
        if (ret != WAIT_OBJECT_0 + count) return ret;
        while (PeekMessageA( &msg, 0, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageA( &msg );
        }
    }
}

static void test_detached_gdi_child( HWND parent, BOOL cross_process )
{
    WNDCLASSA cls = {0};
    LONG_PTR style;
    DWORD parent_pid = 0, child_pid = 0;
    HWND child, previous, actual_parent;
    POINT point = {0};
    RECT rect;

    cls.lpfnWndProc = detached_gdi_wndproc;
    cls.hInstance = GetModuleHandleA( NULL );
    cls.lpszClassName = "detached_gdi_child_class";
    ok( RegisterClassA( &cls ), "RegisterClassA failed, error %lu\n", GetLastError() );
    child = CreateWindowExA( 0, cls.lpszClassName, NULL, WS_POPUP,
                             0, 0, 64, 64, NULL, NULL, cls.hInstance, NULL );
    ok( !!child, "CreateWindowExA failed, error %lu\n", GetLastError() );
    if (!child) goto cleanup;

    GetWindowThreadProcessId( parent, &parent_pid );
    GetWindowThreadProcessId( child, &child_pid );
    ok( parent_pid && (parent_pid != GetCurrentProcessId()) == cross_process,
        "parent belongs to process %lu\n", parent_pid );
    ok( child_pid == GetCurrentProcessId(), "child belongs to process %lu\n", child_pid );
    ok( !GetParent( child ), "child unexpectedly has a parent\n" );

    style = GetWindowLongPtrA( child, GWL_STYLE );
    ok( style & WS_POPUP, "initial style is %#Ix\n", style );
    ok( SetWindowLongPtrA( child, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD ) == style,
        "SetWindowLongPtrA failed, error %lu\n", GetLastError() );
    SetLastError( 0 );
    previous = SetParent( child, parent );
    ok( previous == GetDesktopWindow() || (!previous && !GetLastError()),
        "SetParent returned %p, error %lu\n", previous, GetLastError() );
    actual_parent = GetParent( child );
    ok( actual_parent == parent, "GetParent returned %p, expected %p\n", actual_parent, parent );
    if (actual_parent != parent) goto destroy;
    ok( GetAncestor( child, GA_ROOT ) == parent, "unexpected root window\n" );
    style = GetWindowLongPtrA( child, GWL_STYLE );
    ok( (style & (WS_CHILD | WS_POPUP)) == WS_CHILD, "child style is %#Ix\n", style );

    ok( SetWindowPos( child, NULL, 8, 12, 64, 64,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED ),
        "SetWindowPos failed, error %lu\n", GetLastError() );
    MapWindowPoints( child, parent, &point, 1 );
    ok( point.x == 8 && point.y == 12, "child offset is %ld,%ld\n", point.x, point.y );
    ok( IsWindowVisible( child ), "child is not visible\n" );
    UpdateWindow( child );
    if (paint_gdi_bands( child )) check_gdi_bands( child );

    ok( SetWindowPos( child, NULL, 12, 16, 68, 68, SWP_NOZORDER | SWP_NOACTIVATE ),
        "moving or resizing child failed, error %lu\n", GetLastError() );
    point.x = point.y = 0;
    MapWindowPoints( child, parent, &point, 1 );
    ok( point.x == 12 && point.y == 16, "moved child offset is %ld,%ld\n", point.x, point.y );
    GetClientRect( child, &rect );
    ok( rect.right == 68 && rect.bottom == 68, "resized client is %ldx%ld\n", rect.right, rect.bottom );
    ShowWindow( child, SW_HIDE );
    ok( !IsWindowVisible( child ), "child is still visible\n" );
    ShowWindow( child, SW_SHOWNA );
    ok( IsWindowVisible( child ), "child is still hidden\n" );
    UpdateWindow( child );
    if (paint_gdi_bands( child )) check_gdi_bands( child );

destroy:
    DestroyWindow( child );
cleanup:
    UnregisterClassA( cls.lpszClassName, cls.hInstance );
}

static void test_detached_child_gdi( char **argv )
{
    WNDCLASSA cls = {0};
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION info;
    char cmdline[MAX_PATH * 2];
    DWORD ret, exit_code;
    int len;
    HWND hwnd;

    cls.lpfnWndProc = detached_gdi_wndproc;
    cls.hInstance = GetModuleHandleA( NULL );
    cls.lpszClassName = "detached_gdi_parent_class";
    ok( RegisterClassA( &cls ), "RegisterClassA failed, error %lu\n", GetLastError() );
    hwnd = CreateWindowExA( WS_EX_TOPMOST, cls.lpszClassName, NULL, WS_POPUP | WS_VISIBLE,
                            GetSystemMetrics( SM_CXSCREEN ) / 2 - 48,
                            GetSystemMetrics( SM_CYSCREEN ) / 2 - 48,
                            96, 96, NULL, NULL, cls.hInstance, NULL );
    ok( !!hwnd, "CreateWindowExA failed, error %lu\n", GetLastError() );
    if (!hwnd)
    {
        UnregisterClassA( cls.lpszClassName, cls.hInstance );
        return;
    }
    UpdateWindow( hwnd );
    test_detached_gdi_child( hwnd, FALSE );

    len = snprintf( cmdline, sizeof(cmdline), "\"%s\" %s detached_child_gdi %p", argv[0], argv[1], hwnd );
    ok( len >= 0 && (size_t)len < sizeof(cmdline), "child command line is too long\n" );
    if (len < 0 || (size_t)len >= sizeof(cmdline)) goto cleanup;
    startup.cb = sizeof(startup);
    if (!CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info ))
    {
        ok( 0, "CreateProcessA failed, error %lu\n", GetLastError() );
        goto cleanup;
    }

    ret = wait_for_gdi_objects( 1, &info.hProcess );
    ok( ret == WAIT_OBJECT_0, "child wait returned %#lx\n", ret );
    if (ret == WAIT_OBJECT_0)
    {
        if (GetExitCodeProcess( info.hProcess, &exit_code ))
            ok( exit_code == 0, "child exited with status %#lx\n", exit_code );
        else
            ok( 0, "GetExitCodeProcess failed, error %lu\n", GetLastError() );
    }
    else
    {
        ok( TerminateProcess( info.hProcess, 1 ) || WaitForSingleObject( info.hProcess, 0 ) == WAIT_OBJECT_0,
            "could not terminate child\n" );
        ret = WaitForSingleObject( info.hProcess, 5000 );
        ok( ret == WAIT_OBJECT_0, "child cleanup wait returned %#lx\n", ret );
    }
    CloseHandle( info.hThread );
    CloseHandle( info.hProcess );

cleanup:
    DestroyWindow( hwnd );
    UnregisterClassA( cls.lpszClassName, cls.hInstance );
}

static void direct_gdi_event_names( HWND hwnd, DWORD pid, char *proceed, char *done )
{
    snprintf( proceed, 80, "wine_dce_direct_%lu_%p_proceed", pid, hwnd );
    snprintf( done, 80, "wine_dce_direct_%lu_%p_done", pid, hwnd );
}

static void direct_foreign_gdi_proc( HWND hwnd )
{
    RECT new_area = { 160, 64, 224, 96 };
    char proceed_name[80], done_name[80];
    HANDLE proceed = NULL, done = NULL;
    DWORD owner_pid = 0, ret;
    HBRUSH green;
    HDC hdc;

    GetWindowThreadProcessId( hwnd, &owner_pid );
    ok( owner_pid && owner_pid != GetCurrentProcessId(), "window belongs to process %lu\n", owner_pid );
    if (!owner_pid || owner_pid == GetCurrentProcessId()) return;
    direct_gdi_event_names( hwnd, owner_pid, proceed_name, done_name );
    proceed = OpenEventA( EVENT_ALL_ACCESS, FALSE, proceed_name );
    done = OpenEventA( EVENT_ALL_ACCESS, FALSE, done_name );
    ok( !!proceed && !!done, "OpenEventA failed, error %lu\n", GetLastError() );
    if (!proceed || !done) goto cleanup;

    winetest_push_context( "foreign DC acquired while owner hidden" );
    ok( !IsWindowVisible( hwnd ), "owner is visible before GetDC\n" );
    hdc = GetDC( hwnd );
    ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
    winetest_pop_context();
    if (!hdc) goto cleanup;
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );
    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "parent show wait returned %#lx\n", ret );
    if (ret != WAIT_OBJECT_0) goto release;
    winetest_push_context( "foreign initial drawing after show" );
    paint_gdi_bands_dc( hdc );
    winetest_pop_context();
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );

    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "parent white write wait returned %#lx\n", ret );
    if (ret != WAIT_OBJECT_0) goto release;
    winetest_push_context( "foreign destination ROP" );
    check_gdi_pixel( hdc, 16, 32, RGB(255, 255, 255) );
    check_gdi_pixel( hdc, 48, 32, RGB(255, 255, 255) );
    ok( PatBlt( hdc, 0, 0, 64, 64, DSTINVERT ), "PatBlt DSTINVERT failed\n" );
    ok( GdiFlush(), "GdiFlush failed\n" );
    check_gdi_pixel( hdc, 16, 32, RGB(0, 0, 0) );
    check_gdi_pixel( hdc, 48, 32, RGB(0, 0, 0) );
    winetest_pop_context();
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );

    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "parent small resize wait returned %#lx\n", ret );
    if (ret != WAIT_OBJECT_0) goto release;
    winetest_push_context( "foreign retained DC after small resize" );
    ok( SetPixel( hdc, 100, 80, RGB(0, 255, 0) ) == RGB(0, 255, 0),
        "SetPixel in newly exposed client area failed\n" );
    ok( GdiFlush(), "GdiFlush failed\n" );
    check_gdi_pixel( hdc, 100, 80, RGB(0, 255, 0) );
    winetest_pop_context();
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );

    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "parent large resize wait returned %#lx\n", ret );
    if (ret != WAIT_OBJECT_0) goto release;
    winetest_push_context( "foreign retained DC after resize" );
    green = CreateSolidBrush( RGB(0, 255, 0) );
    ok( !!green, "CreateSolidBrush failed\n" );
    if (green)
    {
        ok( FillRect( hdc, &new_area, green ), "FillRect in new area failed\n" );
        ok( GdiFlush(), "GdiFlush failed\n" );
        check_gdi_pixel( hdc, 180, 80, RGB(0, 255, 0) );
        DeleteObject( green );
    }
    winetest_pop_context();
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );

    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "parent destruction wait returned %#lx\n", ret );
    if (ret != WAIT_OBJECT_0) goto release;
    winetest_push_context( "foreign DC after owner destruction" );
    GetPixel( hdc, 16, 32 );
    GdiFlush();
    ReleaseDC( hwnd, hdc );
    winetest_pop_context();
    goto cleanup;

release:
    ReleaseDC( hwnd, hdc );
cleanup:
    if (done) CloseHandle( done );
    if (proceed) CloseHandle( proceed );
}

static void test_direct_foreign_gdi( char **argv )
{
    WNDCLASSA cls = {0};
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION info = {0};
    char proceed_name[80], done_name[80], cmdline[MAX_PATH * 2];
    HANDLE proceed = NULL, done = NULL, objects[2];
    DWORD ret, exit_code;
    BOOL finished = FALSE, terminated = FALSE, cleared = FALSE;
    BOOL signaled, destroyed, resized = FALSE;
    HWND hwnd = NULL;
    HDC hdc;
    RECT rect;
    int len;

    cls.style = CS_OWNDC;
    cls.lpfnWndProc = detached_gdi_wndproc;
    cls.hInstance = GetModuleHandleA( NULL );
    cls.lpszClassName = "direct_foreign_gdi_class";
    ok( RegisterClassA( &cls ), "RegisterClassA failed, error %lu\n", GetLastError() );
    hwnd = CreateWindowExA( WS_EX_TOPMOST, cls.lpszClassName, NULL, WS_POPUP | WS_VISIBLE,
                            GetSystemMetrics( SM_CXSCREEN ) / 2 - 150,
                            GetSystemMetrics( SM_CYSCREEN ) / 2 - 90,
                            96, 96, NULL, NULL, cls.hInstance, NULL );
    ok( !!hwnd, "CreateWindowExA failed, error %lu\n", GetLastError() );
    if (!hwnd) goto cleanup;
    UpdateWindow( hwnd );

    winetest_push_context( "direct same-process control" );
    if (!paint_gdi_bands( hwnd ) || !check_gdi_bands( hwnd ))
    {
        winetest_pop_context();
        goto cleanup;
    }
    hdc = GetDC( hwnd );
    ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
    if (hdc)
    {
        ok( PatBlt( hdc, 0, 0, 64, 64, WHITENESS ), "PatBlt WHITENESS failed\n" );
        ok( GdiFlush(), "GdiFlush failed\n" );
        cleared = check_gdi_pixel( hdc, 16, 32, RGB(255, 255, 255) );
        cleared &= check_gdi_pixel( hdc, 48, 32, RGB(255, 255, 255) );
        ReleaseDC( hwnd, hdc );
    }
    winetest_pop_context();
    if (!cleared) goto cleanup;

    ShowWindow( hwnd, SW_HIDE );
    ok( !IsWindowVisible( hwnd ), "owner is still visible\n" );
    direct_gdi_event_names( hwnd, GetCurrentProcessId(), proceed_name, done_name );
    proceed = CreateEventA( NULL, FALSE, FALSE, proceed_name );
    done = CreateEventA( NULL, FALSE, FALSE, done_name );
    ok( !!proceed && !!done, "CreateEventA failed, error %lu\n", GetLastError() );
    if (!proceed || !done) goto cleanup;
    len = snprintf( cmdline, sizeof(cmdline), "\"%s\" %s direct_foreign_gdi %p", argv[0], argv[1], hwnd );
    ok( len >= 0 && (size_t)len < sizeof(cmdline), "child command line is too long\n" );
    if (len < 0 || (size_t)len >= sizeof(cmdline)) goto cleanup;
    startup.cb = sizeof(startup);
    if (!CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info ))
    {
        ok( 0, "CreateProcessA failed, error %lu\n", GetLastError() );
        goto cleanup;
    }
    objects[0] = done;
    objects[1] = info.hProcess;

    winetest_push_context( "direct foreign DC acquired before owner show" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child DC acquisition wait returned %#lx\n", ret );
    if (ret == 0)
    {
        ShowWindow( hwnd, SW_SHOWNA );
        ok( IsWindowVisible( hwnd ), "owner is still hidden\n" );
        UpdateWindow( hwnd );
        cleared = FALSE;
        hdc = GetDC( hwnd );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            ok( PatBlt( hdc, 0, 0, 64, 64, WHITENESS ), "PatBlt WHITENESS failed\n" );
            ok( GdiFlush(), "GdiFlush failed\n" );
            cleared = check_gdi_pixel( hdc, 16, 32, RGB(255, 255, 255) );
            cleared &= check_gdi_pixel( hdc, 48, 32, RGB(255, 255, 255) );
            ReleaseDC( hwnd, hdc );
        }
    }
    winetest_pop_context();
    if (ret != 0 || !cleared) goto cleanup;
    signaled = SetEvent( proceed );
    ok( signaled, "SetEvent failed, error %lu\n", GetLastError() );
    if (!signaled) goto cleanup;

    winetest_push_context( "direct child writes, owner reads" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child drawing wait returned %#lx\n", ret );
    if (ret == 0)
    {
        check_gdi_bands( hwnd );
        cleared = FALSE;
        hdc = GetDC( hwnd );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            ok( PatBlt( hdc, 0, 0, 64, 64, WHITENESS ), "PatBlt WHITENESS failed\n" );
            ok( GdiFlush(), "GdiFlush failed\n" );
            cleared = check_gdi_pixel( hdc, 16, 32, RGB(255, 255, 255) );
            cleared &= check_gdi_pixel( hdc, 48, 32, RGB(255, 255, 255) );
            ReleaseDC( hwnd, hdc );
        }
    }
    winetest_pop_context();
    if (ret != 0 || !cleared) goto cleanup;
    signaled = SetEvent( proceed );
    ok( signaled, "SetEvent failed, error %lu\n", GetLastError() );
    if (!signaled) goto cleanup;

    winetest_push_context( "direct child destination ROP, owner reads" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child ROP wait returned %#lx\n", ret );
    if (ret == 0)
    {
        hdc = GetDC( hwnd );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            check_gdi_pixel( hdc, 16, 32, RGB(0, 0, 0) );
            check_gdi_pixel( hdc, 48, 32, RGB(0, 0, 0) );
            ReleaseDC( hwnd, hdc );
        }
        resized = SetWindowPos( hwnd, NULL, 0, 0, 104, 104,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
        ok( resized, "SetWindowPos failed, error %lu\n", GetLastError() );
        if (resized) UpdateWindow( hwnd );
    }
    winetest_pop_context();
    if (ret != 0 || !resized) goto cleanup;
    signaled = SetEvent( proceed );
    ok( signaled, "SetEvent failed, error %lu\n", GetLastError() );
    if (!signaled) goto cleanup;

    winetest_push_context( "direct retained foreign DC after small resize" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child small resize drawing wait returned %#lx\n", ret );
    if (ret == 0)
    {
        hdc = GetDC( hwnd );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            check_gdi_pixel( hdc, 100, 80, RGB(0, 255, 0) );
            ReleaseDC( hwnd, hdc );
        }
        resized = SetWindowPos( hwnd, NULL, 0, 0, 300, 180,
                                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE );
        ok( resized, "SetWindowPos failed, error %lu\n", GetLastError() );
        if (resized)
        {
            UpdateWindow( hwnd );
            GetClientRect( hwnd, &rect );
            ok( rect.right >= 224 && rect.bottom >= 96,
                "resized client is %ldx%ld\n", rect.right, rect.bottom );
            winetest_push_context( "direct old pixels immediately after resize" );
            hdc = GetDC( hwnd );
            ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
            if (hdc)
            {
                check_gdi_pixel( hdc, 16, 32, RGB(0, 0, 0) );
                ReleaseDC( hwnd, hdc );
            }
            winetest_pop_context();
        }
    }
    winetest_pop_context();
    if (ret != 0 || !resized) goto cleanup;
    signaled = SetEvent( proceed );
    ok( signaled, "SetEvent failed, error %lu\n", GetLastError() );
    if (!signaled) goto cleanup;

    winetest_push_context( "direct retained foreign DC after large resize" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child resized drawing wait returned %#lx\n", ret );
    if (ret == 0)
    {
        hdc = GetDC( hwnd );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            check_gdi_pixel( hdc, 180, 80, RGB(0, 255, 0) );
            ReleaseDC( hwnd, hdc );
        }
    }
    winetest_pop_context();
    if (ret != 0) goto cleanup;

    winetest_push_context( "direct foreign DC owner destruction" );
    destroyed = DestroyWindow( hwnd );
    ok( destroyed, "DestroyWindow failed, error %lu\n", GetLastError() );
    if (destroyed) hwnd = NULL;
    signaled = destroyed && SetEvent( proceed );
    ok( signaled, "SetEvent after destruction failed, error %lu\n", GetLastError() );
    if (!signaled)
    {
        winetest_pop_context();
        goto cleanup;
    }
    ret = wait_for_gdi_objects( 1, &info.hProcess );
    ok( ret == 0, "child exit wait returned %#lx\n", ret );
    finished = ret == 0;
    winetest_pop_context();

cleanup:
    if (info.hProcess)
    {
        if (!finished)
        {
            terminated = TerminateProcess( info.hProcess, 1 );
            ret = wait_for_gdi_objects( 1, &info.hProcess );
            ok( ret == 0, "child cleanup wait returned %#lx\n", ret );
        }
        if (ret == 0 && !terminated)
        {
            if (GetExitCodeProcess( info.hProcess, &exit_code ))
                ok( exit_code == 0, "child exited with status %#lx\n", exit_code );
            else
                ok( 0, "GetExitCodeProcess failed, error %lu\n", GetLastError() );
        }
        CloseHandle( info.hThread );
        CloseHandle( info.hProcess );
    }
    if (done) CloseHandle( done );
    if (proceed) CloseHandle( proceed );
    if (hwnd) DestroyWindow( hwnd );
    UnregisterClassA( cls.lpszClassName, cls.hInstance );
}

static void stale_child_gdi_proc( HWND child )
{
    RECT rect = { 0, 0, 64, 64 };
    char proceed_name[80], done_name[80];
    HANDLE proceed = NULL, done = NULL;
    DWORD owner_pid = 0, ret;
    HBRUSH red;
    HDC hdc;

    GetWindowThreadProcessId( child, &owner_pid );
    ok( owner_pid && owner_pid != GetCurrentProcessId(), "child belongs to process %lu\n", owner_pid );
    if (!owner_pid || owner_pid == GetCurrentProcessId()) return;
    direct_gdi_event_names( child, owner_pid, proceed_name, done_name );
    proceed = OpenEventA( EVENT_ALL_ACCESS, FALSE, proceed_name );
    done = OpenEventA( EVENT_ALL_ACCESS, FALSE, done_name );
    ok( !!proceed && !!done, "OpenEventA failed, error %lu\n", GetLastError() );
    if (!proceed || !done) goto cleanup;

    hdc = GetDC( child );
    ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
    if (!hdc) goto cleanup;
    red = CreateSolidBrush( RGB(255, 0, 0) );
    ok( !!red, "CreateSolidBrush failed\n" );
    if (red)
    {
        ok( FillRect( hdc, &rect, red ), "FillRect failed\n" );
        ok( GdiFlush(), "GdiFlush failed\n" );
        DeleteObject( red );
    }
    ok( SetEvent( done ), "SetEvent failed, error %lu\n", GetLastError() );
    ret = WaitForSingleObject( proceed, 5000 );
    ok( ret == WAIT_OBJECT_0, "child destruction wait returned %#lx\n", ret );
    if (ret == WAIT_OBJECT_0)
    {
        PatBlt( hdc, 0, 0, 64, 64, BLACKNESS );
        GdiFlush();
    }
    ReleaseDC( child, hdc );

cleanup:
    if (done) CloseHandle( done );
    if (proceed) CloseHandle( proceed );
}

static void test_stale_child_gdi( char **argv )
{
    WNDCLASSA cls = {0};
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION info = {0};
    char proceed_name[80], done_name[80], cmdline[MAX_PATH * 2];
    HANDLE proceed = NULL, done = NULL, objects[2];
    DWORD ret, exit_code;
    BOOL finished = FALSE, terminated = FALSE, cleared = FALSE;
    BOOL destroyed, signaled;
    HWND root = NULL, child = NULL;
    HDC hdc;
    int len;

    cls.lpfnWndProc = detached_gdi_wndproc;
    cls.hInstance = GetModuleHandleA( NULL );
    cls.lpszClassName = "stale_child_gdi_class";
    ok( RegisterClassA( &cls ), "RegisterClassA failed, error %lu\n", GetLastError() );
    root = CreateWindowExA( WS_EX_TOPMOST, cls.lpszClassName, NULL, WS_POPUP | WS_VISIBLE,
                            GetSystemMetrics( SM_CXSCREEN ) / 2 - 64,
                            GetSystemMetrics( SM_CYSCREEN ) / 2 - 64,
                            128, 128, NULL, NULL, cls.hInstance, NULL );
    ok( !!root, "CreateWindowExA root failed, error %lu\n", GetLastError() );
    if (!root) goto cleanup;
    child = CreateWindowExA( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE,
                             0, 0, 64, 64, root, NULL, cls.hInstance, NULL );
    ok( !!child, "CreateWindowExA child failed, error %lu\n", GetLastError() );
    if (!child) goto cleanup;
    UpdateWindow( root );
    UpdateWindow( child );

    direct_gdi_event_names( child, GetCurrentProcessId(), proceed_name, done_name );
    proceed = CreateEventA( NULL, FALSE, FALSE, proceed_name );
    done = CreateEventA( NULL, FALSE, FALSE, done_name );
    ok( !!proceed && !!done, "CreateEventA failed, error %lu\n", GetLastError() );
    if (!proceed || !done) goto cleanup;
    len = snprintf( cmdline, sizeof(cmdline), "\"%s\" %s stale_child_gdi %p", argv[0], argv[1], child );
    ok( len >= 0 && (size_t)len < sizeof(cmdline), "child command line is too long\n" );
    if (len < 0 || (size_t)len >= sizeof(cmdline)) goto cleanup;
    startup.cb = sizeof(startup);
    if (!CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info ))
    {
        ok( 0, "CreateProcessA failed, error %lu\n", GetLastError() );
        goto cleanup;
    }
    objects[0] = done;
    objects[1] = info.hProcess;

    winetest_push_context( "foreign child drawing before destruction" );
    ret = wait_for_gdi_objects( 2, objects );
    ok( ret == 0, "child drawing wait returned %#lx\n", ret );
    if (ret == 0)
    {
        hdc = GetDC( child );
        ok( !!hdc, "GetDC failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            check_gdi_pixel( hdc, 16, 16, RGB(255, 0, 0) );
            ReleaseDC( child, hdc );
        }
    }
    winetest_pop_context();
    if (ret != 0) goto cleanup;

    winetest_push_context( "foreign child DC after child destruction" );
    destroyed = DestroyWindow( child );
    ok( destroyed, "DestroyWindow child failed, error %lu\n", GetLastError() );
    if (!destroyed)
    {
        winetest_pop_context();
        goto cleanup;
    }
    child = NULL;
    UpdateWindow( root );
    hdc = GetDC( root );
    ok( !!hdc, "GetDC root failed, error %lu\n", GetLastError() );
    if (hdc)
    {
        ok( PatBlt( hdc, 0, 0, 64, 64, WHITENESS ), "PatBlt WHITENESS failed\n" );
        ok( GdiFlush(), "GdiFlush failed\n" );
        cleared = check_gdi_pixel( hdc, 16, 16, RGB(255, 255, 255) );
        ReleaseDC( root, hdc );
    }
    if (!cleared)
    {
        winetest_pop_context();
        goto cleanup;
    }
    signaled = SetEvent( proceed );
    ok( signaled, "SetEvent failed, error %lu\n", GetLastError() );
    if (!signaled)
    {
        winetest_pop_context();
        goto cleanup;
    }
    ret = wait_for_gdi_objects( 1, &info.hProcess );
    ok( ret == 0, "child exit wait returned %#lx\n", ret );
    finished = ret == 0;
    if (finished)
    {
        hdc = GetDC( root );
        ok( !!hdc, "GetDC root failed, error %lu\n", GetLastError() );
        if (hdc)
        {
            check_gdi_pixel( hdc, 16, 16, RGB(255, 255, 255) );
            ReleaseDC( root, hdc );
        }
    }
    winetest_pop_context();

cleanup:
    if (info.hProcess)
    {
        if (!finished)
        {
            terminated = TerminateProcess( info.hProcess, 1 );
            ret = wait_for_gdi_objects( 1, &info.hProcess );
            ok( ret == 0, "child cleanup wait returned %#lx\n", ret );
        }
        if (ret == 0 && !terminated)
        {
            if (GetExitCodeProcess( info.hProcess, &exit_code ))
                ok( exit_code == 0, "child exited with status %#lx\n", exit_code );
            else
                ok( 0, "GetExitCodeProcess failed, error %lu\n", GetLastError() );
        }
        CloseHandle( info.hThread );
        CloseHandle( info.hProcess );
    }
    if (done) CloseHandle( done );
    if (proceed) CloseHandle( proceed );
    if (child) DestroyWindow( child );
    if (root) DestroyWindow( root );
    UnregisterClassA( cls.lpszClassName, cls.hInstance );
}

/* test behavior of DC attributes with various GetDC/ReleaseDC combinations */
static void test_dc_attributes(void)
{
    HDC hdc, old_hdc;
    HDC hdcs[20];
    INT i, rop, def_rop, caps;
    BOOL found_dc;

    /* test cache DC */

    hdc = GetDC( hwnd_cache );
    def_rop = GetROP2( hdc );

    SetROP2( hdc, R2_WHITE );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );

    ok( WindowFromDC( hdc ) == hwnd_cache, "wrong window\n" );
    ReleaseDC( hwnd_cache, hdc );
    ok( WindowFromDC( hdc ) != hwnd_cache, "wrong window\n" );
    hdc = GetDC( hwnd_cache );
    rop = GetROP2( hdc );
    ok( rop == def_rop, "wrong ROP2 %d after release\n", rop );
    SetROP2( hdc, R2_WHITE );
    ok( WindowFromDC( hdc ) == hwnd_cache, "wrong window\n" );
    ReleaseDC( hwnd_cache, hdc );
    old_hdc = hdc;

    found_dc = FALSE;
    for (i = 0; i < 20; i++)
    {
        hdc = hdcs[i] = GetDCEx( hwnd_cache, 0, DCX_USESTYLE | DCX_NORESETATTRS );
        if (!hdc) break;
        rop = GetROP2( hdc );
        ok( rop == def_rop, "wrong ROP2 %d after release %p/%p\n", rop, old_hdc, hdc );
        if (hdc == old_hdc)
        {
            found_dc = TRUE;
            SetROP2( hdc, R2_WHITE );
        }
    }
    if (!found_dc)
    {
        trace( "hdc %p not found in cache using %p\n", old_hdc, hdcs[0] );
        old_hdc = hdcs[0];
        SetROP2( old_hdc, R2_WHITE );
    }
    while (i > 0) ReleaseDC( hwnd_cache, hdcs[--i] );

    for (i = 0; i < 20; i++)
    {
        hdc = hdcs[i] = GetDCEx( hwnd_cache, 0, DCX_USESTYLE | DCX_NORESETATTRS );
        if (!hdc) break;
        rop = GetROP2( hdc );
        if (hdc == old_hdc)
            ok( rop == R2_WHITE || broken( rop == def_rop),  /* win9x doesn't support DCX_NORESETATTRS */
                "wrong ROP2 %d after release %p/%p\n", rop, old_hdc, hdc );
        else
            ok( rop == def_rop, "wrong ROP2 %d after release %p/%p\n", rop, old_hdc, hdc );
    }
    while (i > 0) ReleaseDC( hwnd_cache, hdcs[--i] );

    for (i = 0; i < 20; i++)
    {
        hdc = hdcs[i] = GetDCEx( hwnd_cache, 0, DCX_USESTYLE );
        if (!hdc) break;
        rop = GetROP2( hdc );
        if (hdc == old_hdc)
        {
            ok( rop == R2_WHITE || broken( rop == def_rop),
                "wrong ROP2 %d after release %p/%p\n", rop, old_hdc, hdc );
            SetROP2( old_hdc, def_rop );
        }
        else
            ok( rop == def_rop, "wrong ROP2 %d after release %p/%p\n", rop, old_hdc, hdc );
    }
    while (i > 0) ReleaseDC( hwnd_cache, hdcs[--i] );

    /* Released cache DCs are 'disabled' */
    rop = SetROP2( old_hdc, R2_BLACK );
    ok( rop == 0, "got %d\n", rop );
    rop = GetROP2( old_hdc );
    ok( rop == 0, "got %d\n", rop );
    caps = GetDeviceCaps( old_hdc, HORZRES );
    ok( caps == 0, "got %d\n", caps );
    caps = GetDeviceCaps( old_hdc, VERTRES );
    ok( caps == 0, "got %d\n", caps );
    caps = GetDeviceCaps( old_hdc, NUMCOLORS );
    ok( caps == 0, "got %d\n", caps );
    ok( WindowFromDC( old_hdc ) != hwnd_cache, "wrong window\n" );

    hdc = GetDC(0);
    caps = GetDeviceCaps( hdc, HORZRES );
    ok( caps != 0, "got %d\n", caps );
    caps = GetDeviceCaps( hdc, VERTRES );
    ok( caps != 0, "got %d\n", caps );
    caps = GetDeviceCaps( hdc, NUMCOLORS );
    ok( caps != 0, "got %d\n", caps );
    ReleaseDC( 0, hdc );
    caps = GetDeviceCaps( hdc, HORZRES );
    ok( caps == 0, "got %d\n", caps );
    caps = GetDeviceCaps( hdc, VERTRES );
    ok( caps == 0, "got %d\n", caps );
    caps = GetDeviceCaps( hdc, NUMCOLORS );
    ok( caps == 0, "got %d\n", caps );

    /* test own DC */

    hdc = GetDC( hwnd_owndc );
    SetROP2( hdc, R2_WHITE );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );

    old_hdc = hdc;
    ok( WindowFromDC( hdc ) == hwnd_owndc, "wrong window\n" );
    ReleaseDC( hwnd_owndc, hdc );
    ok( WindowFromDC( hdc ) == hwnd_owndc, "wrong window\n" );
    hdc = GetDC( hwnd_owndc );
    ok( old_hdc == hdc, "didn't get same DC %p/%p\n", old_hdc, hdc );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d after release\n", rop );
    ok( WindowFromDC( hdc ) == hwnd_owndc, "wrong window\n" );
    ReleaseDC( hwnd_owndc, hdc );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d after second release\n", rop );

    /* test class DC */

    hdc = GetDC( hwnd_classdc );
    SetROP2( hdc, R2_WHITE );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );

    old_hdc = hdc;
    ok( WindowFromDC( hdc ) == hwnd_classdc, "wrong window\n" );
    ReleaseDC( hwnd_classdc, hdc );
    ok( WindowFromDC( hdc ) == hwnd_classdc, "wrong window\n" );
    hdc = GetDC( hwnd_classdc );
    ok( old_hdc == hdc, "didn't get same DC %p/%p\n", old_hdc, hdc );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d after release\n", rop );
    ok( WindowFromDC( hdc ) == hwnd_classdc, "wrong window\n" );
    ReleaseDC( hwnd_classdc, hdc );
    rop = GetROP2( hdc );
    ok( rop == R2_WHITE, "wrong ROP2 %d after second release\n", rop );

    /* test class DC with 2 windows */

    old_hdc = GetDC( hwnd_classdc );
    SetROP2( old_hdc, R2_BLACK );
    ok( WindowFromDC( old_hdc ) == hwnd_classdc, "wrong window\n" );
    hdc = GetDC( hwnd_classdc2 );
    ok( old_hdc == hdc, "didn't get same DC %p/%p\n", old_hdc, hdc );
    rop = GetROP2( hdc );
    ok( rop == R2_BLACK, "wrong ROP2 %d for other window\n", rop );
    ok( WindowFromDC( hdc ) == hwnd_classdc2, "wrong window\n" );
    ReleaseDC( hwnd_classdc, old_hdc );
    ReleaseDC( hwnd_classdc, hdc );
    ok( WindowFromDC( hdc ) == hwnd_classdc2, "wrong window\n" );
    rop = GetROP2( hdc );
    ok( rop == R2_BLACK, "wrong ROP2 %d after release\n", rop );
}


/* test behavior with various invalid parameters */
static void test_parameters(void)
{
    HDC hdc;

    hdc = GetDC( hwnd_cache );
    ok( ReleaseDC( hwnd_owndc, hdc ), "ReleaseDC with wrong window should succeed\n" );

    hdc = GetDC( hwnd_cache );
    ok( !ReleaseDC( hwnd_cache, 0 ), "ReleaseDC with wrong HDC should fail\n" );
    ok( ReleaseDC( hwnd_cache, hdc ), "correct ReleaseDC should succeed\n" );
    ok( !ReleaseDC( hwnd_cache, hdc ), "second ReleaseDC should fail\n" );

    hdc = GetDC( hwnd_owndc );
    ok( ReleaseDC( hwnd_cache, hdc ), "ReleaseDC with wrong window should succeed\n" );
    hdc = GetDC( hwnd_owndc );
    ok( ReleaseDC( hwnd_owndc, hdc ), "correct ReleaseDC should succeed\n" );
    ok( ReleaseDC( hwnd_owndc, hdc ), "second ReleaseDC should succeed\n" );

    hdc = GetDC( hwnd_classdc );
    ok( ReleaseDC( hwnd_cache, hdc ), "ReleaseDC with wrong window should succeed\n" );
    hdc = GetDC( hwnd_classdc );
    ok( ReleaseDC( hwnd_classdc, hdc ), "correct ReleaseDC should succeed\n" );
    ok( ReleaseDC( hwnd_classdc, hdc ), "second ReleaseDC should succeed\n" );
}


static void test_dc_visrgn(void)
{
    HDC old_hdc, hdc;
    HRGN hrgn, hrgn2;
    RECT rect, parent_rect;

    /* cache DC */

    SetRect( &rect, 10, 10, 20, 20 );
    MapWindowPoints( hwnd_cache, 0, (POINT *)&rect, 2 );
    hrgn = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_cache, hrgn, DCX_INTERSECTRGN | DCX_USESTYLE );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_cache, hdc );
    ok( GetRgnBox( hrgn, &rect ) == ERROR, "region must no longer be valid\n" );

    /* cache DC with NORESETATTRS */

    SetRect( &rect, 10, 10, 20, 20 );
    MapWindowPoints( hwnd_cache, 0, (POINT *)&rect, 2 );
    hrgn = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_cache, hrgn, DCX_INTERSECTRGN | DCX_USESTYLE | DCX_NORESETATTRS );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_cache, hdc );
    ok( GetRgnBox( hrgn, &rect ) == ERROR, "region must no longer be valid\n" );
    hdc = GetDCEx( hwnd_cache, 0, DCX_USESTYLE | DCX_NORESETATTRS );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20),
        "clip box should have been reset %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_cache, hdc );

    /* window DC */

    SetRect( &rect, 10, 10, 20, 20 );
    MapWindowPoints( hwnd_owndc, 0, (POINT *)&rect, 2 );
    hrgn = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_owndc, hrgn, DCX_INTERSECTRGN | DCX_USESTYLE );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_owndc, hdc );
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    hdc = GetDCEx( hwnd_owndc, 0, DCX_USESTYLE );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_owndc, hdc );
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );

    SetRect( &rect, 20, 20, 30, 30 );
    MapWindowPoints( hwnd_owndc, 0, (POINT *)&rect, 2 );
    hrgn2 = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_owndc, hrgn2, DCX_INTERSECTRGN | DCX_USESTYLE );
    ok( GetRgnBox( hrgn, &rect ) == ERROR, "region must no longer be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 20 && rect.top >= 20 && rect.right <= 30 && rect.bottom <= 30,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );
    ReleaseDC( hwnd_owndc, hdc );
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );
    hdc = GetDCEx( hwnd_owndc, 0, DCX_EXCLUDERGN | DCX_USESTYLE );
    ok( GetRgnBox( hrgn2, &rect ) == ERROR, "region must no longer be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 20 && rect.top >= 20 && rect.right <= 30 && rect.bottom <= 30),
        "clip box should have been reset %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_owndc, hdc );

    /* class DC */

    SetRect( &rect, 10, 10, 20, 20 );
    MapWindowPoints( hwnd_classdc, 0, (POINT *)&rect, 2 );
    hrgn = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_classdc, hrgn, DCX_INTERSECTRGN | DCX_USESTYLE );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_classdc, hdc );
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));

    hdc = GetDCEx( hwnd_classdc, 0, DCX_USESTYLE );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );
    ReleaseDC( hwnd_classdc, hdc );
    ok( GetRgnBox( hrgn, &rect ) != ERROR, "region must still be valid\n" );

    SetRect( &rect, 20, 20, 30, 30 );
    MapWindowPoints( hwnd_classdc, 0, (POINT *)&rect, 2 );
    hrgn2 = CreateRectRgnIndirect( &rect );
    hdc = GetDCEx( hwnd_classdc, hrgn2, DCX_INTERSECTRGN | DCX_USESTYLE );
    ok( GetRgnBox( hrgn, &rect ) == ERROR, "region must no longer be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 20 && rect.top >= 20 && rect.right <= 30 && rect.bottom <= 30,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );

    old_hdc = hdc;
    hdc = GetDCEx( hwnd_classdc2, 0, DCX_USESTYLE );
    ok( old_hdc == hdc, "did not get the same hdc %p/%p\n", old_hdc, hdc );
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 20 && rect.top >= 20 && rect.right <= 30 && rect.bottom <= 30),
        "clip box should have been reset %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_classdc2, hdc );
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );
    hdc = GetDCEx( hwnd_classdc2, 0, DCX_EXCLUDERGN | DCX_USESTYLE );
    ok( GetRgnBox( hrgn2, &rect ) != ERROR, "region2 must still be valid\n" );
    ok( !(rect.left >= 20 && rect.top >= 20 && rect.right <= 30 && rect.bottom <= 30),
        "clip box must have been reset %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_classdc2, hdc );

    /* parent DC */
    hdc = GetDC( hwnd_parentdc );
    GetClipBox( hdc, &rect );
    MapWindowPoints(hwnd_parentdc, hwnd_parent, (POINT *)&rect, 2);
    ReleaseDC( hwnd_parentdc, hdc );

    hdc = GetDC( hwnd_parent );
    GetClipBox( hdc, &parent_rect );
    ReleaseDC( hwnd_parent, hdc );

    ok( EqualRect( &rect, &parent_rect ), "rect = %s, expected %s\n", wine_dbgstr_rect( &rect ),
        wine_dbgstr_rect( &parent_rect ));
}


/* test various BeginPaint/EndPaint behaviors */
static void test_begin_paint(void)
{
    HDC old_hdc, hdc;
    RECT rect, parent_rect, client_rect;
    PAINTSTRUCT ps;
    COLORREF cr;

    /* cache DC */

    /* clear update region */
    RedrawWindow( hwnd_cache, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 10, 10, 20, 20 );
    RedrawWindow( hwnd_cache, &rect, 0, RDW_INVALIDATE );
    hdc = BeginPaint( hwnd_cache, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_cache, &ps );

    SetWindowPos( hwnd_cache, 0, 0, 0, 100, 100, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    RedrawWindow( hwnd_cache, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 0, 0, 150, 150 );
    RedrawWindow( hwnd_cache, &rect, 0, RDW_INVALIDATE|RDW_NOFRAME|RDW_NOERASE );
    hdc = BeginPaint( hwnd_cache, &ps );
    GetClipBox( hdc, &rect );
    GetClientRect( hwnd_cache, &client_rect );
    ok( EqualRect( &rect, &client_rect ), "clip box = %s, expected %s\n",
            wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &client_rect ));
    SetWindowPos( hwnd_cache, 0, 0, 0, 200, 200, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    GetClipBox( hdc, &rect );
    GetClientRect( hwnd_cache, &client_rect );
    todo_wine ok( (!rect.left && !rect.top && rect.right == 150 && rect.bottom == 150) ||
            broken( EqualRect( &rect, &client_rect )),
            "clip box = %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_cache, &ps );

    SetWindowPos( hwnd_cache, 0, 0, 0, 100, 100, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    RedrawWindow( hwnd_cache, NULL, 0, RDW_INVALIDATE|RDW_NOFRAME|RDW_NOERASE );
    hdc = BeginPaint( hwnd_cache, &ps );
    SetWindowPos( hwnd_cache, 0, 0, 0, 200, 200, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    GetClipBox( hdc, &rect );
    GetClientRect( hwnd_cache, &client_rect );
    todo_wine ok( EqualRect( &rect, &client_rect ), "clip box = %s, expected %s\n",
            wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &client_rect ));
    EndPaint( hwnd_cache, &ps );

    /* window DC */

    RedrawWindow( hwnd_owndc, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 10, 10, 20, 20 );
    RedrawWindow( hwnd_owndc, &rect, 0, RDW_INVALIDATE );
    hdc = BeginPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_owndc, hdc );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    ok( GetDC( hwnd_owndc ) == hdc, "got different hdc\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20),
        "clip box should have been reset %s\n", wine_dbgstr_rect( &rect ));
    RedrawWindow( hwnd_owndc, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 10, 10, 20, 20 );
    RedrawWindow( hwnd_owndc, &rect, 0, RDW_INVALIDATE|RDW_ERASE );
    ok( GetDC( hwnd_owndc ) == hdc, "got different hdc\n" );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20),
        "clip box should be the whole window %s\n", wine_dbgstr_rect( &rect ));
    RedrawWindow( hwnd_owndc, NULL, 0, RDW_ERASENOW );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20),
        "clip box should still be the whole window %s\n", wine_dbgstr_rect( &rect ));

    SetWindowPos( hwnd_owndc, 0, 0, 0, 100, 100, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    RedrawWindow( hwnd_owndc, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 0, 0, 50, 50 );
    RedrawWindow( hwnd_owndc, &rect, 0, RDW_INVALIDATE|RDW_ERASE );
    hdc = BeginPaint( hwnd_owndc, &ps );
    GetClipBox( hdc, &rect );
    ok( !rect.left && !rect.top && rect.right == 50 && rect.bottom == 50,
            "clip box = %s\n", wine_dbgstr_rect( &rect ));
    SetWindowPos( hwnd_owndc, 0, 0, 0, 200, 200, SWP_NOZORDER|SWP_NOMOVE|SWP_NOACTIVATE );
    GetClipBox( hdc, &rect );
    GetClientRect( hwnd_owndc, &client_rect );
    ok( EqualRect( &rect, &client_rect ), "clip box = %s, expected %s\n",
            wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &client_rect ));
    EndPaint( hwnd_owndc, &ps );

    /* class DC */

    RedrawWindow( hwnd_classdc, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    SetRect( &rect, 10, 10, 20, 20 );
    RedrawWindow( hwnd_classdc, &rect, 0, RDW_INVALIDATE );
    hdc = BeginPaint( hwnd_classdc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));

    old_hdc = hdc;
    hdc = GetDC( hwnd_classdc2 );
    ok( old_hdc == hdc, "did not get the same hdc %p/%p\n", old_hdc, hdc );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( !(rect.left >= 10 && rect.top >= 10 && rect.right <= 20 && rect.bottom <= 20),
        "clip box should have been reset %s\n", wine_dbgstr_rect( &rect ));
    ReleaseDC( hwnd_classdc2, hdc );
    EndPaint( hwnd_classdc, &ps );

    /* parent DC */
    RedrawWindow( hwnd_parent, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    RedrawWindow( hwnd_parentdc, NULL, 0, RDW_INVALIDATE );
    hdc = BeginPaint( hwnd_parentdc, &ps );
    GetClipBox( hdc, &rect );
    MapWindowPoints(hwnd_parentdc, hwnd_parent, (POINT *)&rect, 2);
    cr = SetPixel( hdc, 10, 10, RGB(255, 0, 0) );
    ok( cr != -1, "error drawing outside of window client area\n" );
    EndPaint( hwnd_parentdc, &ps );
    GetClientRect( hwnd_parent, &parent_rect );

    todo_wine ok( rect.left == parent_rect.left, "rect.left = %ld, expected %ld\n", rect.left, parent_rect.left );
    todo_wine ok( rect.top == parent_rect.top, "rect.top = %ld, expected %ld\n", rect.top, parent_rect.top );
    todo_wine ok( rect.right == parent_rect.right, "rect.right = %ld, expected %ld\n", rect.right, parent_rect.right );
    todo_wine ok( rect.bottom == parent_rect.bottom, "rect.bottom = %ld, expected %ld\n", rect.bottom, parent_rect.bottom );

    hdc = GetDC( hwnd_parent );
    todo_wine ok( GetPixel( hdc, 60, 60 ) == cr, "error drawing outside of window client area\n" );
    ReleaseDC( hwnd_parent, hdc );
}

static void test_cropped_parentdc_paint_clipbox(void)
{
    /* Use WS_BORDER to differentiate window and client rects */
    const UINT common_style = WS_VISIBLE | WS_BORDER;
    HWND hwnd_toplevel, hwnd_container, hwnd_child;
    RECT parent_rect, expect_rect, toplevel_rect, rect;
    POINT dot_pos = { 10, 10 };
    PAINTSTRUCT ps;
    COLORREF cr;
    HDC hdc;

    hwnd_toplevel = CreateWindowA( "static", NULL, WS_OVERLAPPED | common_style,
                                   400, 200, 100, 100, NULL, NULL, NULL, NULL );

    /* Container window extends "outside" the top-level window (hence cropped) */
    hwnd_container = CreateWindowA( "static", NULL, WS_CHILD | common_style,
                                    -40, -40, 180, 180, hwnd_toplevel, NULL, NULL, NULL );

    hwnd_child = CreateWindowA( "parentdc_class", NULL, WS_CHILD | common_style,
                                50, 50, 3, 3, hwnd_container, NULL, NULL, NULL );

    RedrawWindow( hwnd_toplevel, NULL, 0, RDW_VALIDATE|RDW_NOFRAME|RDW_NOERASE );
    RedrawWindow( hwnd_child, NULL, 0, RDW_INVALIDATE );

    hdc = BeginPaint( hwnd_child, &ps );
    GetClipBox( hdc, &rect );
    cr = SetPixel( hdc, dot_pos.x, dot_pos.y, RGB(255, 0, 0) );
    ok( cr != -1, "error drawing outside of window client area\n" );
    EndPaint( hwnd_child, &ps );

    GetClientRect( hwnd_toplevel, &toplevel_rect );
    MapWindowPoints( hwnd_toplevel, hwnd_container, (POINT *)&toplevel_rect, 2);
    GetClientRect( hwnd_container, &parent_rect );
    IntersectRect( &expect_rect, &toplevel_rect, &parent_rect );

    MapWindowPoints( hwnd_child, hwnd_container, (POINT *)&rect, 2 );
    todo_wine
    ok( EqualRect( &rect, &expect_rect ), "rect = %s, expected %s\n",
        wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &expect_rect ) );

    hdc = GetDC( hwnd_container );
    MapWindowPoints( hwnd_child, hwnd_container, &dot_pos, 1 );
    todo_wine
    ok( GetPixel( hdc, dot_pos.x, dot_pos.y ) == cr, "error drawing outside of window client area\n" );
    ReleaseDC( hwnd_container, hdc );

    DestroyWindow( hwnd_toplevel );
}

/* test ScrollWindow with window DCs */
static void test_scroll_window(void)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT clip, rect;

    /* ScrollWindow uses the window DC, ScrollWindowEx doesn't */

    UpdateWindow( hwnd_owndc );
    SetRect( &clip, 25, 25, 50, 50 );
    ScrollWindow( hwnd_owndc, -5, -10, NULL, &clip );
    hdc = BeginPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 25 && rect.top >= 25 && rect.right <= 50 && rect.bottom <= 50,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_owndc, &ps );

    SetViewportExtEx( hdc, 2, 3, NULL );
    SetViewportOrgEx( hdc, 30, 20, NULL );

    ScrollWindow( hwnd_owndc, -5, -10, NULL, &clip );
    hdc = BeginPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 25 && rect.top >= 25 && rect.right <= 50 && rect.bottom <= 50,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_owndc, &ps );

    ScrollWindowEx( hwnd_owndc, -5, -10, NULL, &clip, 0, NULL, SW_INVALIDATE | SW_ERASE );
    hdc = BeginPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= -5 && rect.top >= 5 && rect.right <= 20 && rect.bottom <= 30,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_owndc, &ps );

    SetViewportExtEx( hdc, 1, 1, NULL );
    SetViewportOrgEx( hdc, 0, 0, NULL );

    ScrollWindowEx( hwnd_owndc, -5, -10, NULL, &clip, 0, NULL, SW_INVALIDATE | SW_ERASE );
    hdc = BeginPaint( hwnd_owndc, &ps );
    SetRectEmpty( &rect );
    GetClipBox( hdc, &rect );
    ok( rect.left >= 25 && rect.top >= 25 && rect.right <= 50 && rect.bottom <= 50,
        "invalid clip box %s\n", wine_dbgstr_rect( &rect ));
    EndPaint( hwnd_owndc, &ps );
}

static void test_invisible_create(void)
{
    HWND hwnd_owndc = CreateWindowA("owndc_class", NULL, WS_OVERLAPPED,
                                    0, 200, 100, 100,
                                    0, 0, GetModuleHandleA(0), NULL );
    HDC dc1, dc2;

    dc1 = GetDC(hwnd_owndc);
    dc2 = GetDC(hwnd_owndc);

    ok(dc1 == dc2, "expected owndc dcs to match\n");

    ReleaseDC(hwnd_owndc, dc2);
    ReleaseDC(hwnd_owndc, dc1);
    DestroyWindow(hwnd_owndc);
}

static void test_dc_layout(void)
{
    HWND hwnd_cache_rtl, hwnd_owndc_rtl, hwnd_classdc_rtl, hwnd_classdc2_rtl;
    HDC hdc;
    DWORD layout;

    hdc = GetDC( hwnd_cache );
    SetLayout( hdc, LAYOUT_RTL );
    layout = GetLayout( hdc );
    ReleaseDC( hwnd_cache, hdc );
    if (!layout)
    {
        win_skip( "SetLayout not supported\n" );
        return;
    }

    hwnd_cache_rtl = CreateWindowExA(WS_EX_LAYOUTRTL, "cache_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                     0, 0, 100, 100, 0, 0, GetModuleHandleA(0), NULL );
    hwnd_owndc_rtl = CreateWindowExA(WS_EX_LAYOUTRTL, "owndc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                     0, 200, 100, 100, 0, 0, GetModuleHandleA(0), NULL );
    hwnd_classdc_rtl = CreateWindowExA(WS_EX_LAYOUTRTL, "classdc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                       200, 0, 100, 100, 0, 0, GetModuleHandleA(0), NULL );
    hwnd_classdc2_rtl = CreateWindowExA(WS_EX_LAYOUTRTL, "classdc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                        200, 200, 100, 100, 0, 0, GetModuleHandleA(0), NULL );
    hdc = GetDC( hwnd_cache_rtl );
    layout = GetLayout( hdc );

    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    SetLayout( hdc, 0 );
    ReleaseDC( hwnd_cache_rtl, hdc );
    hdc = GetDC( hwnd_owndc_rtl );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    ReleaseDC( hwnd_cache_rtl, hdc );

    hdc = GetDC( hwnd_cache );
    layout = GetLayout( hdc );
    ok( layout == 0, "wrong layout %lx\n", layout );
    ReleaseDC( hwnd_cache, hdc );

    hdc = GetDC( hwnd_owndc_rtl );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    SetLayout( hdc, 0 );
    ReleaseDC( hwnd_owndc_rtl, hdc );
    hdc = GetDC( hwnd_owndc_rtl );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    ReleaseDC( hwnd_owndc_rtl, hdc );

    hdc = GetDC( hwnd_classdc_rtl );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    SetLayout( hdc, 0 );
    ReleaseDC( hwnd_classdc_rtl, hdc );
    hdc = GetDC( hwnd_classdc2_rtl );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    ReleaseDC( hwnd_classdc2_rtl, hdc );
    hdc = GetDC( hwnd_classdc );
    layout = GetLayout( hdc );
    ok( layout == LAYOUT_RTL, "wrong layout %lx\n", layout );
    ReleaseDC( hwnd_classdc_rtl, hdc );

    DestroyWindow(hwnd_classdc2_rtl);
    DestroyWindow(hwnd_classdc_rtl);
    DestroyWindow(hwnd_owndc_rtl);
    DestroyWindow(hwnd_cache_rtl);
}

static void test_destroyed_window(void)
{
    HDC dc, old_dc;
    int rop;

    dc = GetDC( hwnd_cache );
    SetROP2( dc, R2_WHITE );
    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) == hwnd_cache, "wrong window\n" );
    old_dc = dc;

    DestroyWindow( hwnd_cache );
    rop = GetROP2( dc );
    ok( rop == 0, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) != hwnd_cache, "wrong window\n" );
    ok( !ReleaseDC( hwnd_cache, dc ), "ReleaseDC succeeded\n" );
    dc = GetDC( hwnd_cache );
    ok( !dc, "Got a non-NULL DC (%p) for a destroyed window\n", dc );

    dc = GetDC( hwnd_classdc );
    SetROP2( dc, R2_WHITE );
    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) == hwnd_classdc, "wrong window\n" );
    old_dc = dc;

    dc = GetDC( hwnd_classdc2 );
    ok( old_dc == dc, "wrong DC\n" );
    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) == hwnd_classdc2, "wrong window\n" );
    DestroyWindow( hwnd_classdc2 );

    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) != hwnd_classdc2, "wrong window\n" );
    ok( !ReleaseDC( hwnd_classdc2, dc ), "ReleaseDC succeeded\n" );
    dc = GetDC( hwnd_classdc2 );
    ok( !dc, "Got a non-NULL DC (%p) for a destroyed window\n", dc );

    dc = GetDC( hwnd_classdc );
    ok( dc != 0, "Got NULL DC\n" );
    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) == hwnd_classdc, "wrong window\n" );
    DestroyWindow( hwnd_classdc );

    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) != hwnd_classdc, "wrong window\n" );
    ok( !ReleaseDC( hwnd_classdc, dc ), "ReleaseDC succeeded\n" );
    dc = GetDC( hwnd_classdc );
    ok( !dc, "Got a non-NULL DC (%p) for a destroyed window\n", dc );

    dc = GetDC( hwnd_owndc );
    ok( dc != 0, "Got NULL DC\n" );
    rop = GetROP2( dc );
    ok( rop == R2_WHITE, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) == hwnd_owndc, "wrong window\n" );
    DestroyWindow( hwnd_owndc );

    rop = GetROP2( dc );
    ok( rop == 0, "wrong ROP2 %d\n", rop );
    ok( WindowFromDC( dc ) != hwnd_owndc, "wrong window\n" );
    ok( !ReleaseDC( hwnd_owndc, dc ), "ReleaseDC succeeded\n" );
    dc = GetDC( hwnd_owndc );
    ok( !dc, "Got a non-NULL DC (%p) for a destroyed window\n", dc );

    DestroyWindow( hwnd_parent );
}

START_TEST(dce)
{
    char **argv;
    int argc = winetest_get_mainargs( &argv );
    WNDCLASSA cls;

    if (argc == 4 && !strcmp( argv[2], "detached_child_gdi" ))
    {
        HWND hwnd = NULL;

        ok( sscanf( argv[3], "%p", &hwnd ) == 1, "invalid window handle %s\n", argv[3] );
        if (hwnd) test_detached_gdi_child( hwnd, TRUE );
        return;
    }
    if (argc == 4 && !strcmp( argv[2], "direct_foreign_gdi" ))
    {
        HWND hwnd = NULL;

        ok( sscanf( argv[3], "%p", &hwnd ) == 1, "invalid window handle %s\n", argv[3] );
        if (hwnd) direct_foreign_gdi_proc( hwnd );
        return;
    }
    if (argc == 4 && !strcmp( argv[2], "stale_child_gdi" ))
    {
        HWND hwnd = NULL;

        ok( sscanf( argv[3], "%p", &hwnd ) == 1, "invalid window handle %s\n", argv[3] );
        if (hwnd) stale_child_gdi_proc( hwnd );
        return;
    }

    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = DefWindowProcA;
    cls.cbClsExtra = 0;
    cls.cbWndExtra = 0;
    cls.hInstance = GetModuleHandleA(0);
    cls.hIcon = 0;
    cls.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    cls.hbrBackground = GetStockObject(WHITE_BRUSH);
    cls.lpszMenuName = NULL;
    cls.lpszClassName = "cache_class";
    RegisterClassA(&cls);
    cls.style = CS_DBLCLKS | CS_OWNDC;
    cls.lpszClassName = "owndc_class";
    RegisterClassA(&cls);
    cls.style = CS_DBLCLKS | CS_CLASSDC;
    cls.lpszClassName = "classdc_class";
    RegisterClassA(&cls);
    cls.style = CS_PARENTDC;
    cls.lpszClassName = "parentdc_class";
    RegisterClassA(&cls);

    hwnd_cache = CreateWindowA("cache_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                               0, 0, 100, 100,
                               0, 0, GetModuleHandleA(0), NULL );
    hwnd_owndc = CreateWindowA("owndc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                               0, 200, 100, 100,
                               0, 0, GetModuleHandleA(0), NULL );
    hwnd_classdc = CreateWindowA("classdc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                 200, 0, 100, 100,
                                 0, 0, GetModuleHandleA(0), NULL );
    hwnd_classdc2 = CreateWindowA("classdc_class", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                  200, 200, 100, 100,
                                  0, 0, GetModuleHandleA(0), NULL );
    hwnd_parent = CreateWindowA("static", NULL, WS_OVERLAPPED | WS_VISIBLE,
                                400, 0, 100, 100, 0, 0, 0, NULL );
    hwnd_parentdc = CreateWindowA("parentdc_class", NULL, WS_CHILD | WS_VISIBLE,
                                  50, 50, 1, 1, hwnd_parent, 0, 0, NULL );

    test_dc_attributes();
    test_parameters();
    test_dc_visrgn();
    test_begin_paint();
    test_cropped_parentdc_paint_clipbox();
    test_scroll_window();
    test_invisible_create();
    test_dc_layout();
    test_detached_child_gdi( argv );
    test_direct_foreign_gdi( argv );
    test_stale_child_gdi( argv );
    /* this should be last */
    test_destroyed_window();
}
