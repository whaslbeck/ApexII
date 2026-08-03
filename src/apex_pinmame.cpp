#include "apex_pinmame.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Split each flat {...} object inside the named JSON array (defined below). */
static std::vector<std::string> json_objects(const std::string &json, const char *key);

/* ---- JSON (flat objects, decimal numbers — matches the xpinmamed API) ---- */

/* Position just past the ':' following the "key" token, or npos. */
static size_t json_value_pos(const std::string &json, const char *key)
{
    std::string tok = std::string("\"") + key + "\"";
    size_t k = json.find(tok);
    if (k == std::string::npos) {
        return std::string::npos;
    }
    size_t c = json.find(':', k + tok.size());
    if (c == std::string::npos) {
        return std::string::npos;
    }
    c++;
    while (c < json.size() && (json[c] == ' ' || json[c] == '\t')) {
        c++;
    }
    return c;
}

bool apex_json_int(const std::string &json, const char *key, long &out)
{
    size_t p = json_value_pos(json, key);
    if (p == std::string::npos) {
        return false;
    }
    char *end = nullptr;
    long v = strtol(json.c_str() + p, &end, 0);
    if (end == json.c_str() + p) {
        return false;
    }
    out = v;
    return true;
}

bool apex_json_str(const std::string &json, const char *key, std::string &out)
{
    size_t p = json_value_pos(json, key);
    if (p == std::string::npos || json[p] != '"') {
        return false;
    }
    size_t e = json.find('"', p + 1);
    if (e == std::string::npos) {
        return false;
    }
    out = json.substr(p + 1, e - (p + 1));
    return true;
}

bool apex_json_int_array(const std::string &json, const char *key, std::vector<long> &out)
{
    size_t p = json_value_pos(json, key);
    if (p == std::string::npos || json[p] != '[') {
        return false;
    }
    const char *s = json.c_str() + p + 1;
    while (*s && *s != ']') {
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n') {
            s++;
        }
        if (*s == ']' || *s == '\0') {
            break;
        }
        char *end = nullptr;
        long v = strtol(s, &end, 0);
        if (end == s) {
            break;
        }
        out.push_back(v);
        s = end;
    }
    return true;
}

/* ---- HTTP/1.0 GET against 127.0.0.1 ---- */

bool apex_pinmame_http_get(int port, const std::string &path, std::string &body, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return false;
    }

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (send(fd, req.data(), req.size(), 0) < 0) {
        close(fd);
        return false;
    }

    std::string resp;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, (size_t)n);
    }
    close(fd);
    if (resp.empty()) {
        return false;
    }
    /* Require a 2xx status, then return the body after the header block. */
    if (resp.compare(0, 5, "HTTP/") == 0) {
        size_t sp = resp.find(' ');
        if (sp != std::string::npos && !(resp[sp + 1] == '2')) {
            return false;
        }
    }
    size_t hdr = resp.find("\r\n\r\n");
    body = (hdr == std::string::npos) ? resp : resp.substr(hdr + 4);
    return true;
}

/* ---- process control ---- */

bool apex_pinmame_port_in_use(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    struct timeval tv = {0, 200000}; /* 200 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool listening = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    close(fd);
    return listening;
}

std::string apex_pinmame_log_tail(const ApexPinmame &pm, size_t max)
{
    if (pm.log_path.empty()) {
        return "";
    }
    FILE *f = fopen(pm.log_path.c_str(), "rb");
    if (!f) {
        return "";
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return "";
    }
    long off = (sz > (long)max) ? sz - (long)max : 0;
    fseek(f, off, SEEK_SET);
    std::string out;
    out.resize((size_t)(sz - off));
    size_t rd = fread(&out[0], 1, out.size(), f);
    fclose(f);
    out.resize(rd);
    return out;
}

bool apex_pinmame_launch(ApexPinmame &pm, const std::string &bin,
                         const std::string &rompath, const std::string &game, int port)
{
    if (bin.empty() || game.empty()) {
        pm.last_error = "binary path and game name are required";
        return false;
    }
    if (access(bin.c_str(), X_OK) != 0) {
        pm.last_error = "xpinmamed binary not found or not executable: " + bin;
        return false;
    }
    if (apex_pinmame_is_alive(pm)) {
        pm.last_error = "already running";
        return false;
    }
    if (apex_pinmame_port_in_use(port)) {
        pm.last_error = "port " + std::to_string(port) + " is already in use";
        return false;
    }
    pm.log_path = "/tmp/apeximgui_xpinmamed_" + std::to_string(port) + ".log";

    /* Build argv in the PARENT: after fork() in a multithreaded process (the GUI
       has an async render thread) the child may only call async-signal-safe
       functions, so no allocation between fork and exec. */
    std::string ports = std::to_string(port);
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(bin.c_str()));
    argv.push_back(const_cast<char *>("-headless"));
    argv.push_back(const_cast<char *>("-startpaused"));
    argv.push_back(const_cast<char *>("-nosound"));
    argv.push_back(const_cast<char *>("-httpport"));
    argv.push_back(const_cast<char *>(ports.c_str()));
    if (!rompath.empty()) {
        argv.push_back(const_cast<char *>("-rompath"));
        argv.push_back(const_cast<char *>(rompath.c_str()));
    }
    argv.push_back(const_cast<char *>(game.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        pm.last_error = "fork failed";
        return false;
    }
    if (pid == 0) {
        /* Capture output to the log file so launch failures (port bind, missing
           romset, exec error) can be surfaced to the user. */
        int lf = open(pm.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf < 0) {
            lf = open("/dev/null", O_WRONLY);
        }
        if (lf >= 0) {
            dup2(lf, STDOUT_FILENO);
            dup2(lf, STDERR_FILENO);
            if (lf > STDERR_FILENO) {
                close(lf);
            }
        }
        execvp(bin.c_str(), argv.data());
        _exit(127); /* exec failed */
    }
    pm.pid = pid;
    pm.port = port;
    pm.running = true;
    pm.last_error.clear();
    return true;
}

bool apex_pinmame_is_alive(ApexPinmame &pm)
{
    if (pm.pid <= 0) {
        return false;
    }
    int st;
    pid_t r = waitpid(pm.pid, &st, WNOHANG);
    if (r == 0) {
        return true; /* still running */
    }
    /* exited (r == pid) or error (r < 0): reap and forget. */
    pm.pid = -1;
    pm.running = false;
    return false;
}

/* ---- /api/events SSE reader (halt notifications) ---- */

static void events_loop(ApexPinmame *pm)
{
    int fails = 0;
    while (!pm->ev_stop.load()) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        struct timeval tv = {0, 400000}; /* 400 ms recv timeout → checks ev_stop */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)pm->port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close(fd);
            if (++fails > 25) break; /* emulator gone: stop retrying (~8s) */
            usleep(300000);
            continue;
        }
        fails = 0;
        std::string req = "GET /api/events HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        send(fd, req.data(), req.size(), 0);

        std::string buf;
        while (!pm->ev_stop.load()) {
            char tmp[2048];
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                buf.append(tmp, (size_t)n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    size_t d = line.find("data:");
                    if (d == std::string::npos) continue;
                    std::string js = line.substr(d + 5);
                    std::string ev;
                    if (!apex_json_str(js, "event", ev) || ev != "halt") continue;
                    long v;
                    std::lock_guard<std::mutex> lk(pm->ev_mtx);
                    pm->ev_halt.seq++;
                    apex_json_str(js, "reason", pm->ev_halt.reason);
                    pm->ev_halt.pc = apex_json_int(js, "pc", v) ? v : 0;
                    pm->ev_halt.bank = apex_json_int(js, "bank", v) ? (int)v : -1;
                    pm->ev_halt.has_addr = apex_json_int(js, "addr", v);
                    pm->ev_halt.addr = pm->ev_halt.has_addr ? v : 0;
                }
            } else if (n == 0) {
                break; /* server closed */
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break; /* real error (not the recv timeout) */
            }
        }
        close(fd);
    }
}

void apex_pinmame_events_start(ApexPinmame &pm)
{
    if (pm.ev_running) return;
    pm.ev_stop = false;
    pm.ev_running = true;
    pm.ev_thread = std::thread(events_loop, &pm);
}

void apex_pinmame_events_stop(ApexPinmame &pm)
{
    pm.ev_stop = true;
    if (pm.ev_thread.joinable()) pm.ev_thread.join();
    pm.ev_running = false;
}

bool apex_pinmame_get_halt(ApexPinmame &pm, ApexPinmameHalt &out)
{
    std::lock_guard<std::mutex> lk(pm.ev_mtx);
    out = pm.ev_halt;
    return pm.ev_halt.seq != 0;
}

bool apex_pinmame_instrument(int port, const char *cmd, unsigned addr, int bank)
{
    char path[128];
    if (!strcmp(cmd, "clear")) {
        snprintf(path, sizeof(path), "/api/debugger/instrument?cmd=clear");
    } else if (bank < 0) {
        snprintf(path, sizeof(path), "/api/debugger/instrument?cmd=%s&addr=0x%x", cmd, addr);
    } else {
        snprintf(path, sizeof(path), "/api/debugger/instrument?cmd=%s&addr=0x%x&bank=0x%x",
                 cmd, addr, (unsigned)bank);
    }
    std::string body;
    return apex_pinmame_http_get(port, path, body);
}

bool apex_pinmame_instrument_read(int port, std::vector<ApexPinmameCount> &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/debugger/instrument", body)) {
        return false;
    }
    long v;
    for (const std::string &o : json_objects(body, "points")) {
        ApexPinmameCount c;
        c.addr = apex_json_int(o, "addr", v) ? v : 0;
        c.bank = apex_json_int(o, "bank", v) ? (int)v : -1;
        c.count = apex_json_int(o, "count", v) ? v : 0;
        out.push_back(c);
    }
    return true;
}

bool apex_pinmame_exectrace_cmd(int port, const char *cmd)
{
    std::string body;
    return apex_pinmame_http_get(port, std::string("/api/debugger/exectrace?cmd=") + cmd, body);
}

bool apex_pinmame_exectrace(int port, std::vector<ApexPinmameTrace> &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/debugger/exectrace", body, 5000)) {
        return false;
    }
    long v;
    for (const std::string &o : json_objects(body, "trace")) {
        ApexPinmameTrace t;
        t.pc = apex_json_int(o, "pc", v) ? v : 0;
        t.bank = apex_json_int(o, "bank", v) ? (int)v : -1;
        t.a = apex_json_int(o, "a", v) ? v : 0;
        t.b = apex_json_int(o, "b", v) ? v : 0;
        t.x = apex_json_int(o, "x", v) ? v : 0;
        out.push_back(t);
    }
    return true;
}

bool apex_pinmame_memory(int port, unsigned addr, int size, int bank,
                         std::vector<uint8_t> &out)
{
    char path[128];
    if (bank < 0) {
        snprintf(path, sizeof(path), "/api/debugger/memory?addr=0x%x&size=%d", addr, size);
    } else {
        snprintf(path, sizeof(path), "/api/debugger/memory?addr=0x%x&size=%d&bank=0x%x",
                 addr, size, (unsigned)bank);
    }
    std::string body;
    if (!apex_pinmame_http_get(port, path, body)) {
        return false;
    }
    std::vector<long> data;
    if (!apex_json_int_array(body, "data", data)) {
        return false;
    }
    for (long v : data) {
        out.push_back((uint8_t)(v & 0xff));
    }
    return true;
}

bool apex_pinmame_dmd(int port, int &w, int &h, std::vector<uint8_t> &lum)
{
    std::string info;
    if (!apex_pinmame_http_get(port, "/api/dmd/info", info)) {
        return false;
    }
    long lw, lh;
    if (!apex_json_int(info, "width", lw) || !apex_json_int(info, "height", lh) ||
        lw <= 0 || lh <= 0 || lw * lh > (1 << 20)) {
        return false;
    }
    std::string raw;
    if (!apex_pinmame_http_get(port, "/api/dmd/raw", raw)) {
        return false;
    }
    if ((long)raw.size() < lw * lh) {
        return false; /* short/blank frame */
    }
    w = (int)lw;
    h = (int)lh;
    lum.assign(raw.begin(), raw.begin() + (size_t)(lw * lh));
    return true;
}

void apex_pinmame_stop(ApexPinmame &pm)
{
    apex_pinmame_events_stop(pm);
    if (pm.pid > 0) {
        kill(pm.pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            if (waitpid(pm.pid, nullptr, WNOHANG) == pm.pid) {
                pm.pid = -1;
                break;
            }
            usleep(50000); /* up to ~1s for a graceful exit */
        }
        if (pm.pid > 0) {
            kill(pm.pid, SIGKILL);
            waitpid(pm.pid, nullptr, 0);
            pm.pid = -1;
        }
    }
    pm.running = false;
}

/* ---- parsed endpoints ---- */

bool apex_pinmame_get_info(int port, ApexPinmameInfo &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/info", body)) {
        return false;
    }
    long v;
    apex_json_str(body, "game", out.game);
    apex_json_str(body, "description", out.description);
    if (apex_json_int(body, "paused", v)) out.paused = (int)v;
    if (apex_json_int(body, "wpc_bank", v)) out.wpc_bank = (int)v;
    out.valid = !out.game.empty();
    return out.valid;
}

bool apex_pinmame_get_cpu0(int port, ApexPinmameCpu &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/debugger/state", body)) {
        return false;
    }
    /* Parse the first CPU object inside "cpus":[ {...}, ... ]. */
    size_t c = body.find("\"cpus\"");
    size_t open = (c == std::string::npos) ? std::string::npos : body.find('{', c);
    size_t close = (open == std::string::npos) ? std::string::npos : body.find('}', open);
    if (open == std::string::npos || close == std::string::npos) {
        return false;
    }
    std::string cpu0 = body.substr(open, close - open + 1);
    long v;
    if (apex_json_int(cpu0, "pc", v)) out.pc = v;
    if (apex_json_int(cpu0, "sp", v)) out.sp = v;
    if (apex_json_int(cpu0, "a",  v)) out.a  = v;
    if (apex_json_int(cpu0, "b",  v)) out.b  = v;
    if (apex_json_int(cpu0, "x",  v)) out.x  = v;
    if (apex_json_int(cpu0, "y",  v)) out.y  = v;
    if (apex_json_int(cpu0, "u",  v)) out.u  = v;
    if (apex_json_int(cpu0, "dp", v)) out.dp = v;
    if (apex_json_int(cpu0, "cc", v)) out.cc = v;
    out.valid = true;
    return true;
}

bool apex_pinmame_control(int port, const char *cmd)
{
    std::string body;
    return apex_pinmame_http_get(port, std::string("/api/debugger/control?cmd=") + cmd, body);
}

/* Return each flat {...} object substring inside the array named `key`. */
static std::vector<std::string> json_objects(const std::string &json, const char *key)
{
    std::vector<std::string> out;
    std::string tok = std::string("\"") + key + "\"";
    size_t k = json.find(tok);
    if (k == std::string::npos) {
        return out;
    }
    size_t lb = json.find('[', k);
    if (lb == std::string::npos) {
        return out;
    }
    size_t i = lb + 1;
    while (i < json.size() && json[i] != ']') {
        if (json[i] == '{') {
            size_t e = json.find('}', i); /* objects here have no nested braces */
            if (e == std::string::npos) break;
            out.push_back(json.substr(i, e - i + 1));
            i = e + 1;
        } else {
            i++;
        }
    }
    return out;
}

/* Percent-encode a query-parameter value (condition expressions have =, <, >, …). */
static std::string url_encode(const char *s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0xf];
        }
    }
    return o;
}

bool apex_pinmame_breakpoint(int port, const char *cmd, unsigned addr, int bank,
                             const char *cond)
{
    char path[256];
    if (bank < 0) {
        snprintf(path, sizeof(path), "/api/debugger/breakpoints?cmd=%s&addr=0x%x", cmd, addr);
    } else {
        snprintf(path, sizeof(path), "/api/debugger/breakpoints?cmd=%s&addr=0x%x&bank=0x%x",
                 cmd, addr, (unsigned)bank);
    }
    std::string p = path;
    if (cond && *cond) {
        /* The debugger wants REG op HEX with an uppercase register and no 0x
           prefix (e.g. A==7F); normalise so "a==0x7f" is accepted too. */
        std::string cc;
        for (const char *q = cond; *q;) {
            if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { q += 2; continue; }
            char ch = *q++;
            if (ch == ' ' || ch == '\t') continue; /* the parser rejects whitespace */
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            cc += ch;
        }
        p += "&cond=";
        p += url_encode(cc.c_str());
    }
    std::string body;
    return apex_pinmame_http_get(port, p, body);
}

bool apex_pinmame_watchpoint(int port, const char *cmd, unsigned addr, int bank,
                             int len, int mode)
{
    char path[256];
    if (bank < 0) {
        snprintf(path, sizeof(path),
                 "/api/debugger/watchpoints?cmd=%s&addr=0x%x&len=%d&mode=%d", cmd, addr, len, mode);
    } else {
        snprintf(path, sizeof(path),
                 "/api/debugger/watchpoints?cmd=%s&addr=0x%x&len=%d&mode=%d&bank=0x%x",
                 cmd, addr, len, mode, (unsigned)bank);
    }
    std::string body;
    return apex_pinmame_http_get(port, path, body);
}

bool apex_pinmame_input(int port, int sw, int val, int pulse_ms)
{
    char path[128];
    if (pulse_ms > 0) {
        snprintf(path, sizeof(path), "/api/input?sw=%d&val=%d&pulse=%d", sw, val, pulse_ms);
    } else {
        snprintf(path, sizeof(path), "/api/input?sw=%d&val=%d", sw, val);
    }
    std::string body;
    return apex_pinmame_http_get(port, path, body);
}

bool apex_pinmame_points(int port, std::vector<ApexPinmamePoint> &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/debugger/points", body)) {
        return false;
    }
    long v;
    for (const std::string &o : json_objects(body, "breakpoints")) {
        ApexPinmamePoint p;
        p.is_wp = 0;
        if (apex_json_int(o, "idx", v))  p.idx = (int)v;
        if (apex_json_int(o, "addr", v)) p.addr = v;
        if (apex_json_int(o, "bank", v)) p.bank = (int)v;
        if (apex_json_int(o, "hits", v)) p.hits = v;
        apex_json_str(o, "cond", p.cond);
        out.push_back(p);
    }
    for (const std::string &o : json_objects(body, "watchpoints")) {
        ApexPinmamePoint p;
        p.is_wp = 1;
        if (apex_json_int(o, "idx", v))  p.idx = (int)v;
        if (apex_json_int(o, "addr", v)) p.addr = v;
        if (apex_json_int(o, "bank", v)) p.bank = (int)v;
        if (apex_json_int(o, "mode", v)) p.mode = (int)v;
        if (apex_json_int(o, "len", v))  p.len = (int)v;
        apex_json_str(o, "cond", p.cond);
        out.push_back(p);
    }
    return true;
}

bool apex_pinmame_point_delete(int port, int is_wp, int idx)
{
    std::vector<ApexPinmamePoint> pts;
    if (!apex_pinmame_points(port, pts)) {
        return false;
    }
    /* Clear the whole type, then re-add every point of that type except idx. */
    if (is_wp) {
        apex_pinmame_watchpoint(port, "clear", 0, -1, 1, 2);
    } else {
        apex_pinmame_breakpoint(port, "clear", 0, -1, nullptr);
    }
    for (const ApexPinmamePoint &p : pts) {
        if (p.is_wp != is_wp || p.idx == idx) {
            continue;
        }
        if (is_wp) {
            apex_pinmame_watchpoint(port, "add", (unsigned)p.addr, p.bank,
                                    p.len > 0 ? p.len : 1, p.mode);
        } else {
            apex_pinmame_breakpoint(port, "add", (unsigned)p.addr, p.bank,
                                    p.cond.empty() ? nullptr : p.cond.c_str());
        }
    }
    return true;
}

bool apex_pinmame_switches(int port, std::vector<ApexPinmameSwitch> &out)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/switches", body)) {
        return false;
    }
    long v;
    for (const std::string &o : json_objects(body, "switches")) {
        ApexPinmameSwitch s;
        if (apex_json_int(o, "num", v))    s.num = (int)v;
        if (apex_json_int(o, "col", v))    s.col = (int)v;
        if (apex_json_int(o, "row", v))    s.row = (int)v;
        if (apex_json_int(o, "active", v)) s.active = (int)v;
        apex_json_str(o, "name", s.name);
        out.push_back(s);
    }
    return true;
}

bool apex_pinmame_coverage_cmd(int port, const char *cmd)
{
    std::string body;
    return apex_pinmame_http_get(port, std::string("/api/debugger/coverage?cmd=") + cmd, body);
}

bool apex_pinmame_coverage_summary(int port, long &executed, long &addressable)
{
    std::string body;
    if (!apex_pinmame_http_get(port, "/api/debugger/coverage", body)) {
        return false;
    }
    return apex_json_int(body, "executed", executed) &&
           apex_json_int(body, "addressable", addressable);
}

bool apex_pinmame_coverage_window(int port, int bank, unsigned addr, unsigned size,
                                  std::vector<uint8_t> &flags)
{
    const unsigned CHUNK = 2048; /* mirrors the memory endpoint's per-call cap */
    for (unsigned off = 0; off < size; off += CHUNK) {
        unsigned n = std::min(CHUNK, size - off);
        char path[128];
        if (bank < 0) {
            snprintf(path, sizeof(path),
                     "/api/debugger/coverage?addr=0x%x&size=%u", addr + off, n);
        } else {
            snprintf(path, sizeof(path),
                     "/api/debugger/coverage?addr=0x%x&size=%u&bank=0x%x", addr + off, n,
                     (unsigned)bank);
        }
        std::string body;
        if (!apex_pinmame_http_get(port, path, body)) {
            return false;
        }
        std::vector<long> arr;
        if (!apex_json_int_array(body, "executed", arr)) {
            return false;
        }
        for (long v : arr) {
            flags.push_back(v ? 1 : 0);
        }
    }
    return true;
}
