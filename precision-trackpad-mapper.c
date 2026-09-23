#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <hidsdi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENT_QUEUE_SIZE 128
#define EVENT_QUEUE_MASK (EVENT_QUEUE_SIZE - 1)
#define IN_BUFFER_SIZE   4096
#define MAX_CACHED_DEVICES 16

enum {
    FN_MODE_NONE = 0,
    FN_MODE_LIFT,
    FN_MODE_DOWN
};

enum {
    MODE_NONE = 0,
    MODE_FIRST_SCREEN_CORNER = 1,
    MODE_FIRST_TRACKPAD_CORNER = 2,
    MODE_ACTIVE = 3
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;          // 'PTMC'
    uint16_t version;        // 1
    USHORT keyMainMod;
    USHORT keyQuickMod;
    USHORT keyActivate;
    USHORT keySettings;
    USHORT keyLeftClick;
    USHORT keyRightClick;
    int32_t fnMode;
    int32_t topLeftX;
    int32_t topLeftY;
    int32_t bottomRightX;
    int32_t bottomRightY;
    int64_t trackpadTopLeftX;
    int64_t trackpadTopLeftY;
    int64_t trackpadBottomRightX;
    int64_t trackpadBottomRightY;
} AppConfig;
#pragma pack(pop)

#define CONFIG_MAGIC 0x434D5450 // 'PTMC'

static AppConfig g_cfg = {
    .magic = CONFIG_MAGIC,
    .version = 1,
    .keyMainMod = 0x1D,    // LCtrl
    .keyQuickMod = 0x38,   // LAlt
    .keyActivate = 0x5B,   // LWin
    .keySettings = 0x18,   // O
    .keyLeftClick = 0x29,  // ` (Backtick)
    .keyRightClick = 0x00, // Disabled
    .fnMode = FN_MODE_DOWN,
    .topLeftX = 0,
    .topLeftY = 0,
    .bottomRightX = 0,
    .bottomRightY = 0,
    .trackpadTopLeftX = 0,
    .trackpadTopLeftY = 0,
    .trackpadBottomRightX = 2904,
    .trackpadBottomRightY = 1879
};

static SRWLOCK g_cfgLock = SRWLOCK_INIT;

static volatile LONG g_downMainMod    = 0;
static volatile LONG g_downQuickMod   = 0;
static volatile LONG g_downActivate   = 0;
static volatile LONG g_downSettings   = 0;
static volatile LONG g_downLeftClick  = 0;
static volatile LONG g_downRightClick = 0;

static volatile LONG g_leftTouching   = 0;
static volatile LONG g_rightTouching  = 0;

typedef struct {
    HANDLE deviceHandle;
    PHIDP_PREPARSED_DATA preparsed;
} CachedDevice;

static CachedDevice g_deviceCache[MAX_CACHED_DEVICES];
static int g_deviceCacheCount = 0;
static int g_deviceCacheNextEvict = 0;

static volatile LONG g_running = 1;
static volatile LONG g_mode = MODE_NONE;
static volatile LONG g_inSettingsMenu = 0;
static volatile LONG g_activeContactId = -1;

/* Lock-free Single-Producer Single-Consumer Button Queue */
static INPUT g_buttonQueue[EVENT_QUEUE_SIZE];
static volatile LONG g_queueHead = 0;
static volatile LONG g_queueTail = 0;
static HANDLE g_hWorkerWakeEvent = NULL;

static volatile LONG g_vLeft = 0;
static volatile LONG g_vTop = 0;
static volatile LONG g_vWidth = 1;
static volatile LONG g_vHeight = 1;

static volatile LONG g_latestRawX = -1;
static volatile LONG g_latestRawY = -1;
static volatile LONG g_cursorX = 0;
static volatile LONG g_cursorY = 0;

static HWND g_hMsgWnd = NULL;
static HANDLE g_hWorkerThread = NULL;
static HANDLE g_hHookThread = NULL;
static DWORD g_hookThreadId = 0;

void updateScreenMetrics(void) {
    InterlockedExchange(&g_vLeft,   GetSystemMetrics(SM_XVIRTUALSCREEN));
    InterlockedExchange(&g_vTop,    GetSystemMetrics(SM_YVIRTUALSCREEN));
    InterlockedExchange(&g_vWidth,  GetSystemMetrics(SM_CXVIRTUALSCREEN));
    InterlockedExchange(&g_vHeight, GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

static void getConfigPath(char* outPath, size_t maxLen) {
    DWORD len = GetModuleFileNameA(NULL, outPath, (DWORD)maxLen);
    if (len == 0 || len >= maxLen) {
        strncpy(outPath, ".\\trackpad_config.bin", maxLen);
        return;
    }
    char* lastSlash = strrchr(outPath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        strncat(outPath, "trackpad_config.bin", maxLen - strlen(outPath) - 1);
    } else {
        strncpy(outPath, ".\\trackpad_config.bin", maxLen);
    }
}

void saveSettingsAsync(void) {
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        AcquireSRWLockShared(&g_cfgLock);
        AppConfig snap = g_cfg;
        ReleaseSRWLockShared(&g_cfgLock);

        WriteFile(hFile, &snap, sizeof(AppConfig), &written, NULL);
        CloseHandle(hFile);
    }
}

void loadSettings(void) {
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        AppConfig temp;
        DWORD read = 0;
        if (ReadFile(hFile, &temp, sizeof(AppConfig), &read, NULL) && read == sizeof(AppConfig)) {
            if (temp.magic == CONFIG_MAGIC) {
                AcquireSRWLockExclusive(&g_cfgLock);
                g_cfg = temp;
                ReleaseSRWLockExclusive(&g_cfgLock);
            }
        }
        CloseHandle(hFile);
    }
}

PHIDP_PREPARSED_DATA getOrCachePreparsed(HANDLE hDevice) {
    for (int i = 0; i < g_deviceCacheCount; i++) {
        if (g_deviceCache[i].deviceHandle == hDevice)
            return g_deviceCache[i].preparsed;
    }

    UINT size = 0;
    GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, NULL, &size);
    if (size == 0) return NULL;

    PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)malloc(size);
    if (!preparsed) return NULL;

    if (GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, preparsed, &size) == (UINT)-1) {
        free(preparsed);
        return NULL;
    }

    if (g_deviceCacheCount < MAX_CACHED_DEVICES) {
        g_deviceCache[g_deviceCacheCount].deviceHandle = hDevice;
        g_deviceCache[g_deviceCacheCount].preparsed = preparsed;
        g_deviceCacheCount++;
    } else {
        int idx = g_deviceCacheNextEvict;
        g_deviceCacheNextEvict = (g_deviceCacheNextEvict + 1) % MAX_CACHED_DEVICES;
        free(g_deviceCache[idx].preparsed);
        g_deviceCache[idx].deviceHandle = hDevice;
        g_deviceCache[idx].preparsed = preparsed;
    }
    return preparsed;
}

/* Lock-free push into SPSC queue */
static inline void pushButtonEvent(DWORD dwFlags) {
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = dwFlags;

    LONG tail = InterlockedCompareExchange(&g_queueTail, 0, 0);
    LONG head = InterlockedCompareExchange(&g_queueHead, 0, 0);
    LONG nextTail = (tail + 1) & EVENT_QUEUE_MASK;

    if (nextTail != head) {
        g_buttonQueue[tail] = ip;
        MemoryBarrier();
        InterlockedExchange(&g_queueTail, nextTail);
        SetEvent(g_hWorkerWakeEvent);
    }
}

/* Worker thread consumes button events only */
DWORD WINAPI workerThreadProc(void* arg) {
    (void)arg;
    INPUT batch[EVENT_QUEUE_SIZE];

    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        WaitForSingleObject(g_hWorkerWakeEvent, INFINITE);

        UINT batchCount = 0;
        LONG head = InterlockedCompareExchange(&g_queueHead, 0, 0);
        LONG tail = InterlockedCompareExchange(&g_queueTail, 0, 0);

        while (head != tail && batchCount < EVENT_QUEUE_SIZE) {
            batch[batchCount++] = g_buttonQueue[head];
            head = (head + 1) & EVENT_QUEUE_MASK;
        }

        if (batchCount > 0) {
            InterlockedExchange(&g_queueHead, head);
            SendInput(batchCount, batch, sizeof(INPUT));
        }
    }
    return 0;
}

LRESULT CALLBACK IsolatedLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_MOUSEMOVE) {
            InterlockedExchange(&g_cursorX, p->pt.x);
            InterlockedExchange(&g_cursorY, p->pt.y);
        }
        if (!(p->flags & LLMHF_INJECTED)) {
            if (InterlockedCompareExchange(&g_mode, 0, 0) == MODE_ACTIVE) {
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

DWORD WINAPI isolatedHookThreadProc(void* arg) {
    (void)arg;
    HHOOK hook = SetWindowsHookEx(WH_MOUSE_LL, IsolatedLowLevelMouseProc, GetModuleHandle(NULL), 0);
    if (!hook) return -1;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(hook);
    return 0;
}

/* Non-blocking boundary calibration display */
typedef struct {
    int tlX, tlY, brX, brY;
} BlinkParams;

DWORD WINAPI blinkAnimationThread(void* param) {
    BlinkParams* p = (BlinkParams*)param;
    SetCursorPos(p->tlX, p->tlY);
    Sleep(50);
    SetCursorPos(p->brX, p->tlY);
    Sleep(50);
    SetCursorPos(p->brX, p->brY);
    Sleep(50);
    SetCursorPos(p->tlX, p->brY);
    Sleep(50);
    SetCursorPos(p->tlX, p->tlY);
    free(p);
    return 0;
}

void triggerModeBlink(int state) {
    pushButtonEvent(MOUSEEVENTF_LEFTUP);
    pushButtonEvent(MOUSEEVENTF_RIGHTUP);
    InterlockedExchange(&g_leftTouching, 0);
    InterlockedExchange(&g_rightTouching, 0);

    if (state) {
        BlinkParams* p = (BlinkParams*)malloc(sizeof(BlinkParams));
        if (p) {
            AcquireSRWLockShared(&g_cfgLock);
            p->tlX = g_cfg.topLeftX;
            p->tlY = g_cfg.topLeftY;
            p->brX = g_cfg.bottomRightX;
            p->brY = g_cfg.bottomRightY;
            ReleaseSRWLockShared(&g_cfgLock);

            HANDLE hBlink = CreateThread(NULL, 0, blinkAnimationThread, p, 0, NULL);
            if (hBlink) CloseHandle(hBlink);
            else free(p);
        }
    }
}

void showConfigMenu(void);

void handleKeyboard(USHORT code, USHORT flags) {
    int down = (flags & 1) ^ 1;

    AcquireSRWLockShared(&g_cfgLock);
    USHORT kMain  = g_cfg.keyMainMod;
    USHORT kQuick = g_cfg.keyQuickMod;
    USHORT kLeft  = g_cfg.keyLeftClick;
    USHORT kRight = g_cfg.keyRightClick;
    USHORT kAct   = g_cfg.keyActivate;
    USHORT kSet   = g_cfg.keySettings;
    ReleaseSRWLockShared(&g_cfgLock);

    if (code != 0 && code == kMain)        InterlockedExchange(&g_downMainMod, down);
    else if (code != 0 && code == kQuick)  InterlockedExchange(&g_downQuickMod, down);
    else if (code != 0 && code == kLeft)   InterlockedExchange(&g_downLeftClick, down);
    else if (code != 0 && code == kRight)  InterlockedExchange(&g_downRightClick, down);
    else if (code != 0 && code == kAct)    InterlockedExchange(&g_downActivate, down);
    else if (code != 0 && code == kSet)    InterlockedExchange(&g_downSettings, down);

    if (!down || InterlockedCompareExchange(&g_inSettingsMenu, 0, 0)) return;

    LONG dMain  = InterlockedCompareExchange(&g_downMainMod, 0, 0);
    LONG dQuick = InterlockedCompareExchange(&g_downQuickMod, 0, 0);
    LONG dAct   = InterlockedCompareExchange(&g_downActivate, 0, 0);
    LONG dSet   = InterlockedCompareExchange(&g_downSettings, 0, 0);
    LONG curMode = InterlockedCompareExchange(&g_mode, 0, 0);

    if (dMain && dSet) {
        if (curMode == MODE_ACTIVE) {
            triggerModeBlink(0);
            InterlockedExchange(&g_mode, MODE_NONE);
        }
        showConfigMenu();
        return;
    }

    if (dAct && dMain && !dQuick) {
        LONG cx = InterlockedCompareExchange(&g_cursorX, 0, 0);
        LONG cy = InterlockedCompareExchange(&g_cursorY, 0, 0);
        switch (curMode) {
            case MODE_NONE:
            case MODE_FIRST_TRACKPAD_CORNER:
                AcquireSRWLockExclusive(&g_cfgLock);
                g_cfg.topLeftX = cx;
                g_cfg.topLeftY = cy;
                ReleaseSRWLockExclusive(&g_cfgLock);
                InterlockedExchange(&g_mode, MODE_FIRST_SCREEN_CORNER);
                break;
            case MODE_FIRST_SCREEN_CORNER:
                AcquireSRWLockExclusive(&g_cfgLock);
                if (cx < g_cfg.topLeftX) { g_cfg.bottomRightX = g_cfg.topLeftX; g_cfg.topLeftX = cx; }
                else { g_cfg.bottomRightX = cx; }
                if (cy < g_cfg.topLeftY) { g_cfg.bottomRightY = g_cfg.topLeftY; g_cfg.topLeftY = cy; }
                else { g_cfg.bottomRightY = cy; }
                ReleaseSRWLockExclusive(&g_cfgLock);
                saveSettingsAsync();
                InterlockedExchange(&g_mode, MODE_NONE);
                break;
            case MODE_ACTIVE:
                triggerModeBlink(0);
                InterlockedExchange(&g_mode, MODE_NONE);
                break;
        }
    } else if (dAct && dMain && dQuick) {
        LONG rx = InterlockedCompareExchange(&g_latestRawX, 0, 0);
        LONG ry = InterlockedCompareExchange(&g_latestRawY, 0, 0);
        switch (curMode) {
            case MODE_NONE:
            case MODE_FIRST_SCREEN_CORNER:
                if (rx != -1 && ry != -1) {
                    AcquireSRWLockExclusive(&g_cfgLock);
                    g_cfg.trackpadTopLeftX = rx;
                    g_cfg.trackpadTopLeftY = ry;
                    ReleaseSRWLockExclusive(&g_cfgLock);
                    InterlockedExchange(&g_mode, MODE_FIRST_TRACKPAD_CORNER);
                }
                break;
            case MODE_FIRST_TRACKPAD_CORNER:
                if (rx != -1 && ry != -1) {
                    AcquireSRWLockExclusive(&g_cfgLock);
                    if (rx < g_cfg.trackpadTopLeftX) {
                        g_cfg.trackpadBottomRightX = g_cfg.trackpadTopLeftX;
                        g_cfg.trackpadTopLeftX = rx;
                    } else {
                        g_cfg.trackpadBottomRightX = rx;
                    }
                    if (ry < g_cfg.trackpadTopLeftY) {
                        g_cfg.trackpadBottomRightY = g_cfg.trackpadTopLeftY;
                        g_cfg.trackpadTopLeftY = ry;
                    } else {
                        g_cfg.trackpadBottomRightY = ry;
                    }
                    ReleaseSRWLockExclusive(&g_cfgLock);
                    saveSettingsAsync();
                    InterlockedExchange(&g_mode, MODE_NONE);
                }
                break;
            case MODE_ACTIVE:
                triggerModeBlink(0);
                InterlockedExchange(&g_mode, MODE_NONE);
                break;
        }
    } else if (dAct && dQuick && !dMain) {
        if (curMode == MODE_ACTIVE) {
            triggerModeBlink(0);
            InterlockedExchange(&g_mode, MODE_NONE);
        } else {
            InterlockedExchange(&g_mode, MODE_ACTIVE);
            triggerModeBlink(1);
        }
    }
}

LRESULT CALLBACK RawInputWndProc(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT) + IN_BUFFER_SIZE];
    static USAGE usages[64];

    switch (event) {
        case WM_DISPLAYCHANGE:
            updateScreenMetrics();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_INPUT: {
            UINT size = sizeof(rawinputBuffer);
            if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, rawinputBuffer, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
                return 0;

            RAWINPUT* data = (RAWINPUT*)rawinputBuffer;
            if (data->header.dwType == RIM_TYPEKEYBOARD) {
                handleKeyboard(data->data.keyboard.MakeCode, data->data.keyboard.Flags);
                return 0;
            }

            if (data->header.dwType != RIM_TYPEHID)
                return 0;

            PHIDP_PREPARSED_DATA preparsed = getOrCachePreparsed(data->header.hDevice);
            if (!preparsed) return 0;

            ULONG contactCount = 1;
            HidP_GetUsageValue(HidP_Input, 0x0D, 0, 0x54, &contactCount, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid);

            ULONG contactId = 0;
            HidP_GetUsageValue(HidP_Input, 0x0D, 0, 0x51, &contactId, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid);

            ULONG usageLength = sizeof(usages) / sizeof(USAGE);
            int tipSwitch = 0;
            if (HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
                for (ULONG j = 0; j < usageLength; j++) {
                    if (usages[j] == 0x42) {
                        tipSwitch = 1;
                        break;
                    }
                }
            }

            LONG lockedId = InterlockedCompareExchange(&g_activeContactId, -1, -1);
            if (tipSwitch) {
                if (lockedId == -1) {
                    InterlockedExchange(&g_activeContactId, contactId);
                    lockedId = (LONG)contactId;
                }
            } else {
                if (lockedId == (LONG)contactId) {
                    InterlockedExchange(&g_activeContactId, -1);
                    lockedId = -1;
                }
            }

            if (lockedId != -1 && lockedId != (LONG)contactId) {
                return 0;
            }

            ULONG rawValX = 0, rawValY = 0;
            if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x30, &rawValX, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
                InterlockedExchange(&g_latestRawX, (LONG)rawValX);
            }
            if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x31, &rawValY, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
                InterlockedExchange(&g_latestRawY, (LONG)rawValY);
            }

            if (InterlockedCompareExchange(&g_mode, 0, 0) != MODE_ACTIVE ||
                InterlockedCompareExchange(&g_inSettingsMenu, 0, 0)) {
                return 0;
            }

            LONG rx = InterlockedCompareExchange(&g_latestRawX, -1, -1);
            LONG ry = InterlockedCompareExchange(&g_latestRawY, -1, -1);

            AcquireSRWLockShared(&g_cfgLock);
            int64_t tpMinX = g_cfg.trackpadTopLeftX;
            int64_t tpMinY = g_cfg.trackpadTopLeftY;
            int64_t tpMaxX = g_cfg.trackpadBottomRightX;
            int64_t tpMaxY = g_cfg.trackpadBottomRightY;

            int32_t sTopLeftX = g_cfg.topLeftX;
            int32_t sTopLeftY = g_cfg.topLeftY;
            int64_t sW = (int64_t)g_cfg.bottomRightX - sTopLeftX;
            int64_t sH = (int64_t)g_cfg.bottomRightY - sTopLeftY;

            USHORT leftKey = g_cfg.keyLeftClick;
            USHORT rightKey = g_cfg.keyRightClick;
            int32_t fnMode = g_cfg.fnMode;
            ReleaseSRWLockShared(&g_cfgLock);

            if (rx != -1 && ry != -1 && tipSwitch) {
                int64_t tpW = tpMaxX - tpMinX;
                int64_t tpH = tpMaxY - tpMinY;
                if (tpW <= 0) tpW = 1;
                if (tpH <= 0) tpH = 1;

                int64_t clX = rx < tpMinX ? tpMinX : (rx > tpMaxX ? tpMaxX : rx);
                int64_t clY = ry < tpMinY ? tpMinY : (ry > tpMaxY ? tpMaxY : ry);

                int pixelX = sTopLeftX + (int)(( (clX - tpMinX) * sW ) / tpW);
                int pixelY = sTopLeftY + (int)(( (clY - tpMinY) * sH ) / tpH);

                /* Zero-latency direct cursor repositioning */
                SetCursorPos(pixelX, pixelY);
            }

            int leftDown = 0;
            LONG dLeft = InterlockedCompareExchange(&g_downLeftClick, 0, 0);
            if (leftKey != 0) {
                if (fnMode == FN_MODE_DOWN)        leftDown = dLeft;
                else if (fnMode == FN_MODE_LIFT)   leftDown = tipSwitch && !dLeft;
                else                               leftDown = tipSwitch;
            } else {
                leftDown = (fnMode == FN_MODE_NONE) ? tipSwitch : 0;
            }

            int rightDown = (rightKey != 0) ? (int)InterlockedCompareExchange(&g_downRightClick, 0, 0) : 0;

            if (leftDown != InterlockedCompareExchange(&g_leftTouching, 0, 0)) {
                pushButtonEvent(leftDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
                InterlockedExchange(&g_leftTouching, leftDown);
            }
            if (rightDown != InterlockedCompareExchange(&g_rightTouching, 0, 0)) {
                pushButtonEvent(rightDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
                InterlockedExchange(&g_rightTouching, rightDown);
            }

            return 0;
        }
    }
    return DefWindowProc(hwnd, event, wparam, lparam);
}

static void getKeyDisplayString(USHORT code, char* outBuf, size_t outSize) {
    if (code == 0) {
        snprintf(outBuf, outSize, "[Unbound / Disabled]");
        return;
    }
    switch (code) {
        case 0x01: snprintf(outBuf, outSize, "Escape (0x01)"); return;
        case 0x18: snprintf(outBuf, outSize, "O (0x18)"); return;
        case 0x1D: snprintf(outBuf, outSize, "Left Ctrl (0x1D)"); return;
        case 0x38: snprintf(outBuf, outSize, "Left Alt (0x38)"); return;
        case 0x5B: snprintf(outBuf, outSize, "Left Win (0x5B)"); return;
        case 0x29: snprintf(outBuf, outSize, "Backtick ` (0x29)"); return;
    }
    LONG lParam = (LONG)code << 16;
    char name[64] = {0};
    if (GetKeyNameTextA(lParam, name, sizeof(name)) > 0) {
        snprintf(outBuf, outSize, "%s (0x%02X)", name, code);
    } else {
        snprintf(outBuf, outSize, "Key (0x%02X)", code);
    }
}

static void printKeybind(const char* label, USHORT code) {
    char keyText[64];
    getKeyDisplayString(code, keyText, sizeof(keyText));
    printf("   %-20s: %s\n", label, keyText);
}

void printBanner(void) {
    AcquireSRWLockShared(&g_cfgLock);
    AppConfig snap = g_cfg;
    ReleaseSRWLockShared(&g_cfgLock);

    char strMain[64], strQuick[64], strAct[64], strSet[64];
    getKeyDisplayString(snap.keyMainMod, strMain, sizeof(strMain));
    getKeyDisplayString(snap.keyQuickMod, strQuick, sizeof(strQuick));
    getKeyDisplayString(snap.keyActivate, strAct, sizeof(strAct));
    getKeyDisplayString(snap.keySettings, strSet, sizeof(strSet));

    puts("===================================================================");
    puts("              Precision Trackpad Mapper (Ultra-Fast)               ");
    puts("===================================================================");
    puts(" Current Bindings:");
    printKeybind("[Main Modifier]", snap.keyMainMod);
    printKeybind("[Quick Modifier]", snap.keyQuickMod);
    printKeybind("[Activate Key]", snap.keyActivate);
    printKeybind("[Settings Key]", snap.keySettings);
    printKeybind("[Left Click / Fn]", snap.keyLeftClick);
    printKeybind("[Right Click]", snap.keyRightClick);
    puts(" Operational Hotkeys:");
    printf("   Open Settings     : %s + %s\n", strMain, strSet);
    printf("   Define Screen Box : %s + %s\n", strMain, strAct);
    printf("   Define Trackpad   : %s + %s + %s\n", strMain, strQuick, strAct);
    printf("   Toggle Mode       : %s + %s\n", strQuick, strAct);
    puts("-------------------------------------------------------------------");
    printf(" Status: Screen (%d,%d)-(%d,%d) | Trackpad (%lld,%lld)-(%lld,%lld)\n",
           snap.topLeftX, snap.topLeftY, snap.bottomRightX, snap.bottomRightY,
           snap.trackpadTopLeftX, snap.trackpadTopLeftY, snap.trackpadBottomRightX, snap.trackpadBottomRightY);
    puts("===================================================================");
}

void captureKeybind(const char* actionName, USHORT* targetKey) {
    while (GetAsyncKeyState(VK_RETURN) & 0x8000) Sleep(10);
    for (int vk = 8; vk <= 255; vk++) GetAsyncKeyState(vk);
    Sleep(50);

    printf("\nPress any key to bind to [%s] (Press ESC to unbind/disable)...\n", actionName);

    while (1) {
        for (int vk = 8; vk <= 255; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (vk == VK_ESCAPE) {
                    AcquireSRWLockExclusive(&g_cfgLock);
                    *targetKey = 0;
                    ReleaseSRWLockExclusive(&g_cfgLock);
                    printf("[%s] unbound.\n", actionName);
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    Sleep(200);
                    return;
                }
                UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                if (scancode != 0) {
                    AcquireSRWLockExclusive(&g_cfgLock);
                    *targetKey = (USHORT)scancode;
                    ReleaseSRWLockExclusive(&g_cfgLock);
                    char keyText[64];
                    getKeyDisplayString(*targetKey, keyText, sizeof(keyText));
                    printf("Assigned %s to [%s].\n", keyText, actionName);
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    Sleep(200);
                    return;
                }
            }
        }
        Sleep(10);
    }
}

void showConfigMenu(void) {
    InterlockedExchange(&g_inSettingsMenu, 1);
    char choice[16];
    while (1) {
        system("cls");
        printBanner();
        puts("\nConfiguration Menu:");
        puts(" 1. Rebind [Main Modifier]");
        puts(" 2. Rebind [Quick Modifier]");
        puts(" 3. Rebind [Activate Key]");
        puts(" 4. Rebind [Settings Key]");
        puts(" 5. Rebind [Left Click / Fn Key]");
        puts(" 6. Rebind [Right Click Key]");
        puts(" 7. Calibrate Trackpad Manually");
        puts(" 8. Switch Pen Mode (Down / Lift / None)");
        puts(" 9. Save Current Settings (Binary)");
        puts(" 10. Return to Mapping Service");
        puts(" 11. Exit Program");
        printf("\nSelect an option [1-11]: ");

        if (!fgets(choice, sizeof(choice), stdin)) continue;
        int opt = atoi(choice);

        switch (opt) {
            case 1: captureKeybind("Main Modifier", &g_cfg.keyMainMod); break;
            case 2: captureKeybind("Quick Modifier", &g_cfg.keyQuickMod); break;
            case 3: captureKeybind("Activate Key", &g_cfg.keyActivate); break;
            case 4: captureKeybind("Settings Key", &g_cfg.keySettings); break;
            case 5: captureKeybind("Left Click / Fn Key", &g_cfg.keyLeftClick); break;
            case 6: captureKeybind("Right Click Key", &g_cfg.keyRightClick); break;
            case 7: {
                long long x1, y1, x2, y2;
                printf("Enter trackpad bounds (minX minY maxX maxY): ");
                if (scanf("%lld %lld %lld %lld", &x1, &y1, &x2, &y2) == 4 && x1 < x2 && y1 < y2) {
                    AcquireSRWLockExclusive(&g_cfgLock);
                    g_cfg.trackpadTopLeftX = x1; g_cfg.trackpadTopLeftY = y1;
                    g_cfg.trackpadBottomRightX = x2; g_cfg.trackpadBottomRightY = y2;
                    ReleaseSRWLockExclusive(&g_cfgLock);
                    puts("Trackpad boundaries updated.");
                } else {
                    puts("Invalid values entered.");
                }
                while (getchar() != '\n');
                break;
            }
            case 8: {
                AcquireSRWLockExclusive(&g_cfgLock);
                g_cfg.fnMode = (g_cfg.fnMode + 1) % 3;
                ReleaseSRWLockExclusive(&g_cfgLock);
                printf("Fn mode changed.\n");
                Sleep(300);
                break;
            }
            case 9:
                saveSettingsAsync();
                Sleep(200);
                break;
            case 10:
                saveSettingsAsync();
                InterlockedExchange(&g_downMainMod, 0);
                InterlockedExchange(&g_downQuickMod, 0);
                InterlockedExchange(&g_downActivate, 0);
                InterlockedExchange(&g_downSettings, 0);
                InterlockedExchange(&g_inSettingsMenu, 0);
                system("cls");
                printBanner();
                puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");
                return;
            case 11:
                InterlockedExchange(&g_running, 0);
                if (g_hMsgWnd) PostMessage(g_hMsgWnd, WM_CLOSE, 0, 0);
                return;
            default:
                break;
        }
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    (void)dwCtrlType;
    InterlockedExchange(&g_running, 0);
    if (g_hookThreadId) PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hMsgWnd) PostMessage(g_hMsgWnd, WM_CLOSE, 0, 0);
    return TRUE;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    const char* class_name = "precision-trackpad-mapper-class";

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    updateScreenMetrics();

    AcquireSRWLockExclusive(&g_cfgLock);
    g_cfg.topLeftX = g_vLeft;
    g_cfg.topLeftY = g_vTop;
    g_cfg.bottomRightX = g_vLeft + g_vWidth;
    g_cfg.bottomRightY = g_vTop + g_vHeight;
    ReleaseSRWLockExclusive(&g_cfgLock);

    loadSettings();
    g_hWorkerWakeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    g_hWorkerThread = CreateThread(NULL, 0, workerThreadProc, NULL, 0, NULL);
    if (!g_hWorkerThread) return -1;
    SetThreadPriority(g_hWorkerThread, THREAD_PRIORITY_HIGHEST);

    g_hHookThread = CreateThread(NULL, 0, isolatedHookThreadProc, NULL, 0, &g_hookThreadId);
    if (!g_hHookThread) return -1;
    SetThreadPriority(g_hHookThread, THREAD_PRIORITY_HIGHEST);

    WNDCLASS window_class = {0};
    window_class.lpfnWndProc = RawInputWndProc;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = class_name;
    if (!RegisterClass(&window_class)) return -1;

    g_hMsgWnd = CreateWindow(class_name, "Precision Trackpad Engine", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
    if (!g_hMsgWnd) return -1;

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x0D;
    rid[0].usUsage     = 0x05;             // Touchpad
    rid[0].dwFlags     = RIDEV_INPUTSINK;
    rid[0].hwndTarget  = g_hMsgWnd;

    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x06;             // Keyboard
    rid[1].dwFlags     = RIDEV_INPUTSINK;
    rid[1].hwndTarget  = g_hMsgWnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) return -1;

    system("cls");
    printBanner();
    puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");

    MSG message;
    while (InterlockedCompareExchange(&g_running, 1, 1) && GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    InterlockedExchange(&g_running, 0);
    if (g_hookThreadId) PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
    SetEvent(g_hWorkerWakeEvent);

    WaitForSingleObject(g_hWorkerThread, 500);
    WaitForSingleObject(g_hHookThread, 500);

    CloseHandle(g_hWorkerThread);
    CloseHandle(g_hHookThread);
    CloseHandle(g_hWorkerWakeEvent);

    for (int i = 0; i < g_deviceCacheCount; i++) {
        if (g_deviceCache[i].preparsed) {
            free(g_deviceCache[i].preparsed);
        }
    }

    return 0;
}