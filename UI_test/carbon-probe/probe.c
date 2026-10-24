/*
 * probe.c - carbon-probe: toolechain reconnaissance for Mac OS X 10.3.9
 * (panther) from a Leopard intel cross host.
 *
 * One document window, ONE push button ("выход"). Click -> the app quits.
 * The administrator test: window appears, button kills the process.
 *
 * Panther-era APIs only:
 *   CreateNewWindow            10.0+
 *   kWindowStandardHandlerAttribute 10.2+
 *   CreatePushButtonControl    10.0+
 *   InstallControlEventHandler 10.1+
 *   kEventControlHit           10.2+
 *   QuitApplicationEventLoop   10.0+
 * Russia label via CFStringCreateWithCString(UTF-8) - no constant
 * CFString compile flags needed.
 */
#include <Carbon/Carbon.h>

static ControlRef g_btn = NULL;

static pascal OSStatus on_hit(EventHandlerCallRef call, EventRef ev, void *data)
{
    ControlRef hit = NULL;

    GetEventParameter(ev, kEventParamDirectObject, typeControlRef,
                      NULL, sizeof hit, NULL, &hit);
    if (hit == g_btn)
        QuitApplicationEventLoop();
    return eventNotHandledErr;
}

static pascal OSStatus on_win(EventHandlerCallRef call, EventRef ev, void *data)
{
    if (GetEventKind(ev) == kEventWindowClose)   /* red close box quits too */
        QuitApplicationEventLoop();
    return eventNotHandledErr;
}

int main(void)
{
    WindowRef      w = NULL;
    Rect           wr = { 200, 200, 330, 430 };
    Rect           br = { 80, 70, 100, 160 };
    CFStringRef    title, bt;
    EventHandlerUPP hit_upp, win_upp;
    EventTypeSpec  ce = { kEventClassControl, kEventControlHit };
    EventTypeSpec  we = { kEventClassWindow,  kEventWindowClose };
    OSStatus       st;

    st = CreateNewWindow(kDocumentWindowClass,
                         kWindowCloseBoxAttribute | kWindowStandardHandlerAttribute,
                         &wr, &w);
    if (st != noErr || w == NULL) return 1;

    title = CFStringCreateWithCString(kCFAllocatorDefault,
                                      "Разведка тулчейна",
                                      kCFStringEncodingUTF8);
    SetWindowTitleWithCFString(w, title);
    CFRelease(title);

    bt = CFStringCreateWithCString(kCFAllocatorDefault, "выход",
                                   kCFStringEncodingUTF8);
    st = CreatePushButtonControl(w, &br, bt, &g_btn);
    if (st != noErr || g_btn == NULL) return 2;

    win_upp = NewEventHandlerUPP(on_win);
    InstallWindowEventHandler(w, win_upp, 1, &we, NULL, NULL);
    hit_upp = NewEventHandlerUPP(on_hit);
    InstallControlEventHandler(g_btn, hit_upp, 1, &ce, NULL, NULL);

    ShowWindow(w);
    RunApplicationEventLoop();
    return 0;
}
