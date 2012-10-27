/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sys_win.c -- Win32 system interface code

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <conio.h>

#include "client.h"
#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "resource.h"
#include "screen.h"
#include "sys.h"
#include "winquake.h"

#define MINIMUM_WIN_MEMORY	0x0c00000
#define MAXIMUM_WIN_MEMORY	0x1000000

#define PAUSE_SLEEP	50	// sleep time on pause or minimization
#define NOT_FOCUS_SLEEP	20	// sleep time when not focus

int starttime;
qboolean ActiveApp;
qboolean WinNT;

static double timer_pfreq;
static int timer_lowshift;
static unsigned int timer_oldtime;
static qboolean timer_fallback;
static DWORD timer_fallback_start;

HWND hwnd_dialog;		// startup dialog box
HANDLE qwclsemaphore;

static HANDLE tevent;

void MaskExceptions(void);
void Sys_PopFPCW(void);
void Sys_PushFPCW_SetHigh(void);

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    static char data[MAX_PRINTMSG];
    int fd;

    va_start(argptr, fmt);
    vsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
};

/*
===============================================================================

FILE IO

===============================================================================
*/

int
Sys_FileTime(const char *path)
{
    FILE *f;
    int t, retval;

    t = VID_ForceUnlockedAndReturnState();

    f = fopen(path, "rb");

    if (f) {
	fclose(f);
	retval = 1;
    } else {
	retval = -1;
    }

    VID_ForceLockState(t);
    return retval;
}

void
Sys_mkdir(const char *path)
{
    _mkdir(path);
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

/*
================
Sys_MakeCodeWriteable
================
*/
void
Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    DWORD flOldProtect;

//@@@ copy on write or just read-write?
    if (!VirtualProtect
	((LPVOID)startaddr, length, PAGE_READWRITE, &flOldProtect))
	Sys_Error("Protection change failed");
}


static void
Sys_InitTimers(void)
{
    LARGE_INTEGER freq, pcount;
    unsigned int lowpart, highpart;

    MaskExceptions();
    Sys_SetFPCW();

    if (!QueryPerformanceFrequency(&freq)) {
	Con_Printf("WARNING: No hardware timer available, using fallback\n");
	timer_fallback = true;
	timer_fallback_start = timeGetTime();
	return;
    }

    /*
     * get 32 out of the 64 time bits such that we have around
     * 1 microsecond resolution
     */
    lowpart = (unsigned int)freq.LowPart;
    highpart = (unsigned int)freq.HighPart;
    timer_lowshift = 0;

    while (highpart || (lowpart > 2000000.0)) {
	timer_lowshift++;
	lowpart >>= 1;
	lowpart |= (highpart & 1) << 31;
	highpart >>= 1;
    }
    timer_pfreq = 1.0 / (double)lowpart;

    /* Do first time initialisation */
    Sys_PushFPCW_SetHigh();
    QueryPerformanceCounter(&pcount);
    timer_oldtime = (unsigned int)pcount.LowPart >> timer_lowshift;
    timer_oldtime |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);
    Sys_PopFPCW();
}

/*
================
Sys_Init
================
*/
void
Sys_Init(void)
{
    OSVERSIONINFO vinfo;

#ifndef SERVERONLY
    // allocate a named semaphore on the client so the
    // front end can tell if it is alive

    // mutex will fail if semephore allready exists
    qwclsemaphore = CreateMutex(NULL,	/* Security attributes */
				0,	/* owner       */
				"qwcl");	/* Semaphore name      */
    if (!qwclsemaphore)
	Sys_Error("QWCL is already running on this system");
    CloseHandle(qwclsemaphore);

    qwclsemaphore = CreateSemaphore(NULL,	/* Security attributes */
				    0,	/* Initial count       */
				    1,	/* Maximum count       */
				    "qwcl");	/* Semaphore name      */
#endif

    MaskExceptions();
    Sys_SetFPCW();

    // make sure the timer is high precision, otherwise
    // NT gets 18ms resolution
    timeBeginPeriod(1);

    vinfo.dwOSVersionInfoSize = sizeof(vinfo);

    if (!GetVersionEx(&vinfo))
	Sys_Error("Couldn't get OS info");

    if ((vinfo.dwMajorVersion < 4) ||
	(vinfo.dwPlatformId == VER_PLATFORM_WIN32s)) {
	Sys_Error("QuakeWorld requires at least Win95 or NT 4.0");
    }

    if (vinfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
	WinNT = true;
    else
	WinNT = false;
}


void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];

    Host_Shutdown();

    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

    MessageBox(NULL, text, "Error", 0 /* MB_OK */ );

#ifndef SERVERONLY
    CloseHandle(qwclsemaphore);
#endif

    exit(1);
}

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;

    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
}

void
Sys_Quit(void)
{
    VID_ForceUnlockedAndReturnState();

    Host_Shutdown();
#ifndef SERVERONLY
    if (tevent)
	CloseHandle(tevent);

    if (qwclsemaphore)
	CloseHandle(qwclsemaphore);
#endif

    exit(0);
}

double
Sys_DoubleTime(void)
{
    static double curtime = 0.0;
    static double lastcurtime = 0.0;
    static int sametimecount;

    LARGE_INTEGER pcount;
    unsigned int temp, t2;
    double time;

    if (timer_fallback) {
	DWORD now = timeGetTime();
	if (now < timer_fallback_start)	/* wrapped */
	    return (now + (LONG_MAX - timer_fallback_start)) / 1000.0;
	return (now - timer_fallback_start) / 1000.0;
    }

    Sys_PushFPCW_SetHigh();

    QueryPerformanceCounter(&pcount);

    temp = (unsigned int)pcount.LowPart >> timer_lowshift;
    temp |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);

    /* check for turnover or backward time */
    if ((temp <= timer_oldtime) && ((timer_oldtime - temp) < 0x10000000)) {
	timer_oldtime = temp;	/* so we don't get stuck */
    } else {
	t2 = temp - timer_oldtime;
	time = (double)t2 * timer_pfreq;
	timer_oldtime = temp;
	curtime += time;
	if (curtime == lastcurtime) {
	    sametimecount++;
	    if (sametimecount > 100000) {
		curtime += 1.0;
		sametimecount = 0;
	    }
	} else {
	    sametimecount = 0;
	}
	lastcurtime = curtime;
    }

    Sys_PopFPCW();

    return curtime;
}

void
Sys_Sleep(void)
{
}


void
Sys_SendKeyEvents(void)
{
    MSG msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
	// we always update if there are any event, even if we're paused
	scr_skipupdate = 0;

	if (!GetMessage(&msg, NULL, 0, 0))
	    Sys_Quit();
	TranslateMessage(&msg);
	DispatchMessage(&msg);
    }
}



/*
==============================================================================

 WINDOWS CRAP

==============================================================================
*/

/*
==================
WinMain
==================
*/
void
SleepUntilInput(int time)
{

    MsgWaitForMultipleObjects(1, &tevent, FALSE, time, QS_ALLINPUT);
}



/*
==================
WinMain
==================
*/
HINSTANCE global_hInstance;
int global_nCmdShow;
char *argv[MAX_NUM_ARGVS];
static char *empty_string = "";
HWND hwnd_dialog;


int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
	int nCmdShow)
{
    quakeparms_t parms;
    double time, oldtime, newtime;
    MEMORYSTATUS lpBuffer;
    static char cwd[1024];
    int t;
    RECT rect;

    /* previous instances do not exist in Win32 */
    if (hPrevInstance)
	return 0;

    global_hInstance = hInstance;
    global_nCmdShow = nCmdShow;

    lpBuffer.dwLength = sizeof(MEMORYSTATUS);
    GlobalMemoryStatus(&lpBuffer);

    if (!GetCurrentDirectory(sizeof(cwd), cwd))
	Sys_Error("Couldn't determine current directory");

    if (cwd[strlen(cwd) - 1] == '/')
	cwd[strlen(cwd) - 1] = 0;

    parms.basedir = cwd;
    parms.cachedir = NULL;

    parms.argc = 1;
    argv[0] = empty_string;

    while (*lpCmdLine && (parms.argc < MAX_NUM_ARGVS)) {
	while (*lpCmdLine && ((*lpCmdLine <= 32) || (*lpCmdLine > 126)))
	    lpCmdLine++;

	if (*lpCmdLine) {
	    argv[parms.argc] = lpCmdLine;
	    parms.argc++;

	    while (*lpCmdLine && ((*lpCmdLine > 32) && (*lpCmdLine <= 126)))
		lpCmdLine++;

	    if (*lpCmdLine) {
		*lpCmdLine = 0;
		lpCmdLine++;
	    }

	}
    }

    parms.argv = argv;

    COM_InitArgv(parms.argc, parms.argv);

    parms.argc = com_argc;
    parms.argv = com_argv;

    hwnd_dialog =
	CreateDialog(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, NULL);

    if (hwnd_dialog) {
	if (GetWindowRect(hwnd_dialog, &rect)) {
	    if (rect.left > (rect.top * 2)) {
		SetWindowPos(hwnd_dialog, 0,
			     (rect.left / 2) -
			     ((rect.right - rect.left) / 2), rect.top, 0, 0,
			     SWP_NOZORDER | SWP_NOSIZE);
	    }
	}

	ShowWindow(hwnd_dialog, SW_SHOWDEFAULT);
	UpdateWindow(hwnd_dialog);
	SetForegroundWindow(hwnd_dialog);
    }
// take the greater of all the available memory or half the total memory,
// but at least 8 Mb and no more than 16 Mb, unless they explicitly
// request otherwise
    parms.memsize = lpBuffer.dwAvailPhys;

    if (parms.memsize < MINIMUM_WIN_MEMORY)
	parms.memsize = MINIMUM_WIN_MEMORY;

    if (parms.memsize < (lpBuffer.dwTotalPhys >> 1))
	parms.memsize = lpBuffer.dwTotalPhys >> 1;

    if (parms.memsize > MAXIMUM_WIN_MEMORY)
	parms.memsize = MAXIMUM_WIN_MEMORY;

    if (COM_CheckParm("-heapsize")) {
	t = COM_CheckParm("-heapsize") + 1;

	if (t < com_argc)
	    parms.memsize = Q_atoi(com_argv[t]) * 1024;
    }

    parms.membase = malloc(parms.memsize);

    if (!parms.membase)
	Sys_Error("Not enough memory free; check disk space");

    tevent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (!tevent)
	Sys_Error("Couldn't create event");

    Sys_Init();
    Sys_InitTimers();

// because sound is off until we become active
    S_BlockSound();

    Sys_Printf("Host_Init\n");
    Host_Init(&parms);

    oldtime = Sys_DoubleTime();

    /* main window message loop */
    while (1) {
	// yield the CPU for a little while when paused, minimized, or not the focus
	if ((cl.paused && (!ActiveApp && !DDActive)) || !window_visible()
	    || block_drawing) {
	    SleepUntilInput(PAUSE_SLEEP);
	    scr_skipupdate = 1;	// no point in bothering to draw
	} else if (!ActiveApp && !DDActive) {
	    SleepUntilInput(NOT_FOCUS_SLEEP);
	}

	newtime = Sys_DoubleTime();
	time = newtime - oldtime;
	Host_Frame(time);
	oldtime = newtime;
    }

    /* return success of application */
    return TRUE;
}

#ifndef USE_X86_ASM
void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}

void
Sys_SetFPCW(void)
{
}

void
Sys_PushFPCW_SetHigh(void)
{
}

void
Sys_PopFPCW(void)
{
}

void
MaskExceptions(void)
{
}
#endif
