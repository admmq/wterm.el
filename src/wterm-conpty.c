/* wterm-conpty.c -- bridge between plain pipes and a Windows pseudo console.

   Emacs on Windows cannot allocate a pty, so it talks to this helper over
   ordinary pipes.  The helper creates a ConPTY, runs the requested command
   inside it and shuttles bytes in both directions:

     stdin  (from Emacs) -> pseudo console input
     pseudo console output -> stdout (to Emacs)

   Control messages are embedded in the input stream.  They start with the
   byte 0xFF, which never occurs in UTF-8 text:

     0xFF 'R' <rows> ';' <cols> '\n'     resize the pseudo console

   Usage: wterm-conpty.exe ROWS COLS COMMAND-LINE

   COMMAND-LINE is passed verbatim to CreateProcessW.  The helper exits with
   the exit code of the command.  */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef VOID *HPCON_T;
typedef HRESULT (WINAPI *CreatePseudoConsole_t) (COORD, HANDLE, HANDLE, DWORD,
                                                 HPCON_T *);
typedef HRESULT (WINAPI *ResizePseudoConsole_t) (HPCON_T, COORD);
typedef VOID (WINAPI *ClosePseudoConsole_t) (HPCON_T);

static CreatePseudoConsole_t pCreatePseudoConsole;
static ResizePseudoConsole_t pResizePseudoConsole;
static ClosePseudoConsole_t pClosePseudoConsole;

static HPCON_T hpc;
static HANDLE pty_in;           /* we write here; the console reads it */
static HANDLE pty_out;          /* the console writes; we read here */
static HANDLE child_process;
static CRITICAL_SECTION close_lock;
static BOOL pty_closed;

static void
die (const char *msg)
{
  DWORD n;
  HANDLE err = GetStdHandle (STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE)
    {
      WriteFile (err, "wterm-conpty: ", 14, &n, NULL);
      WriteFile (err, msg, (DWORD) strlen (msg), &n, NULL);
      WriteFile (err, "\r\n", 2, &n, NULL);
    }
  ExitProcess (127);
}

static void
close_pty (void)
{
  EnterCriticalSection (&close_lock);
  if (!pty_closed)
    {
      pty_closed = TRUE;
      /* Closing the pseudo console terminates the console clients still
         attached to it and makes the output pipe report EOF.  */
      pClosePseudoConsole (hpc);
    }
  LeaveCriticalSection (&close_lock);
}

static BOOL
write_all (HANDLE h, const char *buf, DWORD len)
{
  while (len > 0)
    {
      DWORD n;
      if (!WriteFile (h, buf, len, &n, NULL))
        return FALSE;
      buf += n;
      len -= n;
    }
  return TRUE;
}

/* Output batching.  ConPTY writes about one line at a time, and every
   write that reaches Emacs costs a filter call and a redisplay.  While a
   program is streaming, output is gathered for a few milliseconds so Emacs
   gets fewer, larger chunks.  The reply to a key often comes in a few
   quick pieces (PowerShell hides the cursor, then draws the line); those
   must pass at once, so gathering only starts after several writes in a
   row that came less than BURST_GAP_US apart.  */
#define BURST_GAP_US 5000       /* writes closer than this are "in a row" */
#define BURST_WRITES 4          /* writes in a row before gathering */
#define GATHER_US 4000          /* how long to gather while streaming */

static HANDLE gather_timer;
static LARGE_INTEGER qpc_freq;

static LONGLONG
now_us (void)
{
  LARGE_INTEGER t;
  QueryPerformanceCounter (&t);
  return t.QuadPart * 1000000 / qpc_freq.QuadPart;
}

static void
sleep_us (LONGLONG us)
{
  LARGE_INTEGER due;
  due.QuadPart = -us * 10;
  if (gather_timer && SetWaitableTimer (gather_timer, &due, 0, NULL, NULL,
                                        FALSE))
    WaitForSingleObject (gather_timer, INFINITE);
  else
    Sleep (1);
}

/* Append whatever is waiting in the pipe to BUF; return the new length.  */
static DWORD
read_available (char *buf, DWORD len, DWORD size)
{
  DWORD avail, n;
  while (len < size && PeekNamedPipe (pty_out, NULL, 0, NULL, &avail, NULL)
         && avail > 0)
    {
      if (!ReadFile (pty_out, buf + len, avail < size - len ? avail
                     : size - len, &n, NULL) || n == 0)
        break;
      len += n;
    }
  return len;
}

/* Pseudo console output -> stdout.  */
static DWORD WINAPI
output_thread (LPVOID arg)
{
  HANDLE out = GetStdHandle (STD_OUTPUT_HANDLE);
  static char buf[65536];
  LONGLONG last_write = 0;
  int in_a_row = 0;
  (void) arg;

  QueryPerformanceFrequency (&qpc_freq);
  /* A high resolution timer allows sub-millisecond waits without changing
     the system timer resolution (Windows 10 1803+).  */
  gather_timer = CreateWaitableTimerExW (NULL, NULL,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  for (;;)
    {
      DWORD n, total;
      LONGLONG start;
      if (!ReadFile (pty_out, buf, sizeof buf, &n, NULL) || n == 0)
        break;
      total = read_available (buf, n, sizeof buf);
      start = now_us ();
      in_a_row = start - last_write < BURST_GAP_US ? in_a_row + 1 : 0;
      if (in_a_row >= BURST_WRITES)
        while (total < sizeof buf && now_us () - start < GATHER_US)
          {
            sleep_us (500);
            total = read_available (buf, total, sizeof buf);
          }
      if (!write_all (out, buf, total))
        break;
      last_write = now_us ();
    }
  return 0;
}

static void
handle_control (const char *msg)
{
  if (msg[0] == 'R')
    {
      int rows = atoi (msg + 1);
      const char *semi = strchr (msg, ';');
      int cols = semi ? atoi (semi + 1) : 0;
      if (rows > 0 && cols > 0 && rows < 32768 && cols < 32768)
        {
          COORD size = { (SHORT) cols, (SHORT) rows };
          pResizePseudoConsole (hpc, size);
        }
    }
}

/* stdin -> pseudo console input, filtering out control messages.  */
static DWORD WINAPI
input_thread (LPVOID arg)
{
  HANDLE in = GetStdHandle (STD_INPUT_HANDLE);
  static char buf[16384];
  static char out[16384];
  char ctl[64];
  int ctl_len = -1;             /* -1: not inside a control message */
  (void) arg;

  for (;;)
    {
      DWORD n, i, o = 0;
      if (!ReadFile (in, buf, sizeof buf, &n, NULL) || n == 0)
        break;
      for (i = 0; i < n; i++)
        {
          char c = buf[i];
          if (ctl_len >= 0)
            {
              if (c == '\n')
                {
                  ctl[ctl_len] = '\0';
                  handle_control (ctl);
                  ctl_len = -1;
                }
              else if (ctl_len < (int) sizeof ctl - 1)
                ctl[ctl_len++] = c;
            }
          else if ((unsigned char) c == 0xFF)
            {
              if (o > 0 && !write_all (pty_in, out, o))
                goto done;
              o = 0;
              ctl_len = 0;
            }
          else
            out[o++] = c;
        }
      if (o > 0 && !write_all (pty_in, out, o))
        break;
    }
 done:
  /* Emacs closed our stdin: the terminal is gone.  */
  close_pty ();
  if (child_process)
    TerminateProcess (child_process, 1);
  return 0;
}

static BOOL WINAPI
ctrl_handler (DWORD type)
{
  switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      /* `interrupt-process' from Emacs: forward a ^C to the terminal.  */
      {
        DWORD n;
        WriteFile (pty_in, "\x03", 1, &n, NULL);
      }
      return TRUE;
    default:
      /* CTRL_CLOSE_EVENT etc.: `delete-process' or logoff.  */
      close_pty ();
      if (child_process)
        TerminateProcess (child_process, 1);
      return FALSE;
    }
}

int
main (void)
{
  int argc;
  LPWSTR *argv = CommandLineToArgvW (GetCommandLineW (), &argc);
  HMODULE kernel32;
  HANDLE in_read, out_write;
  COORD size;
  HRESULT hr;
  STARTUPINFOEXW si;
  PROCESS_INFORMATION pi;
  SIZE_T attr_size = 0;
  LPWSTR cmdline;
  HANDLE threads[2];
  DWORD exit_code = 0;

  if (!argv || argc != 4)
    die ("usage: wterm-conpty ROWS COLS COMMAND-LINE");

  size.Y = (SHORT) _wtoi (argv[1]);
  size.X = (SHORT) _wtoi (argv[2]);
  if (size.X <= 0 || size.Y <= 0)
    die ("invalid terminal size");

  /* Use the ConPTY built into Windows.  It is looked up at run time so
     that older systems get a clear message instead of a loader error.  */
  kernel32 = GetModuleHandleW (L"kernel32.dll");
  pCreatePseudoConsole = (CreatePseudoConsole_t) (void (*) (void))
    GetProcAddress (kernel32, "CreatePseudoConsole");
  pResizePseudoConsole = (ResizePseudoConsole_t) (void (*) (void))
    GetProcAddress (kernel32, "ResizePseudoConsole");
  pClosePseudoConsole = (ClosePseudoConsole_t) (void (*) (void))
    GetProcAddress (kernel32, "ClosePseudoConsole");
  if (!pCreatePseudoConsole || !pResizePseudoConsole || !pClosePseudoConsole)
    die ("ConPTY is not available (Windows 10 1809 or newer is required)");

  InitializeCriticalSection (&close_lock);

  /* A large output pipe lets the console keep writing while we gather
     output or wait for Emacs to take it.  */
  if (!CreatePipe (&in_read, &pty_in, NULL, 0)
      || !CreatePipe (&pty_out, &out_write, NULL, 1 << 20))
    die ("CreatePipe failed");

  hr = pCreatePseudoConsole (size, in_read, out_write, 0, &hpc);
  if (FAILED (hr))
    die ("CreatePseudoConsole failed");
  /* The pseudo console holds its own duplicates of these.  */
  CloseHandle (in_read);
  CloseHandle (out_write);

  /* Emacs starts us with CREATE_NEW_PROCESS_GROUP, which disables Ctrl-C
     handling and would be inherited by the shell.  Re-enable it so that ^C
     typed in the terminal interrupts programs as expected.  */
  SetConsoleCtrlHandler (NULL, FALSE);
  SetConsoleCtrlHandler (ctrl_handler, TRUE);

  memset (&si, 0, sizeof si);
  si.StartupInfo.cb = sizeof si;
  /* Without this the child can pick up our redirected std handles (the
     pipes to Emacs) instead of the pseudo console.  */
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
  si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
  si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

  InitializeProcThreadAttributeList (NULL, 1, 0, &attr_size);
  si.lpAttributeList = HeapAlloc (GetProcessHeap (), 0, attr_size);
  if (!si.lpAttributeList
      || !InitializeProcThreadAttributeList (si.lpAttributeList, 1, 0,
                                             &attr_size)
      || !UpdateProcThreadAttribute (si.lpAttributeList, 0,
                                     PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                     hpc, sizeof hpc, NULL, NULL))
    die ("could not set up the process attribute list");

  cmdline = _wcsdup (argv[3]);
  if (!CreateProcessW (NULL, cmdline, NULL, NULL, FALSE,
                       EXTENDED_STARTUPINFO_PRESENT
                       | CREATE_UNICODE_ENVIRONMENT,
                       NULL, NULL, &si.StartupInfo, &pi))
    {
      char msg[512];
      int len = WideCharToMultiByte (CP_UTF8, 0, argv[3], -1, msg + 20,
                                     sizeof msg - 21, NULL, NULL);
      memcpy (msg, "cannot run command: ", 20);
      if (len <= 0)
        msg[20] = '\0';
      die (msg);
    }
  child_process = pi.hProcess;
  CloseHandle (pi.hThread);

  threads[0] = CreateThread (NULL, 0, output_thread, NULL, 0, NULL);
  threads[1] = CreateThread (NULL, 0, input_thread, NULL, 0, NULL);

  WaitForSingleObject (pi.hProcess, INFINITE);
  GetExitCodeProcess (pi.hProcess, &exit_code);

  /* Flush remaining output: closing the console makes the output thread
     see EOF once everything has been read.  */
  close_pty ();
  WaitForSingleObject (threads[0], 3000);
  ExitProcess (exit_code);
}
