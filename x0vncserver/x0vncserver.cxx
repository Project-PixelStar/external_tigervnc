/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2004-2006 Constantin Kaplinsky.  All Rights Reserved.
 *    
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// FIXME: Check cases when screen width/height is not a multiply of 32.
//        e.g. 800x600.

#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <rfb/Logger_stdio.h>
#include <rfb/LogWriter.h>
#include <rfb/VNCServerST.h>
#include <rfb/Configuration.h>
#include <rfb/SSecurityFactoryStandard.h>
#include <rfb/Timer.h>
#include <network/TcpSocket.h>
#include <tx/TXWindow.h>

#include <vncconfig_unix/QueryConnectDialog.h>

#include <signal.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include <x0vncserver/Image.h>
#include <x0vncserver/PollingManager.h>
#include <x0vncserver/PollingScheduler.h>

// XXX Lynx/OS 2.3: protos for select(), bzero()
#ifdef Lynx
#include <sys/proto.h>
#endif

using namespace rfb;
using namespace network;

LogWriter vlog("main");

IntParameter pollingCycle("PollingCycle", "Milliseconds per one polling "
                          "cycle; actual interval may be dynamically "
                          "adjusted to satisfy MaxProcessorUsage setting", 30);
IntParameter maxProcessorUsage("MaxProcessorUsage", "Maximum percentage of "
                               "CPU time to be consumed", 35);
BoolParameter useShm("UseSHM", "Use MIT-SHM extension if available", true);
BoolParameter useOverlay("OverlayMode", "Use overlay mode under "
                         "IRIX or Solaris", true);
StringParameter displayname("display", "The X display", "");
IntParameter rfbport("rfbport", "TCP port to listen for RFB protocol",5900);
IntParameter queryConnectTimeout("QueryConnectTimeout",
                                 "Number of seconds to show the Accept Connection dialog before "
                                 "rejecting the connection",
                                 10);
StringParameter hostsFile("HostsFile", "File with IP access control rules", "");

static void CleanupSignalHandler(int sig)
{
  // CleanupSignalHandler allows C++ object cleanup to happen because it calls
  // exit() rather than the default which is to abort.
  fprintf(stderr,"CleanupSignalHandler called\n");
  exit(1);
}


class QueryConnHandler : public VNCServerST::QueryConnectionHandler,
                         public QueryResultCallback {
public:
  QueryConnHandler(Display* dpy, VNCServerST* vs)
    : display(dpy), server(vs), queryConnectDialog(0), queryConnectSock(0) {}
  ~QueryConnHandler() { delete queryConnectDialog; }

  // -=- VNCServerST::QueryConnectionHandler interface
  virtual VNCServerST::queryResult queryConnection(network::Socket* sock,
                                                   const char* userName,
                                                   char** reason) {
    if (queryConnectSock) {
      *reason = strDup("Another connection is currently being queried.");
      return VNCServerST::REJECT;
    }
    if (!userName) userName = "(anonymous)";
    queryConnectSock = sock;
    CharArray address(sock->getPeerAddress());
    delete queryConnectDialog;
    queryConnectDialog = new QueryConnectDialog(display, address.buf,
                                                userName, queryConnectTimeout,
                                                this);
    queryConnectDialog->map();
    return VNCServerST::PENDING;
  }

  // -=- QueryResultCallback interface
  virtual void queryApproved() {
    server->approveConnection(queryConnectSock, true, 0);
    queryConnectSock = 0;
  }
  virtual void queryRejected() {
    server->approveConnection(queryConnectSock, false,
                              "Connection rejected by local user");
    queryConnectSock = 0;
  }
private:
  Display* display;
  VNCServerST* server;
  QueryConnectDialog* queryConnectDialog;
  network::Socket* queryConnectSock;
};


class XDesktop : public SDesktop, public ColourMap
{
public:
  XDesktop(Display* dpy_)
    : dpy(dpy_), pb(0), server(0), image(0), pollmgr(0),
      oldButtonMask(0), haveXtest(false), maxButtons(0), running(false)
  {
#ifdef HAVE_XTEST
    int xtestEventBase;
    int xtestErrorBase;
    int major, minor;

    if (XTestQueryExtension(dpy, &xtestEventBase,
                            &xtestErrorBase, &major, &minor)) {
      XTestGrabControl(dpy, True);
      vlog.info("XTest extension present - version %d.%d",major,minor);
      haveXtest = true;
    } else {
#endif
      vlog.info("XTest extension not present");
      vlog.info("unable to inject events or display while server is grabbed");
#ifdef HAVE_XTEST
    }
#endif

  }
  virtual ~XDesktop() {
    stop();
  }

  // -=- SDesktop interface

  virtual void start(VNCServer* vs) {

    // Determine actual number of buttons of the X pointer device.
    unsigned char btnMap[8];
    int numButtons = XGetPointerMapping(dpy, btnMap, 8);
    maxButtons = (numButtons > 8) ? 8 : numButtons;
    vlog.info("Enabling %d button%s of X pointer device",
              maxButtons, (maxButtons != 1) ? "s" : "");

    int dpyWidth = DisplayWidth(dpy, DefaultScreen(dpy));
    int dpyHeight = DisplayHeight(dpy, DefaultScreen(dpy));

    ImageFactory factory((bool)useShm, (bool)useOverlay);
    image = factory.newImage(dpy, dpyWidth, dpyHeight);
    image->get(DefaultRootWindow(dpy));

    pollmgr = new PollingManager(dpy, image, &factory);
    pollmgr->setVNCServer(vs);

    pf.bpp = image->xim->bits_per_pixel;
    pf.depth = image->xim->depth;
    pf.bigEndian = (image->xim->byte_order == MSBFirst);
    pf.trueColour = image->isTrueColor();
    pf.redShift   = ffs(image->xim->red_mask) - 1;
    pf.greenShift = ffs(image->xim->green_mask) - 1;
    pf.blueShift  = ffs(image->xim->blue_mask) - 1;
    pf.redMax     = image->xim->red_mask   >> pf.redShift;
    pf.greenMax   = image->xim->green_mask >> pf.greenShift;
    pf.blueMax    = image->xim->blue_mask  >> pf.blueShift;

    pb = new FullFramePixelBuffer(pf, dpyWidth, dpyHeight,
                                  (rdr::U8*)image->xim->data, this);
    server = vs;
    server->setPixelBuffer(pb);

    running = true;
  }

  virtual void stop() {
    running = false;

    delete pb;
    delete pollmgr;
    delete image;

    pb = 0;
    pollmgr = 0;
    image = 0;
  }

  inline bool isRunning() {
    return running;
  }

  inline void poll() {
    if (pollmgr)
      pollmgr->poll();
  }

  virtual void pointerEvent(const Point& pos, int buttonMask) {
    pollmgr->setPointerPos(pos);
#ifdef HAVE_XTEST
    if (!haveXtest) return;
    XTestFakeMotionEvent(dpy, DefaultScreen(dpy), pos.x, pos.y, CurrentTime);
    if (buttonMask != oldButtonMask) {
      for (int i = 0; i < maxButtons; i++) {
	if ((buttonMask ^ oldButtonMask) & (1<<i)) {
          if (buttonMask & (1<<i)) {
            XTestFakeButtonEvent(dpy, i+1, True, CurrentTime);
          } else {
            XTestFakeButtonEvent(dpy, i+1, False, CurrentTime);
          }
        }
      }
    }
    oldButtonMask = buttonMask;
#endif
  }

  virtual void keyEvent(rdr::U32 key, bool down) {
#ifdef HAVE_XTEST
    if (!haveXtest) return;
    int keycode = XKeysymToKeycode(dpy, key);
    if (keycode)
      XTestFakeKeyEvent(dpy, keycode, down, CurrentTime);
#endif
  }

  virtual void clientCutText(const char* str, int len) {
  }

  virtual Point getFbSize() {
    return Point(pb->width(), pb->height());
  }

  // -=- ColourMap callbacks
  virtual void lookup(int index, int* r, int* g, int* b) {
    XColor xc;
    xc.pixel = index;
    if (index < DisplayCells(dpy,DefaultScreen(dpy))) {
      XQueryColor(dpy, DefaultColormap(dpy,DefaultScreen(dpy)), &xc);
    } else {
      xc.red = xc.green = xc.blue = 0;
    }
    *r = xc.red;
    *g = xc.green;
    *b = xc.blue;
  }

protected:
  Display* dpy;
  PixelFormat pf;
  PixelBuffer* pb;
  VNCServer* server;
  Image* image;
  PollingManager* pollmgr;
  int oldButtonMask;
  bool haveXtest;
  int maxButtons;
  bool running;
};


class FileTcpFilter : public TcpFilter
{

public:

  FileTcpFilter(const char *fname)
    : TcpFilter("-"), fileName(NULL), lastModTime(0)
  {
    if (fname != NULL)
      fileName = strdup((char *)fname);
  }

  virtual ~FileTcpFilter()
  {
    if (fileName != NULL)
      free(fileName);
  }

  virtual bool verifyConnection(Socket* s)
  {
    if (!reloadRules()) {
      vlog.error("Could not read IP filtering rules: rejecting all clients");
      filter.clear();
      filter.push_back(parsePattern("-"));
      return false;
    }

    return TcpFilter::verifyConnection(s);
  }

protected:

  bool reloadRules()
  {
    if (fileName == NULL)
      return true;

    struct stat st;
    if (stat(fileName, &st) != 0)
      return false;

    if (st.st_mtime != lastModTime) {
      // Actually reload only if the file was modified
      FILE *fp = fopen(fileName, "r");
      if (fp == NULL)
        return false;

      // Remove all the rules from the parent class
      filter.clear();

      // Parse the file contents adding rules to the parent class
      char buf[32];
      while (readLine(buf, 32, fp)) {
        if (buf[0] && strchr("+-?", buf[0])) {
          filter.push_back(parsePattern(buf));
        }
      }

      fclose(fp);
      lastModTime = st.st_mtime;
    }
    return true;
  }

protected:

  char *fileName;
  time_t lastModTime;

private:

  //
  // NOTE: we silently truncate long lines in this function.
  //

  bool readLine(char *buf, int bufSize, FILE *fp)
  {
    if (fp == NULL || buf == NULL || bufSize == 0)
      return false;

    if (fgets(buf, bufSize, fp) == NULL)
      return false;

    char *ptr = strchr(buf, '\n');
    if (ptr != NULL) {
      *ptr = '\0';              // remove newline at the end
    } else {
      if (!feof(fp)) {
        int c;
        do {                    // skip the rest of a long line
          c = getc(fp);
        } while (c != '\n' && c != EOF);
      }
    }
    return true;
  }

};

char* programName;

static void usage()
{
  fprintf(stderr, "\nusage: %s [<parameters>]\n", programName);
  fprintf(stderr,"\n"
          "Parameters can be turned on with -<param> or off with -<param>=0\n"
          "Parameters which take a value can be specified as "
          "-<param> <value>\n"
          "Other valid forms are <param>=<value> -<param>=<value> "
          "--<param>=<value>\n"
          "Parameter names are case-insensitive.  The parameters are:\n\n");
  Configuration::listParams(79, 14);
  exit(1);
}

int main(int argc, char** argv)
{
  initStdIOLoggers();
  LogWriter::setLogParams("*:stderr:30");

  programName = argv[0];
  Display* dpy;

  for (int i = 1; i < argc; i++) {
    if (Configuration::setParam(argv[i]))
      continue;

    if (argv[i][0] == '-') {
      if (i+1 < argc) {
        if (Configuration::setParam(&argv[i][1], argv[i+1])) {
          i++;
          continue;
        }
      }
      usage();
    }

    usage();
  }

  CharArray dpyStr(displayname.getData());
  if (!(dpy = XOpenDisplay(dpyStr.buf[0] ? dpyStr.buf : 0))) {
    fprintf(stderr,"%s: unable to open display \"%s\"\r\n",
            programName, XDisplayName(displayname.getData()));
    exit(1);
  }

  signal(SIGHUP, CleanupSignalHandler);
  signal(SIGINT, CleanupSignalHandler);
  signal(SIGTERM, CleanupSignalHandler);

  try {
    TXWindow::init(dpy,"x0vncserver");
    XDesktop desktop(dpy);
    VNCServerST server("x0vncserver", &desktop);
    QueryConnHandler qcHandler(dpy, &server);
    server.setQueryConnectionHandler(&qcHandler);

    TcpListener listener((int)rfbport);
    vlog.info("Listening on port %d", (int)rfbport);

    FileTcpFilter fileTcpFilter(hostsFile.getData());
    if (strlen(hostsFile.getData()) != 0)
      listener.setFilter(&fileTcpFilter);

    PollingScheduler sched((int)pollingCycle, (int)maxProcessorUsage);

    while (true) {
      struct timeval tv;
      fd_set rfds;
      std::list<Socket*> sockets;
      std::list<Socket*>::iterator i;

      // Process any incoming X events
      TXWindow::handleXEvents(dpy);

      FD_ZERO(&rfds);
      FD_SET(listener.getFd(), &rfds);
      server.getSockets(&sockets);
      int clients_connected = 0;
      for (i = sockets.begin(); i != sockets.end(); i++) {
        if ((*i)->isShutdown()) {
          server.removeSocket(*i);
          delete (*i);
        } else {
          FD_SET((*i)->getFd(), &rfds);
          clients_connected++;
        }
      }

      if (clients_connected) {
        int wait_ms = sched.millisRemaining();
        if (wait_ms > 500) {
          wait_ms = 500;
        }
        tv.tv_usec = wait_ms * 1000;
#ifdef DEBUG
        // fprintf(stderr, "[%d]\t", wait_ms);
#endif
      } else {
        sched.reset();
        tv.tv_usec = 100000;
      }
      tv.tv_sec = 0;

      // Do the wait...
      sched.sleepStarted();
      int n = select(FD_SETSIZE, &rfds, 0, 0, &tv);
      sched.sleepFinished();

      if (n < 0) {
        if (errno == EINTR) {
          vlog.debug("interrupted select() system call");
          continue;
        } else {
          throw rdr::SystemException("select", errno);
        }
      }

      // Accept new VNC connections
      if (FD_ISSET(listener.getFd(), &rfds)) {
        Socket* sock = listener.accept();
        if (sock) {
          server.addSocket(sock);
        } else {
          vlog.status("Client connection rejected");
        }
      }

      Timer::checkTimeouts();
      server.checkTimeouts();

      // Client list could have been changed.
      server.getSockets(&sockets);

      // Nothing more to do if there are no client connections.
      if (sockets.empty())
        continue;

      // Process events on existing VNC connections
      for (i = sockets.begin(); i != sockets.end(); i++) {
        if (FD_ISSET((*i)->getFd(), &rfds))
          server.processSocketEvent(*i);
      }

      if (desktop.isRunning() && sched.goodTimeToPoll()) {
        sched.newPass();
        desktop.poll();
      }
    }

  } catch (rdr::Exception &e) {
    vlog.error(e.str());
  };

  return 0;
}
