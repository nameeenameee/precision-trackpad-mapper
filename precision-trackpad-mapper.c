#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <hidsdi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fix MinGW-w64 missing QWORD in NEXTRAWINPUTBLOCK */
#ifdef NEXTRAWINPUTBLOCK
#undef NEXTRAWINPUTBLOCK
#endif
#define NEXTRAWINPUTBLOCK(ptr) \
    ((PRAWINPUT)(((ULONG_PTR)((BYTE*)(ptr) + (ptr)->header.dwSize) + sizeof(ULONG_PTR) - 1) & ~(sizeof(ULONG_PTR) - 1)))

#define IN_BUFFER_SIZE         4096
#define MAX_CACHED_DEVICES     16
#define TIMER_BLINK_ID         1001
#define RAWINPUT_BATCH_COUNT   16

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

/* Lock-free Config Snapshot */
static AppConfig* volatile g_activeCfg = NULL;
static SRWLOCK g_cfgUpdateLock = SRWLOCK_INIT;

/* Atomic Global States */
static volatile LONG g_downMainMod    = 0;
static volatile LONG g_downQuickMod   = 0;
static volatile LONG g_downActivate   = 0;
static volatile LONG g_downSettings   = 0;
static volatile LONG g_downLeftClick  = 0;
static volatile LONG g_downRightClick = 0;

static volatile LONG g_leftTouching   = 0;
static volatile LONG g_rightTouching  = 0;

static volatile LONG g_running        = 1;
static volatile LONG g_mode           = MODE_NONE;
static volatile LONG g_inSettingsMenu = 0;
static volatile LONG g_activeContactId = -1;

static volatile LONG g_vLeft   = 0;
static volatile LONG g_vTop    = 0;
static volatile LONG g_vWidth  = 1;
static volatile LONG g_vHeight = 1;

static volatile LONG g_latestRawX = -1;
static volatile LONG g_latestRawY = -1;

static HWND g_hMsgWnd = NULL;
static HANDLE g_hHookThread = NULL;
static DWORD g_hookThreadId = 0;

/* Device Preparsed Data Cache */
typedef struct {
    HANDLE deviceHandle;
    PHIDP_PREPARSED_DATA preparsed;
} CachedDevice;

static CachedDevice g_deviceCache[MAX_CACHED_DEVICES];
static int g_deviceCacheCount = 0;
static CachedDevice* g_lastUsedDevice = NULL;

/* Non-blocking boundary blink state */
static struct {
    int step;
    int tlX, tlY, brX, brY;
} g_blinkState;

/* --- Screen Metrics & Configuration Management --- */

void updateScreenMetrics(void) {
    InterlockedExchange(&g_vLeft,   GetSystemMetrics(SM_XVIRTUALSCREEN));
    InterlockedExchange(&g_vTop,    GetSystemMetrics(SM_YVIRTUALSCREEN));
    LONG w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    LONG h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    InterlockedExchange(&g_vWidth,  w > 0 ? w : 1);
    InterlockedExchange(&g_vHeight, h > 0 ? h : 1);
}

static inline AppConfig* getActiveConfig(void) {
    return (AppConfig*)InterlockedCompareExchangePointer((void* volatile*)&g_activeCfg, NULL, NULL);
}

static void updateConfigRCU(const AppConfig* newCfg) {
    AppConfig* copy = (AppConfig*)malloc(sizeof(AppConfig));
    if (!copy) return;
    memcpy(copy, newCfg, sizeof(AppConfig));

    AcquireSRWLockExclusive(&g_cfgUpdateLock);
    AppConfig* old = (AppConfig*)InterlockedExchangePointer((void* volatile*)&g_activeCfg, copy);
    ReleaseSRWLockExclusive(&g_cfgUpdateLock);

    if (old) {
        Sleep(50);
        free(old);
    }
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
        AppConfig* cur = getActiveConfig();
        if (cur) {
            WriteFile(hFile, cur, sizeof(AppConfig), &written, NULL);
        }
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
                updateConfigRCU(&temp);
            }
        }
        CloseHandle(hFile);
    }
}

/* --- Fast Device Caching --- */

static inline PHIDP_PREPARSED_DATA getOrCachePreparsed(HANDLE hDevice) {
    if (g_lastUsedDevice && g_lastUsedDevice->deviceHandle == hDevice) {
        return g_lastUsedDevice->preparsed;
    }

    for (int i = 0; i < g_deviceCacheCount; ++i) {
        if (g_deviceCache[i].deviceHandle == hDevice) {
            g_lastUsedDevice = &g_deviceCache[i];
            return g_deviceCache[i].preparsed;
        }
    }

    if (g_deviceCacheCount >= MAX_CACHED_DEVICES) return NULL;

    UINT size = 0;
    GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, NULL, &size);
    if (size == 0) return NULL;

    PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)malloc(size);
    if (!preparsed) return NULL;

    if (GetRawInputDeviceInfo(hDevice, RIDI_PREPARSEDDATA, preparsed, &size) == (UINT)-1) {
        free(preparsed);
        return NULL;
    }

    CachedDevice* target = &g_deviceCache[g_deviceCacheCount++];
    target->deviceHandle = hDevice;
    target->preparsed = preparsed;
    g_lastUsedDevice = target;
    return preparsed;
}

/* --- Subpixel Direct Input Dispatch --- */

static inline void moveCursorAbsoluteSubpixel(int64_t targetX, int64_t targetY) {
    LONG vL = g_vLeft;
    LONG vT = g_vTop;
    LONG vW = g_vWidth;
    LONG vH = g_vHeight;

    if (vW <= 0 || vH <= 0) return;

    // High precision direct scale to 65535 normalized coordinates
    DWORD normX = (DWORD)(((targetX - (int64_t)vL) * 65535LL) / (int64_t)vW);
    DWORD normY = (DWORD)(((targetY - (int64_t)vT) * 65535LL) / (int64_t)vH);

    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dx = normX;
    ip.mi.dy = normY;
    ip.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &ip, sizeof(INPUT));
}

static inline void sendButtonEventDirect(DWORD dwFlags) {
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = dwFlags;
    SendInput(1, &ip, sizeof(INPUT));
}

/* --- Hardware Tap/Click Filter Hook Thread --- */

LRESULT CALLBACK HardwareClickFilterProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_mode == MODE_ACTIVE) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (!(p->flags & LLMHF_INJECTED)) {
            switch (wParam) {
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONUP:
                    return 1; // Suppress hardware clicks
                default:
                    break;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

DWORD WINAPI hookThreadProc(void* arg) {
    (void)arg;
    HHOOK hook = SetWindowsHookEx(WH_MOUSE_LL, HardwareClickFilterProc, GetModuleHandle(NULL), 0);
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

/* --- Non-Blocking UI Helpers --- */

static void triggerModeBlink(int state) {
    sendButtonEventDirect(MOUSEEVENTF_LEFTUP);
    sendButtonEventDirect(MOUSEEVENTF_RIGHTUP);
    InterlockedExchange(&g_leftTouching, 0);
    InterlockedExchange(&g_rightTouching, 0);

    if (state && g_hMsgWnd) {
        AppConfig* cfg = getActiveConfig();
        if (!cfg) return;

        g_blinkState.tlX = cfg->topLeftX;
        g_blinkState.tlY = cfg->topLeftY;
        g_blinkState.brX = cfg->bottomRightX;
        g_blinkState.brY = cfg->bottomRightY;
        g_blinkState.step = 0;

        SetTimer(g_hMsgWnd, TIMER_BLINK_ID, 50, NULL);
    }
}

void showConfigMenu(void);

/* --- Key & Trackpad Calibration Routines --- */

void handleKeyboard(USHORT code, USHORT flags) {
    int down = (flags & 1) ^ 1;
    AppConfig* cfg = getActiveConfig();
    if (!cfg) return;

    if (code != 0 && code == cfg->keyMainMod)        InterlockedExchange(&g_downMainMod, down);
    else if (code != 0 && code == cfg->keyQuickMod)  InterlockedExchange(&g_downQuickMod, down);
    else if (code != 0 && code == cfg->keyLeftClick)   InterlockedExchange(&g_downLeftClick, down);
    else if (code != 0 && code == cfg->keyRightClick)  InterlockedExchange(&g_downRightClick, down);
    else if (code != 0 && code == cfg->keyActivate)    InterlockedExchange(&g_downActivate, down);
    else if (code != 0 && code == cfg->keySettings)    InterlockedExchange(&g_downSettings, down);

    if (!down || g_inSettingsMenu) return;

    LONG dMain  = g_downMainMod;
    LONG dQuick = g_downQuickMod;
    LONG dAct   = g_downActivate;
    LONG dSet   = g_downSettings;
    LONG curMode = g_mode;

    if (dMain && dSet) {
        if (curMode == MODE_ACTIVE) {
            triggerModeBlink(0);
            InterlockedExchange(&g_mode, MODE_NONE);
        }
        showConfigMenu();
        return;
    }

    if (dAct && dMain && !dQuick) {
        POINT pt;
        GetCursorPos(&pt);
        AppConfig nextCfg = *cfg;

        switch (curMode) {
            case MODE_NONE:
            case MODE_FIRST_TRACKPAD_CORNER:
                nextCfg.topLeftX = pt.x;
                nextCfg.topLeftY = pt.y;
                updateConfigRCU(&nextCfg);
                InterlockedExchange(&g_mode, MODE_FIRST_SCREEN_CORNER);
                break;

            case MODE_FIRST_SCREEN_CORNER:
                if (pt.x < nextCfg.topLeftX) {
                    nextCfg.bottomRightX = nextCfg.topLeftX;
                    nextCfg.topLeftX = pt.x;
                } else {
                    nextCfg.bottomRightX = pt.x;
                }
                if (pt.y < nextCfg.topLeftY) {
                    nextCfg.bottomRightY = nextCfg.topLeftY;
                    nextCfg.topLeftY = pt.y;
                } else {
                    nextCfg.bottomRightY = pt.y;
                }
                updateConfigRCU(&nextCfg);
                saveSettingsAsync();
                InterlockedExchange(&g_mode, MODE_NONE);
                break;

            case MODE_ACTIVE:
                triggerModeBlink(0);
                InterlockedExchange(&g_mode, MODE_NONE);
                break;
        }
    } else if (dAct && dMain && dQuick) {
        LONG rx = g_latestRawX;
        LONG ry = g_latestRawY;
        AppConfig nextCfg = *cfg;

        switch (curMode) {
            case MODE_NONE:
            case MODE_FIRST_SCREEN_CORNER:
                if (rx != -1 && ry != -1) {
                    nextCfg.trackpadTopLeftX = rx;
                    nextCfg.trackpadTopLeftY = ry;
                    updateConfigRCU(&nextCfg);
                    InterlockedExchange(&g_mode, MODE_FIRST_TRACKPAD_CORNER);
                }
                break;

            case MODE_FIRST_TRACKPAD_CORNER:
                if (rx != -1 && ry != -1) {
                    if (rx < nextCfg.trackpadTopLeftX) {
                        nextCfg.trackpadBottomRightX = nextCfg.trackpadTopLeftX;
                        nextCfg.trackpadTopLeftX = rx;
                    } else {
                        nextCfg.trackpadBottomRightX = rx;
                    }
                    if (ry < nextCfg.trackpadTopLeftY) {
                        nextCfg.trackpadBottomRightY = nextCfg.trackpadTopLeftY;
                        nextCfg.trackpadTopLeftY = ry;
                    } else {
                        nextCfg.trackpadBottomRightY = ry;
                    }
                    updateConfigRCU(&nextCfg);
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

/* --- Optimized HID Packet Parsing & Dispatch --- */

static inline void processRawInputPacket(RAWINPUT* raw) {
    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        handleKeyboard(raw->data.keyboard.MakeCode, raw->data.keyboard.Flags);
        return;
    }

    if (raw->header.dwType != RIM_TYPEHID) return;

    PHIDP_PREPARSED_DATA preparsed = getOrCachePreparsed(raw->header.hDevice);
    if (!preparsed) return;

    ULONG contactId = 0;
    HidP_GetUsageValue(HidP_Input, 0x0D, 0, 0x51, &contactId, preparsed, raw->data.hid.bRawData, raw->data.hid.dwSizeHid);

    USAGE usages[16];
    ULONG usageLength = sizeof(usages) / sizeof(USAGE);
    int tipSwitch = 0;
    if (HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, raw->data.hid.bRawData, raw->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
        for (ULONG j = 0; j < usageLength; ++j) {
            if (usages[j] == 0x42) {
                tipSwitch = 1;
                break;
            }
        }
    }

    LONG lockedId = g_activeContactId;
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
        return;
    }

    ULONG rawValX = 0, rawValY = 0;
    if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x30, &rawValX, preparsed, raw->data.hid.bRawData, raw->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
        InterlockedExchange(&g_latestRawX, (LONG)rawValX);
    }
    if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x31, &rawValY, preparsed, raw->data.hid.bRawData, raw->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
        InterlockedExchange(&g_latestRawY, (LONG)rawValY);
    }

    if (g_mode != MODE_ACTIVE || g_inSettingsMenu) {
        return;
    }

    AppConfig* cfg = getActiveConfig();
    if (!cfg) return;

    LONG rx = g_latestRawX;
    LONG ry = g_latestRawY;

    // Coalesce inputs into a single SendInput array to save context transitions
    INPUT inputs[3];
    int inputCount = 0;

    if (rx != -1 && ry != -1 && tipSwitch) {
        int64_t tpMinX = cfg->trackpadTopLeftX;
        int64_t tpMinY = cfg->trackpadTopLeftY;
        int64_t tpMaxX = cfg->trackpadBottomRightX;
        int64_t tpMaxY = cfg->trackpadBottomRightY;

        int64_t tpW = tpMaxX - tpMinX;
        int64_t tpH = tpMaxY - tpMinY;
        if (tpW <= 0) tpW = 1;
        if (tpH <= 0) tpH = 1;

        int64_t clX = rx < tpMinX ? tpMinX : (rx > tpMaxX ? tpMaxX : rx);
        int64_t clY = ry < tpMinY ? tpMinY : (ry > tpMaxY ? tpMaxY : ry);

        int64_t sTopLeftX = cfg->topLeftX;
        int64_t sTopLeftY = cfg->topLeftY;
        int64_t sW = (int64_t)cfg->bottomRightX - sTopLeftX;
        int64_t sH = (int64_t)cfg->bottomRightY - sTopLeftY;

        // Subpixel target coordinate calculation without quantization downsample
        int64_t targetX = sTopLeftX + ((clX - tpMinX) * sW) / tpW;
        int64_t targetY = sTopLeftY + ((clY - tpMinY) * sH) / tpH;

        LONG vL = g_vLeft;
        LONG vT = g_vTop;
        LONG vW = g_vWidth;
        LONG vH = g_vHeight;

        DWORD normX = (DWORD)(((targetX - (int64_t)vL) * 65535LL) / (int64_t)vW);
        DWORD normY = (DWORD)(((targetY - (int64_t)vT) * 65535LL) / (int64_t)vH);

        inputs[inputCount].type = INPUT_MOUSE;
        inputs[inputCount].mi.dx = normX;
        inputs[inputCount].mi.dy = normY;
        inputs[inputCount].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[inputCount].mi.mouseData = 0;
        inputs[inputCount].mi.time = 0;
        inputs[inputCount].mi.dwExtraInfo = 0;
        inputCount++;
    }

    int leftDown = 0;
    LONG dLeft = g_downLeftClick;
    if (cfg->keyLeftClick != 0) {
        if (cfg->fnMode == FN_MODE_DOWN)        leftDown = dLeft;
        else if (cfg->fnMode == FN_MODE_LIFT)   leftDown = tipSwitch && !dLeft;
        else                                    leftDown = tipSwitch;
    } else {
        leftDown = (cfg->fnMode == FN_MODE_NONE) ? tipSwitch : 0;
    }

    int rightDown = (cfg->keyRightClick != 0) ? (int)g_downRightClick : 0;

    if (leftDown != g_leftTouching) {
        inputs[inputCount].type = INPUT_MOUSE;
        inputs[inputCount].mi.dx = 0;
        inputs[inputCount].mi.dy = 0;
        inputs[inputCount].mi.dwFlags = leftDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        inputs[inputCount].mi.mouseData = 0;
        inputs[inputCount].mi.time = 0;
        inputs[inputCount].mi.dwExtraInfo = 0;
        inputCount++;
        InterlockedExchange(&g_leftTouching, leftDown);
    }

    if (rightDown != g_rightTouching) {
        inputs[inputCount].type = INPUT_MOUSE;
        inputs[inputCount].mi.dx = 0;
        inputs[inputCount].mi.dy = 0;
        inputs[inputCount].mi.dwFlags = rightDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        inputs[inputCount].mi.mouseData = 0;
        inputs[inputCount].mi.time = 0;
        inputs[inputCount].mi.dwExtraInfo = 0;
        inputCount++;
        InterlockedExchange(&g_rightTouching, rightDown);
    }

    if (inputCount > 0) {
        SendInput(inputCount, inputs, sizeof(INPUT));
    }
}

/* --- Optimized Window Procedure with Drained Input Buffer --- */

LRESULT CALLBACK RawInputWndProc(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam) {
    switch (event) {
        case WM_TIMER:
            if (wparam == TIMER_BLINK_ID) {
                switch (g_blinkState.step++) {
                    case 0: moveCursorAbsoluteSubpixel(g_blinkState.tlX, g_blinkState.tlY); break;
                    case 1: moveCursorAbsoluteSubpixel(g_blinkState.brX, g_blinkState.tlY); break;
                    case 2: moveCursorAbsoluteSubpixel(g_blinkState.brX, g_blinkState.brY); break;
                    case 3: moveCursorAbsoluteSubpixel(g_blinkState.tlX, g_blinkState.brY); break;
                    case 4: moveCursorAbsoluteSubpixel(g_blinkState.tlX, g_blinkState.tlY); break;
                    default:
                        KillTimer(hwnd, TIMER_BLINK_ID);
                        break;
                }
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            updateScreenMetrics();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_INPUT: {
            // Stack-allocated batch read buffer to eliminate dynamic allocation
            alignas(RAWINPUT) BYTE rawBuffer[sizeof(RAWINPUTHEADER) + IN_BUFFER_SIZE];
            UINT size = sizeof(rawBuffer);

            if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, rawBuffer, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
                processRawInputPacket((RAWINPUT*)rawBuffer);
            }

            // Drain any pending Raw Input packets in queue immediately
            alignas(RAWINPUT) BYTE batchBuffer[(sizeof(RAWINPUTHEADER) + IN_BUFFER_SIZE) * RAWINPUT_BATCH_COUNT];
            UINT batchSize = sizeof(batchBuffer);
            UINT count = GetRawInputBuffer((PRAWINPUT)batchBuffer, &batchSize, sizeof(RAWINPUTHEADER));

            if (count != (UINT)-1 && count > 0) {
                PRAWINPUT pCurrent = (PRAWINPUT)batchBuffer;
                for (UINT i = 0; i < count; ++i) {
                    processRawInputPacket(pCurrent);
                    pCurrent = NEXTRAWINPUTBLOCK(pCurrent);
                }
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, event, wparam, lparam);
}

/* --- Configuration Menu & UI --- */

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
    AppConfig* snap = getActiveConfig();
    if (!snap) return;

    char strMain[64], strQuick[64], strAct[64], strSet[64];
    getKeyDisplayString(snap->keyMainMod, strMain, sizeof(strMain));
    getKeyDisplayString(snap->keyQuickMod, strQuick, sizeof(strQuick));
    getKeyDisplayString(snap->keyActivate, strAct, sizeof(strAct));
    getKeyDisplayString(snap->keySettings, strSet, sizeof(strSet));

    puts("===================================================================");
    puts("          Precision Trackpad Mapper (v2.0.0)                       ");
    puts("===================================================================");
    puts(" Current Bindings:");
    printKeybind("[Main Modifier]", snap->keyMainMod);
    printKeybind("[Quick Modifier]", snap->keyQuickMod);
    printKeybind("[Activate Key]", snap->keyActivate);
    printKeybind("[Settings Key]", snap->keySettings);
    printKeybind("[Left Click / Fn]", snap->keyLeftClick);
    printKeybind("[Right Click]", snap->keyRightClick);
    puts(" Operational Hotkeys:");
    printf("   Open Settings     : %s + %s\n", strMain, strSet);
    printf("   Define Screen Box : %s + %s\n", strMain, strAct);
    printf("   Define Trackpad   : %s + %s + %s\n", strMain, strQuick, strAct);
    printf("   Toggle Mode       : %s + %s\n", strQuick, strAct);
    puts("-------------------------------------------------------------------");
    printf(" Status: Screen (%d,%d)-(%d,%d) | Trackpad (%lld,%lld)-(%lld,%lld)\n",
           snap->topLeftX, snap->topLeftY, snap->bottomRightX, snap->bottomRightY,
           snap->trackpadTopLeftX, snap->trackpadTopLeftY, snap->trackpadBottomRightX, snap->trackpadBottomRightY);
    puts("===================================================================");
}

void captureKeybind(const char* actionName, size_t structOffset) {
    while (GetAsyncKeyState(VK_RETURN) & 0x8000) Sleep(10);
    for (int vk = 8; vk <= 255; vk++) GetAsyncKeyState(vk);
    Sleep(50);

    printf("\nPress any key to bind to [%s] (Press ESC to unbind/disable)...\n", actionName);

    while (1) {
        for (int vk = 8; vk <= 255; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                AppConfig* cur = getActiveConfig();
                if (!cur) return;
                AppConfig nextCfg = *cur;

                if (vk == VK_ESCAPE) {
                    *(USHORT*)((uint8_t*)&nextCfg + structOffset) = 0;
                    updateConfigRCU(&nextCfg);
                    printf("[%s] unbound.\n", actionName);
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    Sleep(200);
                    return;
                }
                UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                if (scancode != 0) {
                    *(USHORT*)((uint8_t*)&nextCfg + structOffset) = (USHORT)scancode;
                    updateConfigRCU(&nextCfg);
                    char keyText[64];
                    getKeyDisplayString((USHORT)scancode, keyText, sizeof(keyText));
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
            case 1: captureKeybind("Main Modifier", offsetof(AppConfig, keyMainMod)); break;
            case 2: captureKeybind("Quick Modifier", offsetof(AppConfig, keyQuickMod)); break;
            case 3: captureKeybind("Activate Key", offsetof(AppConfig, keyActivate)); break;
            case 4: captureKeybind("Settings Key", offsetof(AppConfig, keySettings)); break;
            case 5: captureKeybind("Left Click / Fn Key", offsetof(AppConfig, keyLeftClick)); break;
            case 6: captureKeybind("Right Click Key", offsetof(AppConfig, keyRightClick)); break;
            case 7: {
                long long x1, y1, x2, y2;
                printf("Enter trackpad bounds (minX minY maxX maxY): ");
                if (scanf("%lld %lld %lld %lld", &x1, &y1, &x2, &y2) == 4 && x1 < x2 && y1 < y2) {
                    AppConfig* cur = getActiveConfig();
                    if (cur) {
                        AppConfig nextCfg = *cur;
                        nextCfg.trackpadTopLeftX = x1; nextCfg.trackpadTopLeftY = y1;
                        nextCfg.trackpadBottomRightX = x2; nextCfg.trackpadBottomRightY = y2;
                        updateConfigRCU(&nextCfg);
                        puts("Trackpad boundaries updated.");
                    }
                } else {
                    puts("Invalid values entered.");
                }
                while (getchar() != '\n');
                break;
            }
            case 8: {
                AppConfig* cur = getActiveConfig();
                if (cur) {
                    AppConfig nextCfg = *cur;
                    nextCfg.fnMode = (nextCfg.fnMode + 1) % 3;
                    updateConfigRCU(&nextCfg);
                    printf("Fn mode changed.\n");
                }
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
                if (g_hookThreadId) PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
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

    AppConfig initCfg = {
        .magic = CONFIG_MAGIC,
        .version = 1,
        .keyMainMod = 0x1D,    // LCtrl
        .keyQuickMod = 0x38,   // LAlt
        .keyActivate = 0x5B,   // LWin
        .keySettings = 0x18,   // O
        .keyLeftClick = 0x29,  // ` (Backtick)
        .keyRightClick = 0x00, // Disabled
        .fnMode = FN_MODE_DOWN,
        .topLeftX = g_vLeft,
        .topLeftY = g_vTop,
        .bottomRightX = g_vLeft + g_vWidth,
        .bottomRightY = g_vTop + g_vHeight,
        .trackpadTopLeftX = 0,
        .trackpadTopLeftY = 0,
        .trackpadBottomRightX = 2904,
        .trackpadBottomRightY = 1879
    };
    updateConfigRCU(&initCfg);
    loadSettings();

    // High thread & process priority for minimum scheduling latency
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    g_hHookThread = CreateThread(NULL, 0, hookThreadProc, NULL, 0, &g_hookThreadId);
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
    rid[0].usUsage     = 0x05; // Precision Touchpad
    rid[0].dwFlags     = RIDEV_INPUTSINK;
    rid[0].hwndTarget  = g_hMsgWnd;

    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x06; // Keyboard
    rid[1].dwFlags     = RIDEV_INPUTSINK;
    rid[1].hwndTarget  = g_hMsgWnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) return -1;

    system("cls");
    printBanner();
    puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");

    MSG message;
    while (g_running) {
        // Fast message dispatch loop
        while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                InterlockedExchange(&g_running, 0);
                break;
            }
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        WaitMessage();
    }

    InterlockedExchange(&g_running, 0);
    if (g_hookThreadId) PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hHookThread, 500);
    CloseHandle(g_hHookThread);

    for (int i = 0; i < g_deviceCacheCount; i++) {
        if (g_deviceCache[i].preparsed) {
            free(g_deviceCache[i].preparsed);
        }
    }

    AppConfig* finalCfg = (AppConfig*)InterlockedExchangePointer((void* volatile*)&g_activeCfg, NULL);
    if (finalCfg) free(finalCfg);

    return 0;
}