#include "ntdll.h"
#include "KernelBase.h"
#include <stdio.h>

#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
#endif

HRESULT X_CreatePseudoConsole(
    COORD ConsoleSize,
    HANDLE hInput,
    HANDLE hOutput,
    DWORD dwFlags,
    PX_HPCON hpCon)
{
    NTSTATUS Status;
    BOOL bRes;

    HANDLE InputHandle = NULL, OutputHandle = NULL;
    HANDLE hConServer = NULL, hConReference;
    HANDLE hProc = NtCurrentProcess();
    HANDLE ReadPipeHandle, WritePipeHandle;
    HANDLE Values[4] = { NULL };
    HANDLE hToken = NULL; // Modify this if necessary

    SECURITY_ATTRIBUTES PipeAttributes = { 0 };
    wchar_t ConHostCommand[MAX_PATH];
    size_t AttrSize;
    LPPROC_THREAD_ATTRIBUTE_LIST AttrList = NULL;
    STARTUPINFOEXW SInfoEx = { 0 };
    PROCESS_INFORMATION ProcInfo;

    if (X_DuplicateHandle(hProc, hInput, hProc, &InputHandle, 0, TRUE, DUPLICATE_SAME_ACCESS) &&
        X_DuplicateHandle(hProc, hOutput, hProc, &OutputHandle, 0, TRUE, DUPLICATE_SAME_ACCESS))
    {
        Status = X_CreateHandle(
            &hConServer,
            L"\\Device\\ConDrv\\Server",
            GENERIC_ALL,
            NULL,
            TRUE,
            0);

        if (Status >= 0)
        {
            PipeAttributes.bInheritHandle = FALSE;
            PipeAttributes.lpSecurityDescriptor = NULL;
            PipeAttributes.nLength = sizeof(PipeAttributes);

            if (X_CreatePipe(&ReadPipeHandle, &WritePipeHandle, &PipeAttributes, 0) &&
                X_SetHandleInformation(ReadPipeHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            {
                PCWSTR InheritCursor = L"--inheritcursor "; // Requires one space
                if (!(dwFlags & PSEUDOCONSOLE_INHERIT_CURSOR))
                    InheritCursor = L"";

#ifdef FUN_MODE
                PCWSTR Format = L"\\\\?\\%s\\system32\\conhost.exe --signal 0x%x --server 0x%x";
                _snwprintf_s(
                    ConHostCommand,
                    MAX_PATH,
                    MAX_PATH,
                    Format,
                    RtlGetNtSystemRoot(),
                    HandleToULong(ReadPipeHandle),
                    HandleToULong(hConServer));
#else
                PCWSTR Format = L"\\\\?\\%s\\system32\\conhost.exe --headless %s--width %hu --height %hu --signal 0x%x --server 0x%x";
                _snwprintf_s(
                    ConHostCommand,
                    MAX_PATH,
                    MAX_PATH,
                    Format,
                    RtlGetNtSystemRoot(),
                    InheritCursor,
                    ConsoleSize.X,
                    ConsoleSize.Y,
                    HandleToULong(ReadPipeHandle),
                    HandleToULong(hConServer));
#endif // FUN_MODE

                // Initialize thread attribute list
                Values[0] = hConServer;
                Values[1] = InputHandle;
                Values[2] = OutputHandle;
                Values[3] = ReadPipeHandle;

                X_InitializeProcThreadAttributeList(NULL, 1, 0, &AttrSize);
                AttrList = malloc(AttrSize);
                X_InitializeProcThreadAttributeList(AttrList, 1, 0, &AttrSize);
                bRes = X_UpdateProcThreadAttribute(
                    AttrList,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, //0x20002u
                    Values,
                    sizeof(Values),
                    NULL,
                    NULL);

                // Assign members of STARTUPINFOEXW
                SInfoEx.StartupInfo.cb = sizeof(SInfoEx);
                SInfoEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
                SInfoEx.StartupInfo.hStdInput = InputHandle;
                SInfoEx.StartupInfo.hStdOutput = OutputHandle;
                SInfoEx.StartupInfo.hStdError = OutputHandle;
                SInfoEx.lpAttributeList = AttrList;

                bRes = CreateProcessAsUserW(
                    hToken,
                    NULL,
                    ConHostCommand,
                    NULL,
                    NULL,
                    TRUE,
                    EXTENDED_STARTUPINFO_PRESENT,
                    NULL,
                    NULL,
                    &SInfoEx.StartupInfo,
                    &ProcInfo);

                if (bRes)
                {
                    Status = X_CreateHandle(
                        &hConReference,
                        L"\\Reference",
                        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                        hConServer,
                        FALSE,
                        FILE_SYNCHRONOUS_IO_NONALERT);

                    if (Status >= 0)
                    {
                        hpCon->hWritePipe = WritePipeHandle;
                        hpCon->hConDrvReference = hConReference;
                        hpCon->hConHostProcess = ProcInfo.hProcess;
                    }
                }
                else
                    Log(X_GetLastError(), L"CreateProcessAsUserW");
            }
            else
                Log(X_GetLastError(), L"CreatePipe");
        }
        else
            Log(X_GetLastError(), L"CreateHandle");
    }
    else
        Log(X_GetLastError(), L"DuplicateHandle");

    // Cleanup
    free(AttrList);
    NtClose(InputHandle);
    NtClose(OutputHandle);
    NtClose(hConServer);
    //NtClose(ProcInfo.hThread);
    //NtClose(ProcInfo.hProcess);
    //NtClose(ReadPipeHandle);
    //NtClose(WritePipeHandle);

    return S_OK;
}

HRESULT X_ResizePseudoConsole(
    PX_HPCON hPC,
    COORD size)
{
    HRESULT hRes = 0;
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;

    RESIZE_PSEUDO_CONSOLE_BUFFER ResizeBuffer;
    ResizeBuffer.Flags = RESIZE_CONHOST_SIGNAL_BUFFER;
    ResizeBuffer.SizeX = size.X;
    ResizeBuffer.SizeY = size.Y;

    // WriteFile(hPC.hWritePipe, &Buffer, sizeof(RESIZE_BUFFER), NULL, NULL);

    Status = NtWriteFile(
        hPC->hWritePipe,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        &ResizeBuffer,
        sizeof(ResizeBuffer),
        NULL,
        NULL);

    if (Status < 0)
    {
        Log(Status, L"NtWriteFile");
        DWORD ErrCode = X_GetLastError();
        if (ErrCode > 0)
            hRes = ErrCode | 0x80070000;
        else
            hRes = ErrCode;
    }
    return hRes;
}

void X_ClosePseudoConsole(
    PX_HPCON hpCon)
{
    X_TerminateProcess(hpCon->hConHostProcess, 0);
    NtClose(hpCon->hWritePipe);
    NtClose(hpCon->hConDrvReference);
}
