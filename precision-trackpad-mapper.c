#define _WIN32_WINNT 0x0A00
#include <Windows.h>
#include <hidsdi.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// Function prototypes
void pressMouse(int down);
void moveMouse(int x, int y);
void delay(unsigned ms);
int processOptions(char* cmdLine);
void help();
LRESULT CALLBACK EventHandler(HWND hwnd, unsigned event, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
DWORD WINAPI handleQueue(void* arg);
DWORD readRegistry(HKEY key, char* path, char* value, DWORD defaultValue);
int popBuffer(INPUT* ip);
int pushBuffer(INPUT* i);
int haveValueCap(HIDP_VALUE_CAPS* cap, unsigned usagePage, unsigned usage);
int haveButtonCap(HIDP_BUTTON_CAPS* cap, unsigned usagePage, unsigned usage);
long getScaledRegion(long rawValue, long trackpadMin, long trackpadMax, unsigned screenScale);
void showActivate(int state, int ms);
void handleKeyboard(USHORT code, USHORT flags);
void promptTrackpadRegion(void);

#define OUT_BUFFER_SIZE 4096
#define IN_BUFFER_SIZE  4096

enum {
    FN_MODE_NONE = 0,
    FN_MODE_LIFT,
    FN_MODE_DOWN
};

int fnMode = FN_MODE_DOWN;
HHOOK miHook;
int touching;
int drawX;
int drawY;

INPUT outBuffer[OUT_BUFFER_SIZE];
unsigned outBufferHead;
unsigned outBufferTail;

#define KEY_MAIN_ACTIVATE_MOD  0x1D
#define KEY_QUICK_ACTIVATE_MOD 0x38
#define KEY_ACTIVATE           0x5B
#define KEY_BACKTICK 0x29

int downMainMod = 0;
int downQuickMod = 0;
int downActivate = 0;
int downFn = 0;

#define INPUT_WAIT 1234

enum {
    MODE_NONE = 0,
    MODE_FIRST_SCREEN_CORNER = 1,
    MODE_FIRST_TRACKPAD_CORNER = 2,
    MODE_ACTIVE = 3
};

int mode = 0;
int mouseX = 0;
int mouseY = 0;
int topLeftX;
int topLeftY;
int bottomRightX;
int bottomRightY;

long trackpadTopLeftX = -1, trackpadTopLeftY = -1;
long trackpadBottomRightX = -1, trackpadBottomRightY = -1;
long rawX = -1, rawY = -1;

int screenRegionDefined = 0;
int trackpadRegionDefined = 0;

HANDLE queueReady;
char running = 1;

// ---------- Function implementations ----------
int popBuffer(INPUT* ip) {
    if (outBufferHead == outBufferTail)
        return -1;
    *ip = outBuffer[outBufferHead];
    outBufferHead = (outBufferHead+1) % OUT_BUFFER_SIZE;
    return 0;
}

int pushBuffer(INPUT* i) {
    unsigned newTail = (outBufferTail+1) % OUT_BUFFER_SIZE;
    if (newTail == outBufferHead)
        return -1;
    outBuffer[outBufferTail] = *i;
    outBufferTail = newTail;
    SetEvent(queueReady);
    return 0;
}

DWORD WINAPI handleQueue(void* arg) {
    (void)arg;
    INPUT i;
    while(running) {
        WaitForSingleObject(queueReady, INFINITE);
        while (running && popBuffer(&i) >= 0) {
            if (i.type == INPUT_WAIT) {
                Sleep(i.hi.uMsg);
            }
            else {
                SendInput(1,&i,sizeof(INPUT));
            }
        }
    }
    ExitThread(0);
    return 0;
}

DWORD readRegistry(HKEY key, char* path, char* value, DWORD defaultValue) {
    DWORD out;
    DWORD length = sizeof(DWORD);
    LONG result = RegGetValue(key, path, value, RRF_RT_DWORD, NULL, &out, &length);
    if (ERROR_SUCCESS != result)
        return defaultValue;
    else
        return out;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if(nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
        if (wParam == WM_MOUSEMOVE) {
            mouseX = p->pt.x;
            mouseY = p->pt.y;
        }
        if (p->flags & LLMHF_INJECTED) {
            // allow injected
        }
        else {
            if (mode == MODE_ACTIVE)
                return 1;
        }
    }
    return CallNextHookEx(miHook, nCode, wParam, lParam);
}

int haveValueCap(HIDP_VALUE_CAPS* cap, unsigned usagePage, unsigned usage) {
    if (cap->UsagePage != usagePage)
        return 0;
    if (cap->IsRange) {
        return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
    }
    else {
        return cap->NotRange.Usage == usage;
    }
}

int haveButtonCap(HIDP_BUTTON_CAPS* cap, unsigned usagePage, unsigned usage) {
    if (cap->UsagePage != usagePage)
        return 0;
    if (cap->IsRange) {
        return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
    }
    else {
        return cap->NotRange.Usage == usage;
    }
}

long getScaledRegion(long rawValue, long trackpadMin, long trackpadMax, unsigned screenScale) {
    if (trackpadMax <= trackpadMin)
        return -1;
    if (rawValue < trackpadMin) rawValue = trackpadMin;
    if (rawValue > trackpadMax) rawValue = trackpadMax;
    long trackpadRange = trackpadMax - trackpadMin;
    return (long)(((LONGLONG)screenScale * (rawValue - trackpadMin) + trackpadRange / 2) / trackpadRange);
}

void moveMouse(int x, int y) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    INPUT ip;
    ip.type = INPUT_MOUSE;
    ip.mi.dx = x*65536/GetSystemMetrics(SM_CXSCREEN);
    ip.mi.dy = y*65536/GetSystemMetrics(SM_CYSCREEN);
    ip.mi.mouseData = 0;
    ip.mi.dwFlags = MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
    ip.mi.time = 0;
    pushBuffer(&ip);
}

void pressMouse(int down) {
    INPUT ip;
    ip.type = INPUT_MOUSE;
    ip.mi.dx = 0;
    ip.mi.dy = 0;
    ip.mi.mouseData = 0;
    ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ip.mi.time = 0;
    pushBuffer(&ip);
}

void delay(unsigned ms) {
    INPUT ip;
    ip.type = INPUT_WAIT;
    ip.hi.uMsg = ms;
    pushBuffer(&ip);
}

void showActivate(int state, int ms) {
    pressMouse(0);
    if(state)
        puts("--- Activating drawing mode ---");
    else
        puts("--- Disabling drawing mode ---");

    if (!screenRegionDefined) {
         puts("(Warning: Screen region not defined, using full screen)");
         topLeftX = 0;
         topLeftY = 0;
         bottomRightX = GetSystemMetrics(SM_CXSCREEN);
         bottomRightY = GetSystemMetrics(SM_CYSCREEN);
         screenRegionDefined = 1;
    }
    if (!trackpadRegionDefined) {
         puts("(Warning: Trackpad region not defined)");
    }

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
    else {
        moveMouse(bottomRightX, bottomRightY);
        delay(ms);
        moveMouse(bottomRightX, topLeftY);
        delay(ms);
        moveMouse(topLeftX, topLeftY);
        delay(ms);
        moveMouse(topLeftX, bottomRightY);
        delay(ms);
        moveMouse(bottomRightX, bottomRightY);
    }
}

void handleKeyboard(USHORT code, USHORT flags) {
    int down = (flags & 1) ^ 1;
    if (code == KEY_MAIN_ACTIVATE_MOD)
        downMainMod = down;
    else if (code == KEY_QUICK_ACTIVATE_MOD)
        downQuickMod = down;
    else if (code == KEY_BACKTICK)
        downFn = down;
    else if (code == KEY_ACTIVATE)
        downActivate = down;

    if (!down) return;

    if (downActivate && downMainMod && !downQuickMod) {
        switch(mode) {
            case MODE_NONE:
            case MODE_FIRST_TRACKPAD_CORNER:
                topLeftX = mouseX;
                topLeftY = mouseY;
                mode = MODE_FIRST_SCREEN_CORNER;
                puts("Screen Corner 1 set. Press Ctrl+Win again at the other corner.");
                break;
            case MODE_FIRST_SCREEN_CORNER:
                if (mouseX < topLeftX) {
                    bottomRightX = topLeftX;
                    topLeftX = mouseX;
                } else {
                    bottomRightX = mouseX;
                }
                if (mouseY < topLeftY) {
                    bottomRightY = topLeftY;
                    topLeftY = mouseY;
                } else {
                    bottomRightY = mouseY;
                }
                screenRegionDefined = 1;
                printf("Screen area defined: (%d,%d)-(%d,%d)\n", topLeftX, topLeftY, bottomRightX, bottomRightY);
                if (trackpadRegionDefined) {
                    puts("Both regions defined. Press Alt+Win to activate drawing.");
                    mode = MODE_NONE;
                } else {
                    puts("Screen area set. Now define trackpad region with Ctrl+Alt+Win.");
                    mode = MODE_NONE;
                }
                break;
             case MODE_ACTIVE:
                showActivate(0, 100);
                mode = MODE_NONE;
                puts("Exited drawing mode.");
                puts("Define Screen: Ctrl+Win | Define Trackpad: Ctrl+Alt+Win | Activate/Deactivate: Alt+Win");
                break;
        }
    } else if (downActivate && downMainMod && downQuickMod) {
         switch(mode) {
            case MODE_NONE:
            case MODE_FIRST_SCREEN_CORNER:
                if (rawX != -1 && rawY != -1) {
                    trackpadTopLeftX = rawX;
                    trackpadTopLeftY = rawY;
                    mode = MODE_FIRST_TRACKPAD_CORNER;
                    puts("Trackpad Corner 1 set. Press Ctrl+Alt+Win again at the other corner.");
                } else {
                    puts("Could not get raw trackpad coordinates. Touch trackpad and try again.");
                }
                break;
            case MODE_FIRST_TRACKPAD_CORNER:
                 if (rawX != -1 && rawY != -1) {
                    if (rawX < trackpadTopLeftX) {
                        trackpadBottomRightX = trackpadTopLeftX;
                        trackpadTopLeftX = rawX;
                    } else {
                        trackpadBottomRightX = rawX;
                    }
                    if (rawY < trackpadTopLeftY) {
                        trackpadBottomRightY = trackpadTopLeftY;
                        trackpadTopLeftY = rawY;
                    } else {
                        trackpadBottomRightY = rawY;
                    }
                    if (trackpadBottomRightX <= trackpadTopLeftX || trackpadBottomRightY <= trackpadTopLeftY) {
                         puts("Error: Defined trackpad region has zero/negative size. Reverting.");
                         mode = MODE_NONE;
                         trackpadTopLeftX = trackpadTopLeftY = trackpadBottomRightX = trackpadBottomRightY = -1;
                         trackpadRegionDefined = 0;
                         puts("Trackpad definition failed.");
                    } else {
                        trackpadRegionDefined = 1;
                        printf("Trackpad raw area defined: (%ld,%ld)-(%ld,%ld)\n", trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
                         if (screenRegionDefined) {
                            puts("Both regions defined. Press Alt+Win to activate drawing.");
                            mode = MODE_NONE;
                        } else {
                            puts("Trackpad area set. Now define screen region with Ctrl+Win.");
                            mode = MODE_NONE;
                        }
                    }
                } else {
                     puts("Could not get raw trackpad coordinates. Touch trackpad and try again.");
                }
                break;
            case MODE_ACTIVE:
                showActivate(0, 100);
                mode = MODE_NONE;
                puts("Exited drawing mode.");
                puts("Define Screen: Ctrl+Win | Define Trackpad: Ctrl+Alt+Win | Activate/Deactivate: Alt+Win");
                break;
         }
    } else if (downActivate && downQuickMod && !downMainMod) {
        if (mode == MODE_ACTIVE) {
            showActivate(0, 100);
            mode = MODE_NONE;
            puts("Exited drawing mode.");
            puts("Define Screen: Ctrl+Win | Define Trackpad: Ctrl+Alt+Win | Activate/Deactivate: Alt+Win");
        } else {
            if (screenRegionDefined && trackpadRegionDefined) {
                printf("Activating with screen area: (%d,%d)-(%d,%d)\n",topLeftX,topLeftY,bottomRightX,bottomRightY);
                printf("Activating with trackpad raw area: (%ld,%ld)-(%ld,%ld)\n", trackpadTopLeftX, trackpadTopLeftY, trackpadBottomRightX, trackpadBottomRightY);
                mode = MODE_ACTIVE;
                touching = 0;
                showActivate(1, 200);
                drawX = -1;
                drawY = -1;
            } else {
                 puts("Cannot activate: Both screen and trackpad regions must be defined first.");
                 if (!screenRegionDefined) puts("- Use Ctrl+Win to define screen area.");
                 if (!trackpadRegionDefined) puts("- Use Ctrl+Alt+Win to define trackpad area.");
            }
        }
    }
}

LRESULT CALLBACK EventHandler(
    HWND hwnd,
    unsigned event,
    WPARAM wparam,
    LPARAM lparam
) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT)+IN_BUFFER_SIZE];
    static BYTE preparsedBuffer[IN_BUFFER_SIZE];
    static BYTE usageBuffer[IN_BUFFER_SIZE];
    USAGE* usages = (USAGE*)usageBuffer;
    RAWINPUT* data = (RAWINPUT*)rawinputBuffer;
    PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)preparsedBuffer;

    switch (event) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_INPUT: {
            unsigned size = sizeof(rawinputBuffer);
            int res = GetRawInputData((HRAWINPUT)lparam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER));
            if (res < 0 || size == 0)
                return 0;
            if (data->header.dwType == RIM_TYPEKEYBOARD) {
                handleKeyboard(data->data.keyboard.MakeCode, data->data.keyboard.Flags);
                return 0;
            }

            size = sizeof(preparsedBuffer);
            res = GetRawInputDeviceInfo(data->header.hDevice, RIDI_PREPARSEDDATA, preparsed, &size);
            if (res < 0 || size == 0)
                return 0;

            // --- Trackpad Resolution Inspector ---
            static int printed_res = 0;
            if (!printed_res) {
                HIDP_VALUE_CAPS valCaps[32];
                USHORT capsLen = 32;
                int foundX = 0;
                int foundY = 0;
                if (HidP_GetValueCaps(HidP_Input, valCaps, &capsLen, preparsed) == HIDP_STATUS_SUCCESS) {
                    for (int i = 0; i < capsLen; i++) {
                        if (valCaps[i].UsagePage == 0x01 && valCaps[i].NotRange.Usage == 0x30) {
                            if (!foundX) {
                                printf("[HID Info] X Resolution: %ld to %ld\n", (long)valCaps[i].LogicalMin, (long)valCaps[i].LogicalMax);
                                foundX = 1;
                            }
                        }
                        else if (valCaps[i].UsagePage == 0x01 && valCaps[i].NotRange.Usage == 0x31) {
                            if (!foundY) {
                                printf("[HID Info] Y Resolution: %ld to %ld\n", (long)valCaps[i].LogicalMin, (long)valCaps[i].LogicalMax);
                                foundY = 1;
                            }
                        }
                    }
                    if (foundX || foundY)
                        printed_res = 1;
                }
            }

            long currentRawX = -1, currentRawY = -1;
            ULONG usageValue;
            res = HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x30, &usageValue, preparsed, (PCHAR)data->data.hid.bRawData, data->data.hid.dwSizeHid);
            if (res >= 0) {
                currentRawX = (long)usageValue;
                rawX = currentRawX;
            } else {
                 rawX = -1;
            }
            res = HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x31, &usageValue, preparsed, (PCHAR)data->data.hid.bRawData, data->data.hid.dwSizeHid);
            if (res >= 0) {
                currentRawY = (long)usageValue;
                rawY = currentRawY;
            } else {
                rawY = -1;
            }

            if (mode != MODE_ACTIVE)
                return 0;

            unsigned long usageLength = sizeof(usageBuffer)/sizeof(USAGE);
            int touch_detected = 0;
              res = HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, (PCHAR)data->data.hid.bRawData, data->data.hid.dwSizeHid);
            if (res >= 0) {
                  for (unsigned long j=0; j < usageLength; j++) {
                    if (usages[j] == 0x42) {
                        touch_detected = 1;
                        break;
                    }
                }
            }

            int pen_down = 0;
            if (fnMode == FN_MODE_DOWN) {
                pen_down = downFn;
            } else if (fnMode == FN_MODE_LIFT) {
                pen_down = touch_detected && !downFn;
            } else {
                 pen_down = touch_detected;
            }

            int x = -1, y = -1;
            if (trackpadRegionDefined && screenRegionDefined) {
                if (currentRawX != -1) {
                    x = getScaledRegion(currentRawX, trackpadTopLeftX, trackpadBottomRightX, bottomRightX - topLeftX);
                }
                if (currentRawY != -1) {
                     y = getScaledRegion(currentRawY, trackpadTopLeftY, trackpadBottomRightY, bottomRightY - topLeftY);
                }
                if (x != -1 && y != -1) {
                    x += topLeftX;
                    y += topLeftY;
                    if (x < topLeftX) x = topLeftX;
                    if (x > bottomRightX) x = bottomRightX;
                    if (y < topLeftY) y = topLeftY;
                    if (y > bottomRightY) y = bottomRightY;
                    if (x != drawX || y != drawY) {
                        moveMouse(x, y);
                        drawX = x;
                        drawY = y;
                    }
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

void promptTrackpadRegion(void) {
    char answer[10];
    printf("\nTrackpad region not defined.\n");
    printf("Do you want to enter raw coordinate limits manually? (y/n) [default: n, using 0,0,2904,1879]: ");
    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        trackpadTopLeftX = 0;
        trackpadTopLeftY = 0;
        trackpadBottomRightX = 2904;
        trackpadBottomRightY = 1879;
        trackpadRegionDefined = 1;
        printf("\nUsing default trackpad raw region: (0,0)-(2904,1879)\n");
        return;
    }

    answer[strcspn(answer, "\r\n")] = '\0';
    if (answer[0] == '\0' || answer[0] == 'n' || answer[0] == 'N') {
        trackpadTopLeftX = 0;
        trackpadTopLeftY = 0;
        trackpadBottomRightX = 2904;
        trackpadBottomRightY = 1879;
        trackpadRegionDefined = 1;
        printf("\nUsing default trackpad raw region: (0,0)-(2904,1879)\n");
        return;
    }

    if (answer[0] == 'y' || answer[0] == 'Y') {
        long x1, y1, x2, y2;
        printf("Enter min X, min Y, max X, max Y separated by commas or spaces: ");
        if (scanf("%ld %ld %ld %ld", &x1, &y1, &x2, &y2) == 4 ||
            scanf("%ld,%ld,%ld,%ld", &x1, &y1, &x2, &y2) == 4) {
            if (x1 < x2 && y1 < y2) {
                trackpadTopLeftX = x1;
                trackpadTopLeftY = y1;
                trackpadBottomRightX = x2;
                trackpadBottomRightY = y2;
                trackpadRegionDefined = 1;
                printf("Trackpad raw region set to: (%ld,%ld)-(%ld,%ld)\n", x1, y1, x2, y2);
            } else {
                printf("Error: x1 < x2 and y1 < y2 required. Region not set.\n");
            }
        } else {
            printf("Invalid input. Region not set.\n");
        }
        while (getchar() != '\n');
    } else {
        printf("You can define the trackpad region later by:\n");
        printf("  Touching near top-left corner, then press Ctrl+Alt+Win\n");
        printf("  Touching near bottom-right corner, then press Ctrl+Alt+Win again.\n");
    }
}

void help() {
    MessageBox(0,
"precision-trackpad-mapper [options]\n"
"\n"
"Options:\n"
"--help                 : this message\n"
"--fn-lift              : lift pen on `\n"
"--fn-down              : lower pen on ` [default]\n"
"--fn-none              : ignore ` key\n"
"--window T,L,B,R       : set screen box coordinates [default: full screen]\n"
"--trackpad X1,Y1,X2,Y2 : set trackpad raw coordinate limits\n"
"\n"
"Key Combinations:\n"
"Ctrl+Win      : Press once at top-left, again at bottom-right to define SCREEN area.\n"
"Ctrl+Alt+Win  : Press once at top-left, again at bottom-right to define TRACKPAD area.\n"
"Alt+Win       : Activate/Deactivate drawing mode (requires both areas defined).\n"
"Ctrl+C (in console) : Quit.\n"
, "Help", 0);
}

int processOptions(char* cmdLine) {
    char* token;
    char* src = cmdLine;
    while (NULL != (token = strtok(src, " "))) {
        src = NULL;
        if (!strcmp(token, "--fn-lift")) {
            fnMode = FN_MODE_LIFT;
        }
        else if (!strcmp(token, "--fn-down")) {
            fnMode = FN_MODE_DOWN;
        }
        else if (!strcmp(token, "--fn-none")) {
            fnMode = FN_MODE_NONE;
        }
        else if (!strcmp(token, "--window")) {
            if (NULL == (token = strtok(src, " "))) {
                help();
                return 0;
            }
            if (sscanf(token,"%d,%d,%d,%d",&topLeftX,&topLeftY,&bottomRightX,&bottomRightY) == 4) {
                 screenRegionDefined = 1;
                 if (topLeftX >= bottomRightX || topLeftY >= bottomRightY) {
                      fprintf(stderr, "Warning: Invalid window coordinates. Using full screen.\n");
                      topLeftX = 0; topLeftY = 0;
                      bottomRightX = GetSystemMetrics(SM_CXSCREEN);
                      bottomRightY = GetSystemMetrics(SM_CYSCREEN);
                 }
            } else {
                 fprintf(stderr, "Warning: Could not parse window coordinates. Using defaults.\n");
            }
        }
        else if (!strcmp(token, "--trackpad")) {
            if (NULL == (token = strtok(src, " "))) {
                help();
                return 0;
            }
            long x1, y1, x2, y2;
            if (sscanf(token,"%ld,%ld,%ld,%ld", &x1, &y1, &x2, &y2) == 4) {
                if (x1 < x2 && y1 < y2) {
                    trackpadTopLeftX = x1;
                    trackpadTopLeftY = y1;
                    trackpadBottomRightX = x2;
                    trackpadBottomRightY = y2;
                    trackpadRegionDefined = 1;
                    printf("Trackpad raw region set to: (%ld,%ld)-(%ld,%ld)\n", x1, y1, x2, y2);
                } else {
                    fprintf(stderr, "Error: Invalid trackpad coordinates (x1>=x2 or y1>=y2).\n");
                    return 0;
                }
            } else {
                fprintf(stderr, "Error: Could not parse trackpad coordinates. Use --trackpad x1,y1,x2,y2\n");
                return 0;
            }
        }
        else if (!strcmp(token, "--help") || !strcmp(token, "-h")) {
            help();
            return 0;
        }
    }
    return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE hPrevInstance,
    PSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)nCmdShow;
    const char* class_name = "finger-draw-2941248-class";

    SetProcessDPIAware();
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    topLeftX = 0;
    topLeftY = 0;
    bottomRightX = screenWidth;
    bottomRightY = screenHeight;
    screenRegionDefined = 1;

    if (!processOptions(lpCmdLine))
        return 0;

    if (!trackpadRegionDefined) {
        promptTrackpadRegion();
    }

    WNDCLASS window_class = {0};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    if (!RegisterClass(&window_class)) {
         fprintf(stderr,"Error: Failed to register window class (Error %ld)\n", GetLastError());
        return -1;
    }

    HWND window = CreateWindow(class_name, "finger-draw-2941248", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
    if (window == NULL) {
        fprintf(stderr,"Error: Failed to create message-only window (Error %ld)\n", GetLastError());
        return -1;
    }

    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x0D;
    rid[0].usUsage = 0x05;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = window;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK | RIDEV_NOLEGACY;
    rid[1].hwndTarget = window;

    int res = RegisterRawInputDevices(rid, 2, sizeof(rid[0]));
    if (!res) {
        fprintf(stderr, "Warning: Failed to register raw input devices (Error %ld).\n", GetLastError());
        res = RegisterRawInputDevices(&rid[1], 1, sizeof(rid[1]));
         if (!res) {
             fprintf(stderr, "Error: Failed to register keyboard raw input device (Error %ld). Exiting.\n", GetLastError());
             return -1;
         }
    }

    miHook = SetWindowsHookEx(WH_MOUSE_LL, (HOOKPROC)(&LowLevelMouseProc), 0, 0);
    if (miHook == NULL) {
         fprintf(stderr, "Warning: Failed to set mouse hook (Error %ld).\n", GetLastError());
    }

    queueReady = CreateEvent(NULL, FALSE, FALSE, (LPTSTR)("queueReady"));
    if (queueReady == NULL) {
         fprintf(stderr, "Error: Failed to create event (Error %ld). Exiting.\n", GetLastError());
         if (miHook) UnhookWindowsHookEx(miHook);
         return -1;
    }
    HANDLE queueThread = CreateThread(NULL, 0, handleQueue, NULL, 0, NULL);
     if (queueThread == NULL) {
         fprintf(stderr, "Error: Failed to create queue thread (Error %ld). Exiting.\n", GetLastError());
         CloseHandle(queueReady);
         if (miHook) UnhookWindowsHookEx(miHook);
         return -1;
    }

    puts("--- precision-trackpad-mapper ---");
    puts("1. Define Screen Area: Press Ctrl+Win at top-left, then again at bottom-right.");
    puts("   (Defaults to full screen if skipped)");
    puts("2. Define Trackpad Area: use --trackpad option, interactive prompt, or Ctrl+Alt+Win while touching.");
    puts("3. Activate/Deactivate: Press Alt+Win (requires both areas defined).");
    if (fnMode == FN_MODE_LIFT)
        puts("Mode: ` lifts pen.");
    else if (fnMode == FN_MODE_DOWN)
        puts("Mode: ` presses pen down (default).");
    else
        puts("Mode: ` key ignored.");
    puts("Press Ctrl+C in this console window to quit.\n");
    
    MSG message;
    while(GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    running = 0;
    SetEvent(queueReady);
    WaitForSingleObject(queueThread, INFINITE);
    CloseHandle(queueThread);
    CloseHandle(queueReady);
    if (miHook) UnhookWindowsHookEx(miHook);
    puts("Exiting.");
    return (int)message.wParam;
}
