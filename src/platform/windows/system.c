/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * This file is the starting point of the program and implements
 * several support functions and the main loop.
 *
 * =======================================================================
 */

#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <stdio.h>
#include <direct.h>
#include <io.h>
#include <conio.h>
#include <shlobj.h>

#include "../../common/header/common.h"
#include "../generic/header/input.h"
#include "header/resource.h"
#include "header/winquake.h"

#define MAX_NUM_ARGVS 128

int starttime;
qboolean ActiveApp;
qboolean Minimized;

static HANDLE hinput, houtput;
static HANDLE qwclsemaphore;

HINSTANCE global_hInstance;

unsigned int sys_msg_time;
unsigned int sys_frame_time;

static char console_text[256];
static int console_textlen;

char findbase[MAX_OSPATH];
char findpath[MAX_OSPATH];
int findhandle;

int argc;
char *argv[MAX_NUM_ARGVS];

qboolean is_portable;

/* ================================================================ */

void Sys_Error(char *error, ...)
{
  va_list argptr;
  char text[1024];

#ifndef DEDICATED_ONLY
  CL_Shutdown();
#endif

  Qcommon_Shutdown();

  va_start(argptr, error);
  vsprintf(text, error, argptr);
  va_end(argptr);
  fprintf(stderr, "Error: %s\n", text);

  MessageBox(NULL, text, "Error", 0 /* MB_OK */);

  if (qwclsemaphore) {
    CloseHandle(qwclsemaphore);
  }

  /* Close stdout and stderr */
#ifndef DEDICATED_ONLY
  fclose(stdout);
  fclose(stderr);
#endif

  exit(1);
}

void Sys_Quit(void)
{
  timeEndPeriod(1);

#ifndef DEDICATED_ONLY
  CL_Shutdown();
#endif

  Qcommon_Shutdown();
  CloseHandle(qwclsemaphore);

  if (dedicated && dedicated->value) {
    FreeConsole();
  }

  /* Close stdout and stderr */
#ifndef DEDICATED_ONLY
  fclose(stdout);
  fclose(stderr);
#endif

  exit(0);
}

/* ================================================================ */

void Sys_Init(void)
{
  OSVERSIONINFO vinfo;

  timeBeginPeriod(1);
  vinfo.dwOSVersionInfoSize = sizeof(vinfo);

  if (!GetVersionEx(&vinfo)) {
    Sys_Error("Couldn't get OS info");
  }

  if (dedicated->value) {
    AllocConsole();

    hinput = GetStdHandle(STD_INPUT_HANDLE);
    houtput = GetStdHandle(STD_OUTPUT_HANDLE);
  }
}

char *Sys_ConsoleInput(void)
{
  INPUT_RECORD recs[1024];
  int ch;
  DWORD dummy, numread, numevents;

  if (!dedicated || !dedicated->value) {
    return NULL;
  }

  for (;;) {
    if (!GetNumberOfConsoleInputEvents(hinput, &numevents)) {
      Sys_Error("Error getting # of console events");
    }

    if (numevents <= 0) {
      break;
    }

    if (!ReadConsoleInput(hinput, recs, 1, &numread)) {
      Sys_Error("Error reading console input");
    }

    if (numread != 1) {
      Sys_Error("Couldn't read console input");
    }

    if (recs[0].EventType == KEY_EVENT) {
      if (!recs[0].Event.KeyEvent.bKeyDown) {
        ch = recs[0].Event.KeyEvent.uChar.AsciiChar;

        switch (ch) {
        case '\r':
          WriteFile(houtput, "\r\n", 2, &dummy, NULL);

          if (console_textlen) {
            console_text[console_textlen] = 0;
            console_textlen = 0;
            return console_text;
          }

          break;

        case '\b':

          if (console_textlen) {
            console_textlen--;
            WriteFile(houtput, "\b \b", 3, &dummy, NULL);
          }

          break;

        default:

          if (ch >= ' ') {
            if (console_textlen < sizeof(console_text) - 2) {
              WriteFile(houtput, &ch, 1, &dummy, NULL);
              console_text[console_textlen] = ch;
              console_textlen++;
            }
          }

          break;
        }
      }
    }
  }

  return NULL;
}

void Sys_ConsoleOutput(char *string)
{
  char text[256];
  DWORD dummy;

  if (!dedicated || !dedicated->value) {
    fputs(string, stdout);
  } else {
    if (console_textlen) {
      text[0] = '\r';
      memset(&text[1], ' ', console_textlen);
      text[console_textlen + 1] = '\r';
      text[console_textlen + 2] = 0;
      WriteFile(houtput, text, console_textlen + 2, &dummy, NULL);
    }

    WriteFile(houtput, string, strlen(string), &dummy, NULL);

    if (console_textlen) {
      WriteFile(houtput, console_text, console_textlen, &dummy, NULL);
    }
  }
}

void Sys_SendKeyEvents(void)
{
#ifndef DEDICATED_ONLY
  input_update();
#endif

  /* grab frame time */
  sys_frame_time = timeGetTime();
}

/* ======================================================================= */

void ParseCommandLine(LPSTR lpCmdLine)
{
  argc = 1;
  argv[0] = "exe";

  while (*lpCmdLine && (argc < MAX_NUM_ARGVS)) {
    while (*lpCmdLine && ((*lpCmdLine <= 32) || (*lpCmdLine > 126))) {
      lpCmdLine++;
    }

    if (*lpCmdLine) {
      argv[argc] = lpCmdLine;
      argc++;

      while (*lpCmdLine && ((*lpCmdLine > 32) && (*lpCmdLine <= 126))) {
        lpCmdLine++;
      }

      if (*lpCmdLine) {
        *lpCmdLine = 0;
        lpCmdLine++;
      }
    }
  }
}

/* ======================================================================= */

long long Sys_Microseconds(void)
{
  long long microseconds;
  static long long uSecbase;

  FILETIME ft;
  unsigned long long tmpres = 0;

  GetSystemTimeAsFileTime(&ft);

  tmpres |= ft.dwHighDateTime;
  tmpres <<= 32;
  tmpres |= ft.dwLowDateTime;

  tmpres /= 10;                   // Convert to microseconds.
  tmpres -= 11644473600000000ULL; // ...and to unix epoch.

  microseconds = tmpres;

  if (!uSecbase) {
    uSecbase = microseconds - 1001ll;
  }

  return microseconds - uSecbase;
}

int Sys_Milliseconds(void)
{
  return (int) (Sys_Microseconds() / 1000ll);
}

void Sys_Sleep(int msec)
{
  Sleep(msec);
}

void Sys_Nanosleep(int nanosec)
{
  HANDLE timer;
  LARGE_INTEGER li;

  timer = CreateWaitableTimer(NULL, TRUE, NULL);

  // Windows has a max. resolution of 100ns.
  li.QuadPart = -nanosec / 100;

  SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
  WaitForSingleObject(timer, INFINITE);

  CloseHandle(timer);
}

/* ======================================================================= */

static qboolean CompareAttributes(unsigned found, unsigned musthave, unsigned canthave)
{
  if ((found & _A_RDONLY) && (canthave & SFF_RDONLY)) {
    return false;
  }

  if ((found & _A_HIDDEN) && (canthave & SFF_HIDDEN)) {
    return false;
  }

  if ((found & _A_SYSTEM) && (canthave & SFF_SYSTEM)) {
    return false;
  }

  if ((found & _A_SUBDIR) && (canthave & SFF_SUBDIR)) {
    return false;
  }

  if ((found & _A_ARCH) && (canthave & SFF_ARCH)) {
    return false;
  }

  if ((musthave & SFF_RDONLY) && !(found & _A_RDONLY)) {
    return false;
  }

  if ((musthave & SFF_HIDDEN) && !(found & _A_HIDDEN)) {
    return false;
  }

  if ((musthave & SFF_SYSTEM) && !(found & _A_SYSTEM)) {
    return false;
  }

  if ((musthave & SFF_SUBDIR) && !(found & _A_SUBDIR)) {
    return false;
  }

  if ((musthave & SFF_ARCH) && !(found & _A_ARCH)) {
    return false;
  }

  return true;
}

char *Sys_FindFirst(char *path, unsigned musthave, unsigned canthave)
{
  struct _finddata_t findinfo;

  if (findhandle) {
    Sys_Error("Sys_BeginFind without close");
  }

  findhandle = 0;

  COM_FilePath(path, findbase);
  findhandle = _findfirst(path, &findinfo);

  if (findhandle == -1) {
    return NULL;
  }

  if (!CompareAttributes(findinfo.attrib, musthave, canthave)) {
    return NULL;
  }

  Com_sprintf(findpath, sizeof(findpath), "%s/%s", findbase, findinfo.name);
  return findpath;
}

char *Sys_FindNext(unsigned musthave, unsigned canthave)
{
  struct _finddata_t findinfo;

  if (findhandle == -1) {
    return NULL;
  }

  if (_findnext(findhandle, &findinfo) == -1) {
    return NULL;
  }

  if (!CompareAttributes(findinfo.attrib, musthave, canthave)) {
    return NULL;
  }

  Com_sprintf(findpath, sizeof(findpath), "%s/%s", findbase, findinfo.name);
  return findpath;
}

void Sys_FindClose(void)
{
  if (findhandle != -1) {
    _findclose(findhandle);
  }

  findhandle = 0;
}

void Sys_Mkdir(char *path)
{
  _mkdir(path);
}

char *Sys_GetCurrentDirectory(void)
{
  static char dir[MAX_OSPATH];

  if (!_getcwd(dir, sizeof(dir))) {
    Sys_Error("Couldn't get current working directory");
  }

  return dir;
}

char *Sys_GetHomeDir(void)
{
  char *cur;
  char *old;
  char profile[MAX_PATH];
  int len;
  static char gdir[MAX_OSPATH];
  WCHAR sprofile[MAX_PATH];
  WCHAR uprofile[MAX_PATH];

  /* The following lines implement a horrible
     hack to connect the UTF-16 WinAPI to the
     ASCII Quake II. While this should work in
     most cases, it'll fail if the "Windows to
     DOS filename translation" is switched off.
     In that case the function will return NULL
     and no homedir is used. */

  /* Get the path to "My Documents" directory */
  SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, uprofile);

  /* Create a UTF-16 DOS path */
  len = GetShortPathNameW(uprofile, sprofile, sizeof(sprofile));

  if (len == 0) {
    return NULL;
  }

  /* Since the DOS path contains no UTF-16 characters, just convert it to ASCII
   */
  WideCharToMultiByte(CP_ACP, 0, sprofile, -1, profile, sizeof(profile), NULL, NULL);

  if (len == 0) {
    return NULL;
  }

  /* Check if path is too long */
  if ((len + strlen(CFGDIR) + 3) >= 256) {
    return NULL;
  }

  /* Replace backslashes by slashes */
  cur = old = profile;

  if (strstr(cur, "\\") != NULL) {
    while (cur != NULL) {
      if ((cur - old) > 1) {
        *cur = '/';
      }

      old = cur;
      cur = strchr(old + 1, '\\');
    }
  }

  snprintf(gdir, sizeof(gdir), "%s/%s/", profile, CFGDIR);

  return gdir;
}

void Sys_RedirectStdout(void)
{
  char *cur;
  char *old;
  char dir[MAX_OSPATH];
  char path_stdout[MAX_OSPATH];
  char path_stderr[MAX_OSPATH];
  const char *tmp;

  if (is_portable) {
    tmp = Sys_GetBinaryDir();
    Q_strlcpy(dir, tmp, sizeof(dir));
  } else {
    tmp = Sys_GetHomeDir();
    Q_strlcpy(dir, tmp, sizeof(dir));
  }

  if (dir == NULL) {
    return;
  }

  cur = old = dir;

  while (cur != NULL) {
    if ((cur - old) > 1) {
      *cur = '\0';
      Sys_Mkdir(dir);
      *cur = '/';
    }

    old = cur;
    cur = strchr(old + 1, '/');
  }

  snprintf(path_stdout, sizeof(path_stdout), "%s/%s", dir, "stdout.txt");
  snprintf(path_stderr, sizeof(path_stderr), "%s/%s", dir, "stderr.txt");

  freopen(path_stdout, "w", stdout);
  freopen(path_stderr, "w", stderr);
}

/* ======================================================================= */

typedef enum YQ2_PROCESS_DPI_AWARENESS
{
  YQ2_PROCESS_DPI_UNAWARE = 0,
  YQ2_PROCESS_SYSTEM_DPI_AWARE = 1,
  YQ2_PROCESS_PER_MONITOR_DPI_AWARE = 2
} YQ2_PROCESS_DPI_AWARENESS;

void Sys_SetHighDPIMode(void)
{
  /* For Vista, Win7 and Win8 */
  BOOL(WINAPI * SetProcessDPIAware)(void) = NULL;

  /* Win8.1 and later */
  HRESULT(WINAPI * SetProcessDpiAwareness)
  (YQ2_PROCESS_DPI_AWARENESS dpiAwareness) = NULL;

  HINSTANCE userDLL = LoadLibrary("USER32.DLL");

  if (userDLL) {
    SetProcessDPIAware = (BOOL(WINAPI *)(void)) GetProcAddress(userDLL, "SetProcessDPIAware");
  }

  HINSTANCE shcoreDLL = LoadLibrary("SHCORE.DLL");

  if (shcoreDLL) {
    SetProcessDpiAwareness =
        (HRESULT(WINAPI *)(YQ2_PROCESS_DPI_AWARENESS)) GetProcAddress(shcoreDLL, "SetProcessDpiAwareness");
  }

  if (SetProcessDpiAwareness) {
    SetProcessDpiAwareness(YQ2_PROCESS_PER_MONITOR_DPI_AWARE);
  } else if (SetProcessDPIAware) {
    SetProcessDPIAware();
  }
}

/* ======================================================================= */

/*
 * Windows main function. Containts the
 * initialization code and the main loop
 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  MSG msg;
  long long oldtime, newtime;

  /* Previous instances do not exist in Win32 */
  if (hPrevInstance) {
    return 0;
  }

  /* Make the current instance global */
  global_hInstance = hInstance;

  /* Setup FPU if necessary */
  Sys_SetupFPU();

  /* Force DPI awareness */
  Sys_SetHighDPIMode();

  /* Parse the command line arguments */
  ParseCommandLine(lpCmdLine);

  /* Are we portable? */
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-portable") == 0) {
      is_portable = true;
    }
  }

/* Need to redirect stdout before anything happens. */
#ifndef DEDICATED_ONLY
  Sys_RedirectStdout();
#endif

  /* Seed PRNG */
  randk_seed();

  /* Call the initialization code */
  Qcommon_Init(argc, argv);

  /* Save our time */
  oldtime = Sys_Microseconds();

  /* The legendary main loop */
  while (1) {
    /* If at a full screen console, don't update unless needed */
    if (Minimized || (dedicated && dedicated->value)) {
      Sleep(1);
    }

    while (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
      if (!GetMessage(&msg, NULL, 0, 0)) {
        Com_Quit();
      }

      sys_msg_time = msg.time;
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    // Throttle the game a little bit
    Sys_Nanosleep(5000);

    newtime = Sys_Microseconds();
    Qcommon_Frame(newtime - oldtime);
    oldtime = newtime;
  }

  /* never gets here */
  return TRUE;
}
