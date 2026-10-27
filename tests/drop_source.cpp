#include <QByteArray>
#include <QUrl>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>

// Real X11 file-drop selection exchange, without mapping another application
// that Gamescope would scale/focus in place of the preview under test.
int main(int argc,char** argv) {
    auto* display=XOpenDisplay(nullptr);
    if (!display) return 2;
    const auto atom=[&](const char* name) { return XInternAtom(display,name,False); };
    const auto root=DefaultRootWindow(display);
    Window target=std::strtoul(std::getenv("ASSET_PREVIEW_DROP_TARGET"),nullptr,10);
    const int x=std::atoi(std::getenv("ASSET_PREVIEW_DROP_X")), y=std::atoi(std::getenv("ASSET_PREVIEW_DROP_Y"));
    Window current=target;
    // Follow the same deepest XDND-aware child lookup as a file manager,
    // including an embedded OpenGL surface when it advertises drop support.
    for (int depth=0; depth<16; ++depth) {
        Atom type; int format; unsigned long count,remaining; unsigned char* data=nullptr;
        XGetWindowProperty(display,current,atom("XdndAware"),0,1,False,AnyPropertyType,&type,&format,&count,&remaining,&data);
        if (count) target=current;
        if (data) XFree(data);
        Window child; int localX,localY;
        XTranslateCoordinates(display,root,current,x,y,&localX,&localY,&child);
        if (!child) break;
        current=child;
    }
    const auto source=XCreateSimpleWindow(display,root,0,0,1,1,0,0,0);
    const auto selection=atom("XdndSelection"), uri=atom("text/uri-list"), copy=atom("XdndActionCopy");
    const Atom actions[]{copy,atom("XdndActionMove")};
    const bool moveOnly=std::getenv("ASSET_PREVIEW_DROP_MOVE_ONLY");
    XChangeProperty(display,source,atom("XdndActionList"),XA_ATOM,32,PropModeReplace,
        reinterpret_cast<const unsigned char*>(actions+(moveOnly?1:0)),moveOnly?1:2);
    QByteArray payload;
    for (int i=1;i<argc;++i) payload+=QUrl::fromLocalFile(QString::fromLocal8Bit(argv[i])).toEncoded()+"\r\n";
    XSetSelectionOwner(display,selection,source,CurrentTime);
    auto send=[&](const char* name,long a=0,long b=0,long c=0,long d=0) {
        XEvent event{}; event.xclient.type=ClientMessage; event.xclient.display=display;
        event.xclient.window=target; event.xclient.message_type=atom(name); event.xclient.format=32;
        event.xclient.data.l[0]=source; event.xclient.data.l[1]=a; event.xclient.data.l[2]=b;
        event.xclient.data.l[3]=c; event.xclient.data.l[4]=d;
        XSendEvent(display,target,False,NoEventMask,&event); XFlush(display);
    };
    send("XdndEnter",5L<<24,uri);
    send("XdndPosition",0,(x<<16)|(y&0xffff),CurrentTime,atom("XdndActionMove"));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    int result=2;
    while (std::chrono::steady_clock::now()<deadline) {
        if (!XPending(display)) {
            pollfd descriptor{ConnectionNumber(display),POLLIN,0}; poll(&descriptor,1,50); continue;
        }
        XEvent event; XNextEvent(display,&event);
        if (event.type==SelectionRequest) {
            const auto& request=event.xselectionrequest;
            const auto property=request.property?request.property:request.target;
            XEvent reply{}; reply.xselection.type=SelectionNotify; reply.xselection.display=display;
            reply.xselection.requestor=request.requestor; reply.xselection.selection=request.selection;
            reply.xselection.target=request.target; reply.xselection.time=request.time;
            if (request.target==uri) {
                XChangeProperty(display,request.requestor,property,uri,8,PropModeReplace,
                    reinterpret_cast<const unsigned char*>(payload.constData()),payload.size());
                reply.xselection.property=property;
            }
            XSendEvent(display,request.requestor,False,NoEventMask,&reply); XFlush(display);
        } else if (event.type==ClientMessage && event.xclient.message_type==atom("XdndStatus")) {
            if (!(event.xclient.data.l[1]&1)) { std::puts("drop rejected"); result=1; break; }
            send("XdndDrop",0,CurrentTime);
        } else if (event.type==ClientMessage && event.xclient.message_type==atom("XdndFinished")) {
            result=(event.xclient.data.l[1]&1) && event.xclient.data.l[2]==long(copy)?0:1;
            std::printf("drop finished: copy=%d\n",result==0); break;
        }
    }
    XDestroyWindow(display,source); XCloseDisplay(display);
    return result;
}
