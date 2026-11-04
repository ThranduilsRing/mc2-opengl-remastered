#ifndef WINUSER_H
#define WINUSER_H

#ifdef __cplusplus
extern "C" {
#endif

constexpr auto SM_SWAPBUTTON = 23;

constexpr auto MB_USERICON = 128;
constexpr auto MB_ICONASTERISK = 64;
constexpr auto MB_ICONEXCLAMATION = 0x30;
constexpr auto MB_ICONWARNING = 0x30;
constexpr auto MB_ICONERROR = 16;
constexpr auto MB_ICONHAND = 16;
constexpr auto MB_ICONQUESTION = 32;
constexpr auto MB_OK = 0;
constexpr auto MB_ABORTRETRYIGNORE = 2;
constexpr auto MB_APPLMODAL = 0;
constexpr auto MB_DEFAULT_DESKTOP_ONLY = 0x20000;
constexpr auto MB_HELP = 0x4000;
constexpr auto MB_RIGHT = 0x80000;
constexpr auto MB_RTLREADING = 0x100000;
constexpr auto MB_TOPMOST = 0x40000;
constexpr auto MB_DEFBUTTON1 = 0;
constexpr auto MB_DEFBUTTON2 = 256;
constexpr auto MB_DEFBUTTON3 = 512;
constexpr auto MB_DEFBUTTON4 = 0x300;
constexpr auto MB_ICONINFORMATION = 64;
constexpr auto MB_ICONSTOP = 16;
constexpr auto MB_OKCANCEL = 1;
constexpr auto MB_RETRYCANCEL = 5;
constexpr auto MB_SERVICE_NOTIFICATION = 0x00200000;
constexpr auto MB_SERVICE_NOTIFICATION_NT3X = 0x00040000;
constexpr auto MB_SETFOREGROUND = 0x10000;
constexpr auto MB_SYSTEMMODAL = 4096;
constexpr auto MB_TASKMODAL = 0x2000;
constexpr auto MB_YESNO = 4;
constexpr auto MB_YESNOCANCEL = 3;
constexpr auto MB_ICONMASK = 240;
constexpr auto MB_DEFMASK = 3840;
constexpr auto MB_MODEMASK = 0x00003000;
constexpr auto MB_MISCMASK = 0x0000C000;
constexpr auto MB_NOFOCUS = 0x00008000;
constexpr auto MB_TYPEMASK = 15;
constexpr auto MB_TOPMOST = 0x40000;

constexpr auto IDOK = 1;
constexpr auto IDCANCEL = 2;
constexpr auto IDABORT = 3;
constexpr auto IDRETRY = 4;
constexpr auto IDIGNORE = 5;
constexpr auto IDYES = 6;
constexpr auto IDNO = 7;
constexpr auto IDCLOSE = 8;
constexpr auto IDHELP = 9;



constexpr auto VK_LBUTTON = 1;
constexpr auto VK_RBUTTON = 2;
constexpr auto VK_CANCEL = 3;
constexpr auto VK_MBUTTON = 4;

constexpr auto VK_BACK = 8;
constexpr auto VK_TAB = 9;
constexpr auto VK_CLEAR = 12;
constexpr auto VK_RETURN = 13;
constexpr auto VK_SHIFT = 16;
constexpr auto VK_CONTROL = 17;
constexpr auto VK_MENU = 18;
constexpr auto VK_PAUSE = 19;
constexpr auto VK_CAPITAL = 20;
constexpr auto VK_KANA = 0x15;
constexpr auto VK_HANGEUL = 0x15;
constexpr auto VK_HANGUL = 0x15;
constexpr auto VK_JUNJA = 0x17;
constexpr auto VK_FINAL = 0x18;
constexpr auto VK_HANJA = 0x19;
constexpr auto VK_KANJI = 0x19;
constexpr auto VK_ESCAPE = 0x1B;
constexpr auto VK_CONVERT = 0x1C;
constexpr auto VK_NONCONVERT = 0x1D;
constexpr auto VK_ACCEPT = 0x1E;
constexpr auto VK_MODECHANGE = 0x1F;
constexpr auto VK_SPACE = 32;
constexpr auto VK_PRIOR = 33;
constexpr auto VK_NEXT = 34;
constexpr auto VK_END = 35;
constexpr auto VK_HOME = 36;
constexpr auto VK_LEFT = 37;
constexpr auto VK_UP = 38;
constexpr auto VK_RIGHT = 39;
constexpr auto VK_DOWN = 40;
constexpr auto VK_SELECT = 41;
constexpr auto VK_PRINT = 42;
constexpr auto VK_EXECUTE = 43;
constexpr auto VK_SNAPSHOT = 44;
constexpr auto VK_INSERT = 45;
constexpr auto VK_DELETE = 46;
constexpr auto VK_HELP = 47;
constexpr auto VK_LWIN = 0x5B;
constexpr auto VK_RWIN = 0x5C;
constexpr auto VK_APPS = 0x5D;
constexpr auto VK_SLEEP = 0x5F;
constexpr auto VK_NUMPAD0 = 0x60;
constexpr auto VK_NUMPAD1 = 0x61;
constexpr auto VK_NUMPAD2 = 0x62;
constexpr auto VK_NUMPAD3 = 0x63;
constexpr auto VK_NUMPAD4 = 0x64;
constexpr auto VK_NUMPAD5 = 0x65;
constexpr auto VK_NUMPAD6 = 0x66;
constexpr auto VK_NUMPAD7 = 0x67;
constexpr auto VK_NUMPAD8 = 0x68;
constexpr auto VK_NUMPAD9 = 0x69;
constexpr auto VK_MULTIPLY = 0x6A;
constexpr auto VK_ADD = 0x6B;
constexpr auto VK_SEPARATOR = 0x6C;
constexpr auto VK_SUBTRACT = 0x6D;
constexpr auto VK_DECIMAL = 0x6E;
constexpr auto VK_DIVIDE = 0x6F;
constexpr auto VK_F1 = 0x70;
constexpr auto VK_F2 = 0x71;
constexpr auto VK_F3 = 0x72;
constexpr auto VK_F4 = 0x73;
constexpr auto VK_F5 = 0x74;
constexpr auto VK_F6 = 0x75;
constexpr auto VK_F7 = 0x76;
constexpr auto VK_F8 = 0x77;
constexpr auto VK_F9 = 0x78;
constexpr auto VK_F10 = 0x79;
constexpr auto VK_F11 = 0x7A;
constexpr auto VK_F12 = 0x7B;
constexpr auto VK_F13 = 0x7C;
constexpr auto VK_F14 = 0x7D;
constexpr auto VK_F15 = 0x7E;
constexpr auto VK_F16 = 0x7F;
constexpr auto VK_F17 = 0x80;
constexpr auto VK_F18 = 0x81;
constexpr auto VK_F19 = 0x82;
constexpr auto VK_F20 = 0x83;
constexpr auto VK_F21 = 0x84;
constexpr auto VK_F22 = 0x85;
constexpr auto VK_F23 = 0x86;
constexpr auto VK_F24 = 0x87;
constexpr auto VK_NUMLOCK = 0x90;
constexpr auto VK_SCROLL = 0x91;
constexpr auto VK_LSHIFT = 0xA0;
constexpr auto VK_RSHIFT = 0xA1;
constexpr auto VK_LCONTROL = 0xA2;
constexpr auto VK_RCONTROL = 0xA3;
constexpr auto VK_LMENU = 0xA4;
constexpr auto VK_RMENU = 0xA5;

constexpr auto VK_OEM_1 = 0xBA;

constexpr auto VK_OEM_2 = 0xBF;
constexpr auto VK_OEM_3 = 0xC0;
constexpr auto VK_OEM_4 = 0xDB;
constexpr auto VK_OEM_5 = 0xDC;
constexpr auto VK_OEM_6 = 0xDD;
constexpr auto VK_OEM_7 = 0xDE;
constexpr auto VK_OEM_8 = 0xDF;

constexpr auto VK_PROCESSKEY = 0xE5;

constexpr auto VK_ATTN = 0xF6;
constexpr auto VK_CRSEL = 0xF7;
constexpr auto VK_EXSEL = 0xF8;
constexpr auto VK_EREOF = 0xF9;
constexpr auto VK_PLAY = 0xFA;
constexpr auto VK_ZOOM = 0xFB;
constexpr auto VK_NONAME = 0xFC;
constexpr auto VK_PA1 = 0xFD;
constexpr auto VK_OEM_CLEAR = 0xFE;


int GetSystemMetrics(int);
SHORT GetAsyncKeyState(int);

#define MessageBox MessageBoxA //__AW(MessageBox)
#define CharLower CharLowerA 
#define CharUpper CharUpperA 

int MessageBoxA(HWND,LPCSTR,LPCSTR,UINT);
int MessageBoxW(HWND,LPCWSTR,LPCWSTR,UINT);

LPSTR CharLowerA(LPSTR);
LPSTR CharUpperA(LPSTR);

LPWSTR CharLowerW(LPWSTR);
LPWSTR CharUpperW(LPWSTR);

#ifdef __cplusplus
}
#endif

#endif // WINUSER_H

