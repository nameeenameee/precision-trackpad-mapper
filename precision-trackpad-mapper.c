#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <hidsdi.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_BUFFER_SIZE 4096
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

// Default Key Scancodes
static USHORT keyMainMod   = 0x1D; // LCtrl[cite: 1]
static USHORT keyQuickMod  = 0x38; // LAlt[cite: 1]
static USHORT keyActivate  = 0x5B; // LWin[cite: 1]
static USHORT keyDrawFn    = 0x29; // Backtick `[cite: 1]

#define INPUT_WAIT 1234

typedef struct {
    HANDLE deviceHandle;
    PHIDP_PREPARSED_DATA preparsed;
} CachedDevice;

static CachedDevice g_deviceCache[MAX_CACHED_DEVICES];
static int g_deviceCacheCount = 0;

static int fnMode = FN_MODE_DOWN; 
static HHOOK miHook = NULL;
static int touching = 0;
static int drawX = -1;
static int drawY = -1;

static INPUT outBuffer[OUT_BUFFER_SIZE];
static unsigned outBufferHead = 0;
static unsigned outBufferTail = 0;
static CRITICAL_SECTION queueLock;
static HANDLE queueReady;
static volatile char running = 1;

static int downMainMod = 0;
static int downQuickMod = 0;
static int downActivate = 0;
static int downFn = 0;

static int mode = MODE_NONE;
static int mouseX = 0;
static int mouseY = 0;
static int topLeftX = 0;
static int topLeftY = 0;
static int bottomRightX = 0;
static int bottomRightY = 0;

static long trackpadTopLeftX = 0, trackpadTopLeftY = 0; 
static long trackpadBottomRightX = 2904, trackpadBottomRightY = 1879; 
static long rawX = -1, rawY = -1; 

static int screenRegionDefined = 1;
static int trackpadRegionDefined = 1;

// Function Prototypes
void pressMouse(int down);
void moveMouse(int x, int y);
void delay(unsigned ms);
int processOptions(char* cmdLine);
void help(void);
void printBanner(void);
void showConfigMenu(void);
void captureKeybind(const char* actionName, USHORT* targetKey);
LRESULT CALLBACK EventHandler(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
DWORD WINAPI handleQueue(void* arg);
int popBuffer(INPUT* ip);
int pushBuffer(const INPUT* i);
void showActivate(int state, int ms);
void handleKeyboard(USHORT code, USHORT flags);
PHIDP_PREPARSED_DATA getOrCachePreparsed(HANDLE hDevice);
void setHookState(int enable);

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
    }
    return preparsed;
}

void setHookState(int enable) {
    if (enable && !miHook) {
        miHook = SetWindowsHookEx(WH_MOUSE_LL, (HOOKPROC)LowLevelMouseProc, GetModuleHandle(NULL), 0);
    } else if (!enable && miHook) {
        UnhookWindowsHookEx(miHook);
        miHook = NULL;
    }
}

int popBuffer(INPUT* ip) {
    EnterCriticalSection(&queueLock);
    if (outBufferHead == outBufferTail) {
        LeaveCriticalSection(&queueLock);
        return -1;
    }
    *ip = outBuffer[outBufferHead];
    outBufferHead = (outBufferHead + 1) % OUT_BUFFER_SIZE;
    LeaveCriticalSection(&queueLock);
    return 0;
}

int pushBuffer(const INPUT* i) {
    EnterCriticalSection(&queueLock);
    if (outBufferTail != outBufferHead) {
        unsigned prevIndex = (outBufferTail + OUT_BUFFER_SIZE - 1) % OUT_BUFFER_SIZE;
        if (outBuffer[prevIndex].type == INPUT_MOUSE && i->type == INPUT_MOUSE) {
            if ((outBuffer[prevIndex].mi.dwFlags & ~(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) == 0 &&
                (i->mi.dwFlags & ~(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) == 0) {
                outBuffer[prevIndex] = *i;
                LeaveCriticalSection(&queueLock);
                return 0;
            }
        }
    }

    unsigned newTail = (outBufferTail + 1) % OUT_BUFFER_SIZE;
    if (newTail == outBufferHead) {
        LeaveCriticalSection(&queueLock);
        return -1;
    }
    outBuffer[outBufferTail] = *i;
    outBufferTail = newTail;
    LeaveCriticalSection(&queueLock);
    SetEvent(queueReady);
    return 0;
}

DWORD WINAPI handleQueue(void* arg) {
    (void)arg;
    INPUT i;
    while (running) {
        WaitForSingleObject(queueReady, INFINITE);
        while (running && popBuffer(&i) >= 0) {
            if (i.type == INPUT_WAIT) {
                Sleep(i.hi.uMsg);
            } else {
                SendInput(1, &i, sizeof(INPUT));
            }
        }
    }
    return 0;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_MOUSEMOVE) {
            mouseX = p->pt.x;
            mouseY = p->pt.y;
        }
        if (!(p->flags & LLMHF_INJECTED)) {
            if (mode == MODE_ACTIVE) return 1;
        }
    }
    return CallNextHookEx(miHook, nCode, wParam, lParam);
}

void moveMouse(int x, int y) {
    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vWidth <= 0 || vHeight <= 0) return;

    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dx = (LONG)(((double)(x - vLeft) * 65535.0) / (vWidth - 1) + 0.5);
    ip.mi.dy = (LONG)(((double)(y - vTop) * 65535.0) / (vHeight - 1) + 0.5);
    ip.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    pushBuffer(&ip);
}

void pressMouse(int down) {
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    pushBuffer(&ip);
}

void delay(unsigned ms) {
    INPUT ip = {0};
    ip.type = INPUT_WAIT;
    ip.hi.uMsg = ms;
    pushBuffer(&ip);
}

void showActivate(int state, int ms) {
    pressMouse(0); 
    printf("\n>>> %s DRAWING MODE <<<\n", state ? "ENABLED" : "DISABLED");

    if (state) {
        moveMouse(topLeftX, topLeftY); 
        delay(ms); 
        moveMouse(bottomRightX, topLeftY); 
        delay(ms); 
        moveMouse(bottomRightX, bottomRightY); 
        delay(ms); 
        moveMouse(topLeftX, bottomRightY); 
        delay(ms); 
        moveMouse(topLeftX, topLeftY); 
    }
}

void handleKeyboard(USHORT code, USHORT flags) {
    int down = (flags & 1) ^ 1; 
    if (code == keyMainMod)        downMainMod = down;
    else if (code == keyQuickMod)  downQuickMod = down;
    else if (code == keyDrawFn)    downFn = down;
    else if (code == keyActivate)  downActivate = down;

    if (!down) return; 

    // Set Screen Area (MainMod + Activate)
    if (downActivate && downMainMod && !downQuickMod) {
        switch (mode) {
            case MODE_NONE:
            case MODE_FIRST_TRACKPAD_CORNER:
                topLeftX = mouseX; 
                topLeftY = mouseY; 
                mode = MODE_FIRST_SCREEN_CORNER; 
                printf("\n[Calibrate Screen] Point 1 set to (%d, %d). Move cursor to opposite corner and press again.\n", topLeftX, topLeftY);
                break;
            case MODE_FIRST_SCREEN_CORNER: 
                if (mouseX < topLeftX) { bottomRightX = topLeftX; topLeftX = mouseX; } 
                else { bottomRightX = mouseX; } 
                if (mouseY < topLeftY) { bottomRightY = topLeftY; topLeftY = mouseY; } 
                else { bottomRightY = mouseY; } 
                screenRegionDefined = 1; 
                printf("[Calibrate Screen] Complete: Screen mapped to (%d,%d)-(%d,%d)\n", topLeftX, topLeftY, bottomRightX, bottomRightY);
                mode = MODE_NONE; 
                break;
            case MODE_ACTIVE: 
                setHookState(0);
                showActivate(0, 100); 
                mode = MODE_NONE; 
                break;
        }
    } 
    // Set Trackpad Area (MainMod + QuickMod + Activate)
    else if (downActivate && downMainMod && downQuickMod) {
        switch (mode) {
            case MODE_NONE:
            case MODE_FIRST_SCREEN_CORNER:
                if (rawX != -1 && rawY != -1) { 
                    trackpadTopLeftX = rawX; 
                    trackpadTopLeftY = rawY; 
                    mode = MODE_FIRST_TRACKPAD_CORNER; 
                    printf("\n[Calibrate Trackpad] Point 1: (%ld, %ld). Touch opposite corner and press combination again.\n", rawX, rawY);
                } else {
                    printf("\n[Warning] Keep your finger on the trackpad while setting corner!\n");
                }
                break;
            case MODE_FIRST_TRACKPAD_CORNER: 
                if (rawX != -1 && rawY != -1) { 
                    if (rawX < trackpadTopLeftX) { trackpadBottomRightX = trackpadTopLeftX; trackpadTopLeftX = rawX; } 
                    else { trackpadBottomRightX = rawX; } 
                    if (rawY < trackpadTopLeftY) { trackpadBottomRightY = trackpadTopLeftY; trackpadTopLeftY = rawY; } 
                    else { trackpadBottomRightY = rawY; } 
                    if (trackpadBottomRightX <= trackpadTopLeftX || trackpadBottomRightY <= trackpadTopLeftY) { 
                        puts("[Error] Defined trackpad boundaries have zero or inverted size. Reverted.");
                    } else {
                        trackpadRegionDefined = 1; 
                        printf("[Calibrate Trackpad] Complete: Range mapped to (%ld,%ld)-(%ld,%ld)\n", trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
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
    // Toggle Active Mode (QuickMod + Activate)
    else if (downActivate && downQuickMod && !downMainMod) {
        if (mode == MODE_ACTIVE) { 
            setHookState(0);
            showActivate(0, 100); 
            mode = MODE_NONE; 
        } else {
            if (screenRegionDefined && trackpadRegionDefined) { 
                mode = MODE_ACTIVE; 
                touching = 0; 
                setHookState(1);
                showActivate(1, 200); 
                drawX = -1; 
                drawY = -1; 
            } else {
                puts("\n[!] Setup incomplete: Both screen and trackpad boundaries must be defined.");
            }
        }
    }
}

LRESULT CALLBACK EventHandler(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT) + IN_BUFFER_SIZE];
    static USAGE usages[64];

    switch (event) {
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

            if (mode != MODE_ACTIVE) 
                return 0; 

            ULONG usageLength = sizeof(usages) / sizeof(USAGE); 
            int touch_detected = 0; 
            if (HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) { 
                for (ULONG j = 0; j < usageLength; j++) { 
                    if (usages[j] == 0x42) { 
                        touch_detected = 1; 
                        break; 
                    }
                }
            }

            int pen_down = 0; 
            if (fnMode == FN_MODE_DOWN) pen_down = downFn; 
            else if (fnMode == FN_MODE_LIFT) pen_down = touch_detected && !downFn; 
            else pen_down = touch_detected; 

            if (trackpadRegionDefined && screenRegionDefined && rawX != -1 && rawY != -1) { 
                long rx = rawX < trackpadTopLeftX ? trackpadTopLeftX : (rawX > trackpadBottomRightX ? trackpadBottomRightX : rawX);
                long ry = rawY < trackpadTopLeftY ? trackpadTopLeftY : (rawY > trackpadBottomRightY ? trackpadBottomRightY : rawY);

                double normX = (double)(rx - trackpadTopLeftX) / (double)(trackpadBottomRightX - trackpadTopLeftX);
                double normY = (double)(ry - trackpadTopLeftY) / (double)(trackpadBottomRightY - trackpadTopLeftY);

                int x = topLeftX + (int)(normX * (bottomRightX - topLeftX) + 0.5);
                int y = topLeftY + (int)(normY * (bottomRightY - topLeftY) + 0.5);

                if (x != drawX || y != drawY) { 
                    moveMouse(x, y); 
                    drawX = x; 
                    drawY = y; 
                }
            }

            if (pen_down != touching) { 
                pressMouse(pen_down); 
                touching = pen_down; 
            }
            return 0; 
        }
    }
    return DefWindowProc(hwnd, event, wparam, lparam); 
}

void printBanner(void) {
    puts("===================================================================");
    puts("                    Precision Trackpad Mapper                      ");
    puts("===================================================================");
    printf(" Current Bindings:\n");
    printf("   [Main Modifier]   : Scancode 0x%02X (Default: L-Ctrl 0x1D)\n", keyMainMod);
    printf("   [Quick Modifier]  : Scancode 0x%02X (Default: L-Alt  0x38)\n", keyQuickMod);
    printf("   [Activate Key]    : Scancode 0x%02X (Default: L-Win  0x5B)\n", keyActivate);
    printf("   [Draw/Fn Key]     : Scancode 0x%02X (Default: `      0x29)\n", keyDrawFn);
    printf(" Operational Hotkeys:\n");
    printf("   Define Screen Box : <Main Mod>  + <Activate>\n");
    printf("   Define Trackpad   : <Main Mod>  + <Quick Mod> + <Activate>\n");
    printf("   Toggle Mode       : <Quick Mod> + <Activate>\n");
    puts("-------------------------------------------------------------------");
    printf(" Status: Screen (%d,%d)-(%d,%d) | Trackpad (%ld,%ld)-(%ld,%ld)\n",
           topLeftX, topLeftY, bottomRightX, bottomRightY,
           trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
    puts("===================================================================");
}

void captureKeybind(const char* actionName, USHORT* targetKey) {
    printf("\nPress any key to bind to [%s] (Escape to cancel)...\n", actionName);
    while (1) {
        for (int vk = 8; vk <= 255; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (vk == VK_ESCAPE) {
                    puts("Key binding cancelled.");
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    return;
                }
                UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                if (scancode != 0) {
                    *targetKey = (USHORT)scancode;
                    printf("Assigned scancode 0x%02X to [%s].\n", *targetKey, actionName);
                    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
                    return;
                }
            }
        }
        Sleep(10);
    }
}

void showConfigMenu(void) {
    char choice[16];
    while (1) {
        system("cls");
        printBanner();
        puts("\nConfiguration Menu:");
        puts(" 1. Rebind [Main Modifier]");
        puts(" 2. Rebind [Quick Modifier]");
        puts(" 3. Rebind [Activate Key]");
        puts(" 4. Rebind [Draw/Fn Key]");
        puts(" 5. Calibrate Trackpad Manually");
        puts(" 6. Switch Pen Mode (Down / Lift / None)");
        puts(" 7. Start Mapping Service");
        puts(" 8. Exit");
        printf("\nSelect an option [1-8]: ");

        if (!fgets(choice, sizeof(choice), stdin)) continue;
        int opt = atoi(choice);

        switch (opt) {
            case 1: captureKeybind("Main Modifier", &keyMainMod); break;
            case 2: captureKeybind("Quick Modifier", &keyQuickMod); break;
            case 3: captureKeybind("Activate Key", &keyActivate); break;
            case 4: captureKeybind("Draw/Fn Key", &keyDrawFn); break;
            case 5: {
                long x1, y1, x2, y2;
                printf("Enter trackpad bounds (minX minY maxX maxY): ");
                if (scanf("%ld %ld %ld %ld", &x1, &y1, &x2, &y2) == 4 && x1 < x2 && y1 < y2) { 
                    trackpadTopLeftX = x1; trackpadTopLeftY = y1; 
                    trackpadBottomRightX = x2; trackpadBottomRightY = y2; 
                    trackpadRegionDefined = 1; 
                    puts("Trackpad boundaries updated.");
                } else {
                    puts("Invalid values entered.");
                }
                while (getchar() != '\n');
                break;
            }
            case 6: {
                fnMode = (fnMode + 1) % 3;
                printf("Fn mode changed to: %s\n", 
                    fnMode == FN_MODE_DOWN ? "FN_MODE_DOWN" : (fnMode == FN_MODE_LIFT ? "FN_MODE_LIFT" : "FN_MODE_NONE"));
                Sleep(1000);
                break;
            }
            case 7:
                return;
            case 8:
                exit(0);
            default:
                break;
        }
    }
}

void help(void) {
    MessageBox(0, 
        "Precision Trackpad Mapper\n\n"
        "CLI Options:\n"
        "--config / -c            : Launch interactive configurator\n"
        "--fn-lift                : Lift pen when ` is pressed\n" 
        "--fn-down                : Lower pen when ` is pressed [default]\n" 
        "--fn-none                : Ignore ` key\n" 
        "--bind-main <scancode>   : Set Main modifier scancode (hex or dec)\n"
        "--bind-quick <scancode>  : Set Quick modifier scancode\n"
        "--bind-activate <scancode>: Set Activate key scancode\n"
        "--bind-draw <scancode>   : Set Draw key scancode\n"
        "--window L,T,R,B         : Set target screen bounds\n" 
        "--trackpad X1,Y1,X2,Y2   : Set trackpad raw bounds\n", 
        "Help", 0); 
}

int processOptions(char* cmdLine) {
    char* token;
    char* src = cmdLine;
    while (NULL != (token = strtok(src, " "))) { 
        src = NULL; 
        if (!strcmp(token, "--config") || !strcmp(token, "-c")) {
            showConfigMenu();
        } else if (!strcmp(token, "--fn-lift")) { 
            fnMode = FN_MODE_LIFT; 
        } else if (!strcmp(token, "--fn-down")) { 
            fnMode = FN_MODE_DOWN; 
        } else if (!strcmp(token, "--fn-none")) { 
            fnMode = FN_MODE_NONE; 
        } else if (!strcmp(token, "--bind-main")) {
            if (!(token = strtok(NULL, " "))) return 0;
            keyMainMod = (USHORT)strtoul(token, NULL, 0);
        } else if (!strcmp(token, "--bind-quick")) {
            if (!(token = strtok(NULL, " "))) return 0;
            keyQuickMod = (USHORT)strtoul(token, NULL, 0);
        } else if (!strcmp(token, "--bind-activate")) {
            if (!(token = strtok(NULL, " "))) return 0;
            keyActivate = (USHORT)strtoul(token, NULL, 0);
        } else if (!strcmp(token, "--bind-draw")) {
            if (!(token = strtok(NULL, " "))) return 0;
            keyDrawFn = (USHORT)strtoul(token, NULL, 0);
        } else if (!strcmp(token, "--window")) { 
            if (!(token = strtok(NULL, " "))) return 0; 
            sscanf(token, "%d,%d,%d,%d", &topLeftX, &topLeftY, &bottomRightX, &bottomRightY); 
            screenRegionDefined = 1; 
        } else if (!strcmp(token, "--trackpad")) { 
            if (!(token = strtok(NULL, " "))) return 0; 
            sscanf(token, "%ld,%ld,%ld,%ld", &trackpadTopLeftX, &trackpadTopLeftY, &trackpadBottomRightX, &trackpadBottomRightY); 
            trackpadRegionDefined = 1; 
        } else if (!strcmp(token, "--help") || !strcmp(token, "-h")) { 
            help(); 
            return 0; 
        }
    }
    return 1; 
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    const char* class_name = "precision-trackpad-mapper-class";

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initial setup with screen coordinates
    topLeftX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    topLeftY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bottomRightX = topLeftX + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bottomRightY = topLeftY + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    char* cmdLine = GetCommandLineA();
    // Skip executable path in command line
    if (*cmdLine == '"') {
        cmdLine++;
        while (*cmdLine && *cmdLine != '"') cmdLine++;
        if (*cmdLine == '"') cmdLine++;
    } else {
        while (*cmdLine && *cmdLine != ' ') cmdLine++;
    }
    while (*cmdLine == ' ') cmdLine++;

    if (!processOptions(cmdLine)) return 0;

    InitializeCriticalSection(&queueLock);

    WNDCLASS window_class = {0};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = class_name;
    if (!RegisterClass(&window_class)) return -1; 

    HWND window = CreateWindow(class_name, "Precision Trackpad Engine", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0); 
    if (!window) return -1; 

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x0D; 
    rid[0].usUsage = 0x05;     // Touchpad[cite: 1]
    rid[0].dwFlags = RIDEV_INPUTSINK; 
    rid[0].hwndTarget = window; 

    rid[1].usUsagePage = 0x01; 
    rid[1].usUsage = 0x06;     // Keyboard[cite: 1]
    rid[1].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY; 
    rid[1].hwndTarget = window; 

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) return -1;

    queueReady = CreateEvent(NULL, FALSE, FALSE, NULL); 
    HANDLE queueThread = CreateThread(NULL, 0, handleQueue, NULL, 0, NULL); 
    if (!queueThread) return -1; 

    system("cls");
    printBanner();
    puts("\n[Engine Running] Listening for inputs. Press Ctrl+C in this window to quit.");

    MSG message;
    while (GetMessage(&message, NULL, 0, 0)) { 
        TranslateMessage(&message); 
        DispatchMessage(&message); 
    }

    running = 0;
    SetEvent(queueReady); 
    WaitForSingleObject(queueThread, INFINITE); 
    CloseHandle(queueThread); 
    CloseHandle(queueReady); 
    setHookState(0);
    DeleteCriticalSection(&queueLock);

    for (int i = 0; i < g_deviceCacheCount; i++) {
        free(g_deviceCache[i].preparsed);
    }

    return (int)message.wParam; 
}