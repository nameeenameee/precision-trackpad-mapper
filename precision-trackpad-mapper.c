#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <hidsdi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_BUFFER_SIZE 4096
#define OUT_BUFFER_MASK (OUT_BUFFER_SIZE - 1)
#define IN_BUFFER_SIZE  4096
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

// Key Scancodes (0x00 = Unbound / Disabled)
static USHORT keyMainMod    = 0x1D; // LCtrl
static USHORT keyQuickMod   = 0x38; // LAlt
static USHORT keyActivate   = 0x5B; // LWin
static USHORT keySettings   = 0x18; // O
static USHORT keyLeftClick  = 0x29; // ` (Backtick)
static USHORT keyRightClick = 0x00; // Disabled

static volatile int downMainMod    = 0;
static volatile int downQuickMod   = 0;
static volatile int downActivate   = 0;
static volatile int downSettings   = 0;
static volatile int downLeftClick  = 0;
static volatile int downRightClick = 0;

static int leftTouching  = 0;
static int rightTouching = 0;

typedef struct {
    HANDLE deviceHandle;
    PHIDP_PREPARSED_DATA preparsed;
} CachedDevice;

static CachedDevice g_deviceCache[MAX_CACHED_DEVICES];
static int g_deviceCacheCount = 0;

static volatile int fnMode = FN_MODE_DOWN; 
static HHOOK miHook = NULL;

static INPUT outBuffer[OUT_BUFFER_SIZE];
static unsigned outBufferHead = 0;
static unsigned outBufferTail = 0;
static CRITICAL_SECTION queueLock;
static HANDLE queueReady;
static volatile LONG running = 1;

static volatile int mode = MODE_NONE;
static volatile int inSettingsMenu = 0;
static volatile int mouseX = 0;
static volatile int mouseY = 0;

// Screen Bounds & Virtual Screen Cache
static int topLeftX = 0, topLeftY = 0;
static int bottomRightX = 0, bottomRightY = 0;
static int vLeft = 0, vTop = 0, vWidth = 0, vHeight = 0;

// Trackpad Bounds
static long trackpadTopLeftX = 0, trackpadTopLeftY = 0; 
static long trackpadBottomRightX = 2904, trackpadBottomRightY = 1879; 
static volatile long rawX = -1, rawY = -1; 

// Fixed-point scale factors (16.16)
static int64_t mapScaleX = 0;
static int64_t mapScaleY = 0;
static int screenRegionDefined = 1;
static int trackpadRegionDefined = 1;

static HWND g_hWnd = NULL;
static HANDLE g_hWorkerThread = NULL;

void updateScreenMetrics(void) {
    vLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

void recalculateMappingFactors(void) {
    long tpW = trackpadBottomRightX - trackpadTopLeftX;
    long tpH = trackpadBottomRightY - trackpadTopLeftY;
    if (tpW <= 0) tpW = 1;
    if (tpH <= 0) tpH = 1;

    mapScaleX = (((int64_t)(bottomRightX - topLeftX)) << 16) / tpW;
    mapScaleY = (((int64_t)(bottomRightY - topLeftY)) << 16) / tpH;
}

static void getConfigPath(char* outPath, size_t maxLen) {
    DWORD len = GetModuleFileNameA(NULL, outPath, (DWORD)maxLen);
    if (len == 0 || len >= maxLen) {
        strncpy(outPath, ".\\trackpad_config.ini", maxLen);
        return;
    }
    char* lastSlash = strrchr(outPath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        strncat(outPath, "trackpad_config.ini", maxLen - strlen(outPath) - 1);
    } else {
        strncpy(outPath, ".\\trackpad_config.ini", maxLen);
    }
}

void saveSettings(void) {
    char cfg[MAX_PATH];
    char buf[64];
    getConfigPath(cfg, sizeof(cfg));

    sprintf(buf, "%u", keyMainMod);    WritePrivateProfileStringA("Keybinds", "MainMod", buf, cfg);
    sprintf(buf, "%u", keyQuickMod);   WritePrivateProfileStringA("Keybinds", "QuickMod", buf, cfg);
    sprintf(buf, "%u", keyActivate);   WritePrivateProfileStringA("Keybinds", "Activate", buf, cfg);
    sprintf(buf, "%u", keySettings);   WritePrivateProfileStringA("Keybinds", "Settings", buf, cfg);
    sprintf(buf, "%u", keyLeftClick);  WritePrivateProfileStringA("Keybinds", "LeftClick", buf, cfg);
    sprintf(buf, "%u", keyRightClick); WritePrivateProfileStringA("Keybinds", "RightClick", buf, cfg);

    sprintf(buf, "%d", fnMode);        WritePrivateProfileStringA("General", "FnMode", buf, cfg);

    sprintf(buf, "%d", topLeftX);      WritePrivateProfileStringA("Screen", "TopLeftX", buf, cfg);
    sprintf(buf, "%d", topLeftY);      WritePrivateProfileStringA("Screen", "TopLeftY", buf, cfg);
    sprintf(buf, "%d", bottomRightX);  WritePrivateProfileStringA("Screen", "BottomRightX", buf, cfg);
    sprintf(buf, "%d", bottomRightY);  WritePrivateProfileStringA("Screen", "BottomRightY", buf, cfg);

    sprintf(buf, "%ld", trackpadTopLeftX);     WritePrivateProfileStringA("Trackpad", "TopLeftX", buf, cfg);
    sprintf(buf, "%ld", trackpadTopLeftY);     WritePrivateProfileStringA("Trackpad", "TopLeftY", buf, cfg);
    sprintf(buf, "%ld", trackpadBottomRightX); WritePrivateProfileStringA("Trackpad", "BottomRightX", buf, cfg);
    sprintf(buf, "%ld", trackpadBottomRightY); WritePrivateProfileStringA("Trackpad", "BottomRightY", buf, cfg);

    recalculateMappingFactors();
    printf("\n[Settings saved to %s]\n", cfg);
}

void loadSettings(void) {
    char cfg[MAX_PATH];
    getConfigPath(cfg, sizeof(cfg));

    DWORD attr = GetFileAttributesA(cfg);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        recalculateMappingFactors();
        return;
    }

    keyMainMod    = (USHORT)GetPrivateProfileIntA("Keybinds", "MainMod", keyMainMod, cfg);
    keyQuickMod   = (USHORT)GetPrivateProfileIntA("Keybinds", "QuickMod", keyQuickMod, cfg);
    keyActivate   = (USHORT)GetPrivateProfileIntA("Keybinds", "Activate", keyActivate, cfg);
    keySettings   = (USHORT)GetPrivateProfileIntA("Keybinds", "Settings", keySettings, cfg);
    keyLeftClick  = (USHORT)GetPrivateProfileIntA("Keybinds", "LeftClick", keyLeftClick, cfg);
    keyRightClick = (USHORT)GetPrivateProfileIntA("Keybinds", "RightClick", keyRightClick, cfg);

    fnMode = GetPrivateProfileIntA("General", "FnMode", fnMode, cfg);

    topLeftX     = GetPrivateProfileIntA("Screen", "TopLeftX", topLeftX, cfg);
    topLeftY     = GetPrivateProfileIntA("Screen", "TopLeftY", topLeftY, cfg);
    bottomRightX = GetPrivateProfileIntA("Screen", "BottomRightX", bottomRightX, cfg);
    bottomRightY = GetPrivateProfileIntA("Screen", "BottomRightY", bottomRightY, cfg);

    trackpadTopLeftX     = GetPrivateProfileIntA("Trackpad", "TopLeftX", (int)trackpadTopLeftX, cfg);
    trackpadTopLeftY     = GetPrivateProfileIntA("Trackpad", "TopLeftY", (int)trackpadTopLeftY, cfg);
    trackpadBottomRightX = GetPrivateProfileIntA("Trackpad", "BottomRightX", (int)trackpadBottomRightX, cfg);
    trackpadBottomRightY = GetPrivateProfileIntA("Trackpad", "BottomRightY", (int)trackpadBottomRightY, cfg);

    recalculateMappingFactors();
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
        // Drop and replace first entry when full
        free(g_deviceCache[0].preparsed);
        g_deviceCache[0].deviceHandle = hDevice;
        g_deviceCache[0].preparsed = preparsed;
    }
    return preparsed;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_MOUSEMOVE) {
            mouseX = p->pt.x;
            mouseY = p->pt.y;
        }
        if (!(p->flags & LLMHF_INJECTED)) {
            if (mode == MODE_ACTIVE) return 1; // Block hardware mouse events while mapping is active
        }
    }
    return CallNextHookEx(miHook, nCode, wParam, lParam);
}

void setHookState(int enable) {
    if (enable && !miHook) {
        miHook = SetWindowsHookEx(WH_MOUSE_LL, (HOOKPROC)LowLevelMouseProc, GetModuleHandle(NULL), 0);
    } else if (!enable && miHook) {
        UnhookWindowsHookEx(miHook);
        miHook = NULL;
    }
}

int pushBuffer(const INPUT* i) {
    EnterCriticalSection(&queueLock);
    unsigned nextTail = (outBufferTail + 1) & OUT_BUFFER_MASK;
    if (nextTail == outBufferHead) {
        LeaveCriticalSection(&queueLock);
        return -1; // Buffer full; drop frame
    }
    outBuffer[outBufferTail] = *i;
    outBufferTail = nextTail;
    LeaveCriticalSection(&queueLock);
    SetEvent(queueReady);
    return 0;
}

DWORD WINAPI handleQueue(void* arg) {
    (void)arg;
    INPUT batch[64];
    while (InterlockedCompareExchange(&running, 1, 1)) {
        WaitForSingleObject(queueReady, INFINITE);

        while (1) {
            UINT count = 0;
            EnterCriticalSection(&queueLock);
            while (outBufferHead != outBufferTail && count < 64) {
                batch[count++] = outBuffer[outBufferHead];
                outBufferHead = (outBufferHead + 1) & OUT_BUFFER_MASK;
            }
            LeaveCriticalSection(&queueLock);

            if (count == 0) break;
            SendInput(count, batch, sizeof(INPUT));
        }
    }
    return 0;
}

static inline void pushAbsoluteMove(int pixelX, int pixelY) {
    if (vWidth <= 0 || vHeight <= 0) return;

    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    // Map desktop space to normalized 0-65535 coordinates
    ip.mi.dx = (LONG)(((int64_t)(pixelX - vLeft) * 65536) / vWidth);
    ip.mi.dy = (LONG)(((int64_t)(pixelY - vTop) * 65536) / vHeight);
    pushBuffer(&ip);
}

static inline void pressLeft(int down) {
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    pushBuffer(&ip);
}

static inline void pressRight(int down) {
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    pushBuffer(&ip);
}

void showActivate(int state, int ms) {
    pressLeft(0);
    pressRight(0);
    leftTouching = 0;
    rightTouching = 0;
    printf("\n>>> %s DRAWING MODE <<<\n", state ? "ENABLED" : "DISABLED");

    if (state) {
        pushAbsoluteMove(topLeftX, topLeftY);
        Sleep(ms);
        pushAbsoluteMove(bottomRightX, topLeftY);
        Sleep(ms);
        pushAbsoluteMove(bottomRightX, bottomRightY);
        Sleep(ms);
        pushAbsoluteMove(topLeftX, bottomRightY);
        Sleep(ms);
        pushAbsoluteMove(topLeftX, topLeftY);
    }
}

void showConfigMenu(void);

void handleKeyboard(USHORT code, USHORT flags) {
    int down = (flags & 1) ^ 1; 

    if (code != 0 && code == keyMainMod)        downMainMod = down;
    else if (code != 0 && code == keyQuickMod)  downQuickMod = down;
    else if (code != 0 && code == keyLeftClick)  downLeftClick = down;
    else if (code != 0 && code == keyRightClick) downRightClick = down;
    else if (code != 0 && code == keyActivate)  downActivate = down;
    else if (code != 0 && code == keySettings)  downSettings = down;

    if (!down || inSettingsMenu) return;

    if (downMainMod && downSettings) {
        if (mode == MODE_ACTIVE) {
            setHookState(0);
            showActivate(0, 50);
            mode = MODE_NONE;
        }
        showConfigMenu();
        return;
    }

    if (downActivate && downMainMod && !downQuickMod) {
        switch (mode) {
            case MODE_NONE:
            case MODE_FIRST_TRACKPAD_CORNER:
                topLeftX = mouseX; 
                topLeftY = mouseY; 
                mode = MODE_FIRST_SCREEN_CORNER; 
                printf("\n[Calibrate Screen] Point 1: (%d, %d). Move to opposite corner & press shortcut again.\n", topLeftX, topLeftY);
                break;
            case MODE_FIRST_SCREEN_CORNER: 
                if (mouseX < topLeftX) { bottomRightX = topLeftX; topLeftX = mouseX; } 
                else { bottomRightX = mouseX; } 
                if (mouseY < topLeftY) { bottomRightY = topLeftY; topLeftY = mouseY; } 
                else { bottomRightY = mouseY; } 
                screenRegionDefined = 1; 
                recalculateMappingFactors();
                printf("[Calibrate Screen] Done: (%d,%d)-(%d,%d)\n", topLeftX, topLeftY, bottomRightX, bottomRightY);
                saveSettings();
                mode = MODE_NONE; 
                break;
            case MODE_ACTIVE: 
                setHookState(0);
                showActivate(0, 100); 
                mode = MODE_NONE; 
                break;
        }
    } 
    else if (downActivate && downMainMod && downQuickMod) {
        switch (mode) {
            case MODE_NONE:
            case MODE_FIRST_SCREEN_CORNER:
                if (rawX != -1 && rawY != -1) { 
                    trackpadTopLeftX = rawX; 
                    trackpadTopLeftY = rawY; 
                    mode = MODE_FIRST_TRACKPAD_CORNER; 
                    printf("\n[Calibrate Trackpad] Point 1: (%ld, %ld). Place finger on corner & press again.\n", rawX, rawY);
                }
                break;
            case MODE_FIRST_TRACKPAD_CORNER: 
                if (rawX != -1 && rawY != -1) { 
                    if (rawX < trackpadTopLeftX) { trackpadBottomRightX = trackpadTopLeftX; trackpadTopLeftX = rawX; } 
                    else { trackpadBottomRightX = rawX; } 
                    if (rawY < trackpadTopLeftY) { trackpadBottomRightY = trackpadTopLeftY; trackpadTopLeftY = rawY; } 
                    else { trackpadBottomRightY = rawY; } 
                    if (trackpadBottomRightX <= trackpadTopLeftX || trackpadBottomRightY <= trackpadTopLeftY) { 
                        puts("[Error] Trackpad boundaries zero or inverted.");
                    } else {
                        trackpadRegionDefined = 1; 
                        recalculateMappingFactors();
                        printf("[Calibrate Trackpad] Done: (%ld,%ld)-(%ld,%ld)\n", trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
                        saveSettings();
                    }
                    mode = MODE_NONE; 
                }
                break;
            case MODE_ACTIVE: 
                setHookState(0);
                showActivate(0, 100); 
                mode = MODE_NONE; 
                break;
        }
    } 
    else if (downActivate && downQuickMod && !downMainMod) {
        if (mode == MODE_ACTIVE) { 
            setHookState(0);
            showActivate(0, 100); 
            mode = MODE_NONE; 
        } else {
            if (screenRegionDefined && trackpadRegionDefined) { 
                mode = MODE_ACTIVE; 
                leftTouching = 0;
                rightTouching = 0;
                setHookState(1);
                showActivate(1, 150); 
            }
        }
    }
}

LRESULT CALLBACK EventHandler(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT) + IN_BUFFER_SIZE];
    static USAGE usages[32];

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

            ULONG usageValue;
            if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x30, &usageValue, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) { 
                rawX = (long)usageValue; 
            }
            if (HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x31, &usageValue, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) { 
                rawY = (long)usageValue; 
            }

            if (mode != MODE_ACTIVE || inSettingsMenu) 
                return 0; 

            ULONG usageLength = sizeof(usages) / sizeof(USAGE); 
            int touch_detected = 0; 
            if (HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) { 
                for (ULONG j = 0; j < usageLength; j++) { 
                    if (usages[j] == 0x42) { // Tip Switch
                        touch_detected = 1; 
                        break; 
                    }
                }
            }

            int left_down = 0;
            if (keyLeftClick != 0) {
                if (fnMode == FN_MODE_DOWN)        left_down = downLeftClick; 
                else if (fnMode == FN_MODE_LIFT)   left_down = touch_detected && !downLeftClick; 
                else                               left_down = touch_detected; 
            } else {
                left_down = (fnMode == FN_MODE_NONE) ? touch_detected : 0;
            }

            int right_down = (keyRightClick != 0) ? downRightClick : 0;

            if (trackpadRegionDefined && screenRegionDefined && rawX != -1 && rawY != -1) { 
                long rx = rawX < trackpadTopLeftX ? trackpadTopLeftX : (rawX > trackpadBottomRightX ? trackpadBottomRightX : rawX);
                long ry = rawY < trackpadTopLeftY ? trackpadTopLeftY : (rawY > trackpadBottomRightY ? trackpadBottomRightY : rawY);

                int x = topLeftX + (int)((((int64_t)(rx - trackpadTopLeftX) * mapScaleX) + 0x8000) >> 16);
                int y = topLeftY + (int)((((int64_t)(ry - trackpadTopLeftY) * mapScaleY) + 0x8000) >> 16);

                pushAbsoluteMove(x, y);
            }

            if (left_down != leftTouching) { 
                pressLeft(left_down); 
                leftTouching = left_down; 
            }
            if (right_down != rightTouching) { 
                pressRight(right_down); 
                rightTouching = right_down; 
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
    char strMain[64], strQuick[64], strAct[64], strSet[64];
    getKeyDisplayString(keyMainMod, strMain, sizeof(strMain));
    getKeyDisplayString(keyQuickMod, strQuick, sizeof(strQuick));
    getKeyDisplayString(keyActivate, strAct, sizeof(strAct));
    getKeyDisplayString(keySettings, strSet, sizeof(strSet));

    puts("===================================================================");
    puts("              Precision Trackpad Mapper                            ");
    puts("===================================================================");
    puts(" Current Bindings:");
    printKeybind("[Main Modifier]", keyMainMod);
    printKeybind("[Quick Modifier]", keyQuickMod);
    printKeybind("[Activate Key]", keyActivate);
    printKeybind("[Settings Key]", keySettings);
    printKeybind("[Left Click / Fn]", keyLeftClick);
    printKeybind("[Right Click]", keyRightClick);
    puts(" Operational Hotkeys:");
    printf("   Open Settings     : %s + %s\n", strMain, strSet);
    printf("   Define Screen Box : %s + %s\n", strMain, strAct);
    printf("   Define Trackpad   : %s + %s + %s\n", strMain, strQuick, strAct);
    printf("   Toggle Mode       : %s + %s\n", strQuick, strAct);
    puts("-------------------------------------------------------------------");
    printf(" Status: Screen (%d,%d)-(%d,%d) | Trackpad (%ld,%ld)-(%ld,%ld)\n",
           topLeftX, topLeftY, bottomRightX, bottomRightY,
           trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
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
                    *targetKey = 0;
                    printf("[%s] has been unbound / disabled.\n", actionName);
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    Sleep(200);
                    return;
                }
                UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                if (scancode != 0) {
                    *targetKey = (USHORT)scancode;
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
    inSettingsMenu = 1;
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
        puts(" 9. Save Current Settings to INI");
        puts(" 10. Return to Mapping Service");
        puts(" 11. Exit Program");
        printf("\nSelect an option [1-11]: ");

        if (!fgets(choice, sizeof(choice), stdin)) continue;
        int opt = atoi(choice);

        switch (opt) {
            case 1: captureKeybind("Main Modifier", &keyMainMod); break;
            case 2: captureKeybind("Quick Modifier", &keyQuickMod); break;
            case 3: captureKeybind("Activate Key", &keyActivate); break;
            case 4: captureKeybind("Settings Key", &keySettings); break;
            case 5: captureKeybind("Left Click / Fn Key", &keyLeftClick); break;
            case 6: captureKeybind("Right Click Key", &keyRightClick); break;
            case 7: {
                long x1, y1, x2, y2;
                printf("Enter trackpad bounds (minX minY maxX maxY): ");
                if (scanf("%ld %ld %ld %ld", &x1, &y1, &x2, &y2) == 4 && x1 < x2 && y1 < y2) { 
                    trackpadTopLeftX = x1; trackpadTopLeftY = y1; 
                    trackpadBottomRightX = x2; trackpadBottomRightY = y2; 
                    trackpadRegionDefined = 1; 
                    recalculateMappingFactors();
                    puts("Trackpad boundaries updated.");
                } else {
                    puts("Invalid values entered.");
                }
                while (getchar() != '\n');
                break;
            }
            case 8: {
                fnMode = (fnMode + 1) % 3;
                printf("Fn mode changed to: %s\n", 
                    fnMode == FN_MODE_DOWN ? "FN_MODE_DOWN" : (fnMode == FN_MODE_LIFT ? "FN_MODE_LIFT" : "FN_MODE_NONE"));
                Sleep(400);
                break;
            }
            case 9:
                saveSettings();
                Sleep(300);
                break;
            case 10:
                saveSettings();
                inSettingsMenu = 0;
                downMainMod = downQuickMod = downActivate = downSettings = 0;
                system("cls");
                printBanner();
                puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");
                return;
            case 11:
                InterlockedExchange(&running, 0);
                if (g_hWnd) PostMessage(g_hWnd, WM_CLOSE, 0, 0);
                return;
            default:
                break;
        }
    }
}

static BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    (void)dwCtrlType;
    InterlockedExchange(&running, 0);
    setHookState(0);
    if (g_hWnd) PostMessage(g_hWnd, WM_CLOSE, 0, 0);
    return TRUE;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    const char* class_name = "precision-trackpad-mapper-class";

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    updateScreenMetrics();

    topLeftX = vLeft;
    topLeftY = vTop;
    bottomRightX = topLeftX + vWidth;
    bottomRightY = topLeftY + vHeight;

    loadSettings();
    InitializeCriticalSection(&queueLock);

    WNDCLASS window_class = {0};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = class_name;
    if (!RegisterClass(&window_class)) return -1; 

    g_hWnd = CreateWindow(class_name, "Precision Trackpad Engine", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0); 
    if (!g_hWnd) return -1; 

    // Register Touchpad and Keyboard devices safely
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x0D; 
    rid[0].usUsage     = 0x05;             // Touchpad
    rid[0].dwFlags     = RIDEV_INPUTSINK; 
    rid[0].hwndTarget  = g_hWnd; 

    rid[1].usUsagePage = 0x01; 
    rid[1].usUsage     = 0x06;             // Keyboard
    rid[1].dwFlags     = RIDEV_INPUTSINK;  // FIXED: Removed RIDEV_NOLEGACY
    rid[1].hwndTarget  = g_hWnd; 

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) return -1;

    queueReady = CreateEvent(NULL, FALSE, FALSE, NULL); 
    g_hWorkerThread = CreateThread(NULL, 0, handleQueue, NULL, 0, NULL); 
    if (!g_hWorkerThread) return -1; 

    system("cls");
    printBanner();
    puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");

    MSG message;
    while (InterlockedCompareExchange(&running, 1, 1) && GetMessage(&message, NULL, 0, 0)) { 
        TranslateMessage(&message); 
        DispatchMessage(&message); 
    }

    // Teardown
    InterlockedExchange(&running, 0);
    setHookState(0);
    SetEvent(queueReady); 
    WaitForSingleObject(g_hWorkerThread, 500); 

    CloseHandle(g_hWorkerThread); 
    CloseHandle(queueReady); 
    DeleteCriticalSection(&queueLock);

    for (int i = 0; i < g_deviceCacheCount; i++) {
        if (g_deviceCache[i].preparsed) {
            free(g_deviceCache[i].preparsed);
        }
    }

    return 0; 
}