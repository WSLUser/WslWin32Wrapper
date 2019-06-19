/*
 * Includes
 */
#include "stdafx.h"

/*
 * Defines
 */
// Uncomment to enable extra debug print statements
//#define DEBUG

#ifdef DEBUG
#define DEBUG_PRINT(x) wprintf(L"[DEBUG] %s\n", x)
#else
#define DEBUG_PRINT(x)
#endif

#define WSL_EXEC L"wsl.exe"
#define OUTBUF_SIZE 4096

/*
 * Global Vars
 */
//static HANDLE				parent_stdin = GetStdHandle(STD_INPUT_HANDLE);
//static HANDLE				stdi_thread;
//static bool				stdithread_open;
static HANDLE				stdo_thread;
static HANDLE				handle_out;
static HANDLE				handle_in;
static HANDLE				wrapper_stdout;
static HANDLE				wrapper_stdin;
static HANDLE				conpty_stdout;
static HANDLE				conpty_stdin;
static HANDLE				parent_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
static bool					conpty_open;
static bool					stdothread_open;
static HPCON				wsl_conpty;
static STARTUPINFOEXW		startup_info_ex;
static PROCESS_INFORMATION	process_info;

/*
 * Function Decs
 */
//static void __cdecl _ProcessConPTYStdi(LPVOID context);
static void __cdecl	_ProcessConPTYStdo(LPVOID context);
static void			_EnableConsoleVTProcessing(void);
static COORD		_GetConPTYDimen(void);
static void			_InitConPTYAndPipes(void);
static void			_InitConPTYStartupInfo(void);
static void			_WslExec(std::wstring* command_str);
static void			_CloseHandles(void);
static void			_ExitError(const WCHAR* errorstr);

/*
 * Function Defs
 */
int wmain(const int argc, const WCHAR** argv)
{
	// Initial check
	if (argc <= 1) _ExitError(L"No arguments supplied");

	// Function variables
	int i;
	std::wstring command_str = std::wstring(WSL_EXEC);

	// Convert supplied arguments to string for use in _WslExec
	for (i = 1; i < argc; ++i) {
		command_str.append(L" ");
		command_str.append(argv[i]);
	}
	DEBUG_PRINT(command_str.c_str());

	// Setup and create pseudo console
	_EnableConsoleVTProcessing();
	_InitConPTYAndPipes();
	_InitConPTYStartupInfo();

	// Execute command and handle stdio via ConPTY!
	_WslExec(&command_str);

	return EXIT_SUCCESS;
}

static void _EnableConsoleVTProcessing(void)
{
	DEBUG_PRINT(L"Getting console mode and enabling VT processing");
	DWORD console_mode;

	// Enable console VT processing
	if (!GetConsoleMode(parent_stdout, &console_mode))
		_ExitError(L"Unable to get console stdout mode");
	if (!SetConsoleMode(parent_stdout, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
		_ExitError(L"Unable to set console stdin mode");

	//if (!GetConsoleMode(parent_stdin, &console_mode))
	//	_ExitError(L"Unable to get console stdin mode");
	//if (!SetConsoleMode(parent_stdin, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
	//	_ExitError(L"Unable to set console stdin mode");
}

static COORD _GetConPTYDimen(void)
{
	COORD con_size;
	CONSOLE_SCREEN_BUFFER_INFO con_buf_info;

	DEBUG_PRINT(L"Getting console screen buffer info...");
	if (!GetConsoleScreenBufferInfo(parent_stdout, &con_buf_info))
		_ExitError(L"Unable to get console screen buffer info");

	con_size.X = con_buf_info.srWindow.Right - con_buf_info.srWindow.Left + 1;
	con_size.Y = con_buf_info.srWindow.Bottom - con_buf_info.srWindow.Top + 1;

#ifdef DEBUG
	WCHAR debug_str[16];
	swprintf_s(debug_str, L"X=%i, Y=%i", con_size.X, con_size.Y);
	DEBUG_PRINT(debug_str);
#endif

	return con_size;
}

static void _InitConPTYAndPipes(void)
{
	// Create the in/out pipes
	DEBUG_PRINT(L"Creating pipes...");
	conpty_stdin = INVALID_HANDLE_VALUE;
	conpty_stdout = INVALID_HANDLE_VALUE;
	if (!CreatePipe(&conpty_stdin, &wrapper_stdout, NULL, 0) ||
		!CreatePipe(&wrapper_stdin, &conpty_stdout, NULL, 0)
		) _ExitError(L"Unable to create pipes");

	// Create ConPTY using pipes
	DEBUG_PRINT(L"Creating ConPTY...");
	if (CreatePseudoConsole(
		_GetConPTYDimen(),	// Initial dimensions
		conpty_stdin,
		conpty_stdout,
		0,
		&wsl_conpty
	)) _ExitError(L"Unable to create ConPTY");
	conpty_open = TRUE;

	// Can close handles to PTY end of pipes here as handles dupe'd into
	// ConHost and released after ConPTY destroyed
	if (conpty_stdin == INVALID_HANDLE_VALUE ||
		conpty_stdout == INVALID_HANDLE_VALUE)
		_ExitError(L"ConPTY pipe handle(s) invalid");
	else {
		CloseHandle(conpty_stdin);
		CloseHandle(conpty_stdout);
	}
}

static void _InitConPTYStartupInfo(void)
{
	DEBUG_PRINT(L"Initializing ConPTY STARTUPINFO");
	SIZE_T size = 0;

	startup_info_ex.StartupInfo.cb = sizeof(STARTUPINFOEX);

	// Create appropriate size thread attr list
	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	startup_info_ex.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(size));

	// Set STARTUPINFO attr list & init
	if (startup_info_ex.lpAttributeList == NULL ||
		!InitializeProcThreadAttributeList(
			startup_info_ex.lpAttributeList,
			1,
			0,
			(PSIZE_T)& size
	)) _ExitError(L"Unable to initialize STARTUPINFOEX attribute list");

	// Set thread attr list's ConPTY to the specified ConPTY
	if (!UpdateProcThreadAttribute(
		startup_info_ex.lpAttributeList,
		0,
		PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
		wsl_conpty,
		sizeof(HPCON),
		NULL,
		NULL
	)) _ExitError(L"Unable to set STARTUPINFOEX attribute list to specified ConPTY");

	return;
}

static void _WslExec(std::wstring* command_str)
{
	// Create stdio processing threads
	DEBUG_PRINT(L"Creating ConPTY stdio processing threads...");
	stdo_thread = INVALID_HANDLE_VALUE;
	//stdi_thread = INVALID_HANDLE_VALUE;

	stdo_thread = reinterpret_cast<HANDLE>(_beginthread(_ProcessConPTYStdo, 0, wrapper_stdin));
	//stdi_thread = reinterpret_cast<HANDLE>(_beginthread(_ProcessConPTYStdi, 0, wrapper_stdout));

	if (stdo_thread == INVALID_HANDLE_VALUE /* ||
		stdi_thread == INVALID_HANDLE_VALUE */) {
		_ExitError(L"Unable to create ConPTY stdio processing threads");
	}
	stdothread_open = TRUE;
	//stdithread_open = TRUE;

	// Create process
	DEBUG_PRINT(L"Creating wsl.exe process...");
	if (!CreateProcessW(
		NULL,							// No module name, use commandline
		LPWSTR(command_str->c_str()),			// Passed command (incl. arguments)
		NULL,							// Process handle not inheritable
		NULL,							// Thread handle not inheritable
		FALSE,							// Handle inheritance
		EXTENDED_STARTUPINFO_PRESENT,	// No creation flags
		NULL,							// Use parent's environment block
		NULL,							// Use parent's directory block
		&startup_info_ex.StartupInfo,	// Ptr to STARTUPINFO block
		&process_info					// Ptr to PROCESSINFO block
	)) _ExitError(L"Unable to create wsl.exe process");

	// Read stdout pipe in our read thread, then stop when wsl.exe finished
	WaitForSingleObject(process_info.hProcess, INFINITE);
	Sleep(500);

	// Close handles and exit
	_CloseHandles();
	ExitProcess(EXIT_SUCCESS);
}

static void __cdecl _ProcessConPTYStdo(LPVOID context)
{
	DWORD byte_read;
	DWORD byte_written;
	CHAR char_buffer[OUTBUF_SIZE];

	for (;;) {
		if (!ReadFile(wrapper_stdin, char_buffer, OUTBUF_SIZE, &byte_read, NULL)
			|| byte_read == 0)
			break;

		WriteFile(parent_stdout, char_buffer, byte_read, &byte_written, NULL);
	}
}

static void _CloseHandles(void)
{
	DEBUG_PRINT(L"Closing handles");

	// Close read/write thread
	if (stdothread_open) {
		DEBUG_PRINT(L"Closing stdo threads");
		CloseHandle(stdo_thread);
	}

	//	if (stdithread_open) {
	//		DEBUG_PRINT(L"Closing stdi thread");
	//		CloseHandle(stdi_thread);
	//	}

	// Close ConPTY
	if (conpty_open) {
		DEBUG_PRINT(L"Closing ConPTY, ConHost and attached clients");
		ClosePseudoConsole(wsl_conpty);
	}
}

static void _ExitError(const WCHAR* errorstr)
{
	DWORD dword = GetLastError();

	if (dword != 0) {
		LPVOID lp_mesg_buf;

		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			dword,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)& lp_mesg_buf,
			0,
			NULL
		);

		wprintf(L"\n[ERROR] %s: %s\n", errorstr, (LPWSTR) lp_mesg_buf);
		LocalFree(lp_mesg_buf);
	}
	else {
		wprintf(L"[ERROR] %s\n", errorstr);
	}

	_CloseHandles();
	ExitProcess(EXIT_FAILURE);
}

//static void __cdecl _ProcessConPTYStdi(LPVOID context)
//{
//	DWORD byte_read;
//	DWORD byte_written;
//	CHAR char_buffer[OUTBUF_SIZE];
//
//	for (;;) {
//		if (!ReadFile(parent_stdin, char_buffer, OUTBUF_SIZE, &byte_read, NULL)
//			|| byte_read == 0)
//			break;
//
//		WriteFile(wrapper_stdin, char_buffer, byte_read, &byte_written, NULL);
//	}
//}