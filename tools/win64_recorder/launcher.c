/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Suspended-process launcher that injects twd_recorder.dll into WalkingDead4.exe
 * before the entry point runs, captures live function calls into TWD_CAPTURE_OUT,
 * and stops after --timeout-ms cleanly.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dirname_of(const char *path, char *out, size_t cap) {
    size_t len = strlen(path);
    if (len >= cap) len = cap - 1;
    memcpy(out, path, len);
    out[len] = '\0';
    for (size_t i = len; i > 0; i--) {
        if (out[i - 1] == '\\' || out[i - 1] == '/') {
            out[i - 1] = '\0';
            return;
        }
    }
    out[0] = '.';
    out[1] = '\0';
}

int main(int argc, char **argv) {
    const char *exe_path = NULL;
    const char *dll_path = NULL;
    const char *out_path = NULL;
    DWORD timeout_ms = 8000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (strcmp(argv[i], "--dll") == 0 && i + 1 < argc) {
            dll_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            timeout_ms = (DWORD)strtoul(argv[++i], NULL, 10);
        }
    }

    if (!exe_path || !dll_path || !out_path) {
        fprintf(stderr, "usage: launcher --exe PATH --dll PATH --out PATH [--timeout-ms MS]\n");
        return 2;
    }

    char exe_full[MAX_PATH], dll_full[MAX_PATH], out_full[MAX_PATH], work_dir[MAX_PATH];
    if (!GetFullPathNameA(exe_path, MAX_PATH, exe_full, NULL) ||
        !GetFullPathNameA(dll_path, MAX_PATH, dll_full, NULL) ||
        !GetFullPathNameA(out_path, MAX_PATH, out_full, NULL)) {
        fprintf(stderr, "failed to resolve paths\n");
        return 2;
    }
    dirname_of(exe_full, work_dir, sizeof(work_dir));

    SetEnvironmentVariableA("TWD_CAPTURE_OUT", out_full);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_MINIMIZE;

    if (!CreateProcessA(exe_full, NULL, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, work_dir, &si, &pi)) {
        fprintf(stderr, "CreateProcessA failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    size_t dll_bytes = strlen(dll_full) + 1;
    void *remote_str = VirtualAllocEx(pi.hProcess, NULL, dll_bytes,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_str ||
        !WriteProcessMemory(pi.hProcess, remote_str, dll_full, dll_bytes, NULL)) {
        fprintf(stderr, "remote alloc/write failed: %lu\n", (unsigned long)GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC load_lib = k32 ? GetProcAddress(k32, "LoadLibraryA") : NULL;
    if (!load_lib) {
        fprintf(stderr, "GetProcAddress(LoadLibraryA) failed\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)(void *)load_lib,
                                   remote_str, 0, NULL);
    if (!th) {
        fprintf(stderr, "CreateRemoteThread failed: %lu\n", (unsigned long)GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    WaitForSingleObject(th, 10000);
    DWORD mod_ret = 0;
    GetExitCodeThread(th, &mod_ret);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, remote_str, 0, MEM_RELEASE);

    if (mod_ret == 0) {
        fprintf(stderr, "remote LoadLibraryA returned 0 (check bitness or path)\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    ResumeThread(pi.hThread);
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        printf("captured trace to %s (timed out after %lu ms)\n", out_full, (unsigned long)timeout_ms);
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        printf("captured trace to %s (process exited early: code=%lu / 0x%08lx)\n",
               out_full, (unsigned long)exit_code, (unsigned long)exit_code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
