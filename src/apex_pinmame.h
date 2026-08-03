#ifndef APEX_PINMAME_H
#define APEX_PINMAME_H

/* Client for PinMAME's remote debugger (src/remote_debug in vpinball/pinmame):
   spawns a headless `xpinmamed` process and talks to its HTTP/JSON REST API
   (all GET, loopback).  This is the transport for ApexII's dynamic analysis
   (code coverage, live CPU/RAM, banking).  Pure POSIX + C++ — no ImGui — so it
   can be unit-tested headless against a live emulator.

   API responses use DECIMAL numbers (verified against xpinmamed): e.g. /state
   gives {"cpus":[{"pc":35939,...}]}, coverage gives {"addr":16384,"bank":30,
   "executed":[0,1,...]}, memory gives {"data":[62,125,...]}. */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

/* Last CPU halt reported by the /api/events SSE stream (a breakpoint or
   watchpoint firing).  `seq` bumps on every halt so a poller can spot new ones;
   for a watchpoint, `has_addr` is set and `addr` is the accessed address. */
struct ApexPinmameHalt {
    unsigned long seq = 0;
    std::string   reason;    /* "bp" | "wp" | other */
    long          pc = 0;
    long          addr = 0;
    int           bank = -1;
    bool          has_addr = false;
};

struct ApexPinmame {
    pid_t       pid     = -1;
    int         port    = 0;
    bool        running = false;   /* we launched a child that is (was) alive */
    std::string last_error;
    std::string log_path;          /* child stdout/stderr, for failure diagnostics */

    /* Background reader of the /api/events SSE stream (halt notifications). */
    std::thread       ev_thread;
    std::atomic<bool> ev_stop{false};
    std::mutex        ev_mtx;       /* guards ev_halt */
    ApexPinmameHalt   ev_halt;
    bool              ev_running = false;
};

struct ApexPinmameInfo {
    bool        valid    = false;
    std::string game;
    std::string description;
    int         paused   = -1;
    int         wpc_bank = -1;
};

struct ApexPinmameCpu {
    bool valid = false;
    long pc = 0, sp = 0, a = 0, b = 0, x = 0, y = 0, u = 0, dp = 0, cc = 0;
};

/* Launch xpinmamed headless & paused for `game`, listening on `port`.  Returns
   true if the child was spawned (not that the HTTP server is up yet — poll
   apex_pinmame_get_info for readiness).  On failure fills pm.last_error. */
bool apex_pinmame_launch(ApexPinmame &pm, const std::string &bin,
                         const std::string &rompath, const std::string &game, int port);

/* SIGTERM the child and reap it.  Safe to call when not running. */
void apex_pinmame_stop(ApexPinmame &pm);

/* True if something is already listening on 127.0.0.1:port (would collide). */
bool apex_pinmame_port_in_use(int port);

/* Last ~max bytes of the child's captured stdout/stderr (for error reporting). */
std::string apex_pinmame_log_tail(const ApexPinmame &pm, size_t max = 1200);

/* True if we launched a child and it has not exited (reaps zombies). */
bool apex_pinmame_is_alive(ApexPinmame &pm);

/* Raw HTTP/1.0 GET against 127.0.0.1:port.  Fills `body`; false on any error. */
bool apex_pinmame_http_get(int port, const std::string &path, std::string &body,
                           int timeout_ms = 3000);

/* Parsed endpoints. */
bool apex_pinmame_get_info(int port, ApexPinmameInfo &out);
bool apex_pinmame_get_cpu0(int port, ApexPinmameCpu &out);

/* One breakpoint or watchpoint from /api/debugger/points (bank < 0 = system). */
struct ApexPinmamePoint {
    int         idx = 0;
    int         is_wp = 0;   /* 0 = breakpoint, 1 = watchpoint */
    long        addr = 0;
    int         bank = -1;
    long        hits = 0;
    int         mode = 0;    /* watchpoints: 1=r 2=w 3=rw */
    int         len = 1;     /* watchpoints: byte length */
    std::string cond;        /* condition expression, if any */
};

/* One switch from /api/switches. */
struct ApexPinmameSwitch {
    int         num = 0;
    int         col = -1;
    int         row = -1;
    int         active = 0;
    std::string name;
};

/* Execution / coverage control.  cmd is the literal API command
   (pause|resume|step|... for control; start|stop|clear for coverage). */
bool apex_pinmame_control(int port, const char *cmd);

/* Add/clear a bank-aware breakpoint or watchpoint.  cmd is "add" or "clear"
   (clear ignores the other args).  bank < 0 omits the bank (system window). */
bool apex_pinmame_breakpoint(int port, const char *cmd, unsigned addr, int bank,
                             const char *cond);
bool apex_pinmame_watchpoint(int port, const char *cmd, unsigned addr, int bank,
                             int len, int mode);

/* Inject a switch: val 0/1; pulse_ms > 0 holds for that long then restores. */
bool apex_pinmame_input(int port, int sw, int val, int pulse_ms);

/* Read the current breakpoint+watchpoint list / the switch matrix. */
bool apex_pinmame_points(int port, std::vector<ApexPinmamePoint> &out);
bool apex_pinmame_switches(int port, std::vector<ApexPinmameSwitch> &out);

/* Delete one point by (type, idx).  The remote debugger only supports add|clear,
   so this clears that type and re-adds the others (preserving addr/bank/cond and,
   for watchpoints, len/mode). */
bool apex_pinmame_point_delete(int port, int is_wp, int idx);

/* Start/stop the background SSE reader that captures halt events (needs a live
   connection).  apex_pinmame_stop also stops it. */
void apex_pinmame_events_start(ApexPinmame &pm);
void apex_pinmame_events_stop(ApexPinmame &pm);
/* Copy the last captured halt; returns false if none seen yet. */
bool apex_pinmame_get_halt(ApexPinmame &pm, ApexPinmameHalt &out);

/* Per-address execution counters (/api/debugger/instrument) — for a call hotlist. */
struct ApexPinmameCount { long addr; int bank; long count; };
bool apex_pinmame_instrument(int port, const char *cmd, unsigned addr, int bank);
bool apex_pinmame_instrument_read(int port, std::vector<ApexPinmameCount> &out);

/* Recent-instruction ring (/api/debugger/exectrace) — for an execution backtrace. */
struct ApexPinmameTrace { long pc; int bank; long a, b, x; };
bool apex_pinmame_exectrace_cmd(int port, const char *cmd);   /* start|stop|clear */
bool apex_pinmame_exectrace(int port, std::vector<ApexPinmameTrace> &out);

/* Read `size` bytes from CPU address `addr` (bank < 0 = no bank param). */
bool apex_pinmame_memory(int port, unsigned addr, int size, int bank,
                         std::vector<uint8_t> &out);

/* Fetch the live DMD framebuffer: /api/dmd/info for w/h, /api/dmd/raw for one
   luminance byte per pixel (w*h bytes). */
bool apex_pinmame_dmd(int port, int &w, int &h, std::vector<uint8_t> &lum);
bool apex_pinmame_coverage_cmd(int port, const char *cmd);
bool apex_pinmame_coverage_summary(int port, long &executed, long &addressable);

/* Read the per-byte executed flags for [addr, addr+size) in `bank` (pass
   bank < 0 for the system/fixed window, which takes no bank param).  Appends
   one 0/1 per byte to `flags`.  Chunks internally so any size is safe. */
bool apex_pinmame_coverage_window(int port, int bank, unsigned addr, unsigned size,
                                  std::vector<uint8_t> &flags);

/* Minimal JSON scalar/array extractors (flat objects, decimal numbers).
   Exposed for unit tests. */
bool apex_json_int(const std::string &json, const char *key, long &out);
bool apex_json_str(const std::string &json, const char *key, std::string &out);
bool apex_json_int_array(const std::string &json, const char *key, std::vector<long> &out);

#endif /* APEX_PINMAME_H */
