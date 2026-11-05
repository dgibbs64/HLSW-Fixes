#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "exports.h"

void *real_fn[EXPORT_COUNT];

typedef int (WINAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WINAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WINAPI *recv_t)(SOCKET, char *, int, int);
typedef int (WINAPI *send_t)(SOCKET, const char *, int, int);
typedef int (WINAPI *wsasendto_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int,
                                  LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *wsarecvfrom_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT,
                                    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WINAPI *getpeername_t)(SOCKET, struct sockaddr *, int *);

static recvfrom_t orig_recvfrom;
static sendto_t orig_sendto;
static recv_t orig_recv;
static send_t orig_send;
static wsasendto_t orig_WSASendTo;
static wsarecvfrom_t orig_WSARecvFrom;
static getpeername_t ws_getpeername;

#define A2S_INFO_LEN 25
#define PENDING_MAX 512
#define PENDING_TTL_MS 15000

static const unsigned char a2s_info[A2S_INFO_LEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 'T', 'S', 'o', 'u', 'r', 'c', 'e', ' ', 'E', 'n', 'g',
    'i', 'n', 'e', ' ', 'Q', 'u', 'e', 'r', 'y', 0x00
};

typedef struct {
    SOCKET s;
    ULONG ip;
    USHORT port;
    DWORD info_time;
    DWORD chal_req_time;
    int info_pending;
    int info_retried;
} pending_t;

static pending_t pending[PENDING_MAX];
static CRITICAL_SECTION lock;
static FILE *logf;
static int verbose;

static void logmsg(const char *fmt, ...)
{
    if (!logf)
        return;
    va_list ap;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&lock);
    fprintf(logf, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&lock);
}

static const char *addrstr(ULONG ip, USHORT port, char *buf)
{
    unsigned char *b = (unsigned char *)&ip;
    sprintf(buf, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (unsigned)(((port & 0xFF) << 8) | (port >> 8)));
    return buf;
}

static void logpkt(const char *api, const struct sockaddr *sa, const unsigned char *b, int len)
{
    if (!logf || !verbose)
        return;
    char as[32] = "?", hex[3 * 24 + 1];
    if (sa && sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        addrstr(sin->sin_addr.s_addr, sin->sin_port, as);
    }
    int n = len < 24 ? (len < 0 ? 0 : len) : 24;
    for (int i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02X ", b[i]);
    hex[n * 3] = 0;
    logmsg("%s %s len=%d %s", api, as, len, hex);
}

static pending_t *find_slot(SOCKET s, ULONG ip, USHORT port, int create)
{
    DWORD now = GetTickCount();
    pending_t *free_slot = NULL;
    for (int i = 0; i < PENDING_MAX; i++) {
        pending_t *p = &pending[i];
        if (p->s == s && p->ip == ip && p->port == port && p->s != 0)
            return p;
        if (!free_slot && (p->s == 0 ||
            (now - p->info_time > PENDING_TTL_MS && now - p->chal_req_time > PENDING_TTL_MS)))
            free_slot = p;
    }
    if (!create || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->s = s;
    free_slot->ip = ip;
    free_slot->port = port;
    free_slot->info_time = now - PENDING_TTL_MS - 1;
    free_slot->chal_req_time = now - PENDING_TTL_MS - 1;
    return free_slot;
}

static int is_header(const unsigned char *b, int len)
{
    return len >= 5 && b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF;
}

static void on_send(const char *api, SOCKET s, const struct sockaddr *to, const char *buf, int len)
{
    const unsigned char *b = (const unsigned char *)buf;
    logpkt(api, to, b, len);
    if (!to || to->sa_family != AF_INET || !is_header(b, len))
        return;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)to;

    EnterCriticalSection(&lock);
    if (len == A2S_INFO_LEN && memcmp(b, a2s_info, A2S_INFO_LEN) == 0) {
        pending_t *p = find_slot(s, sin->sin_addr.s_addr, sin->sin_port, 1);
        if (p) {
            p->info_pending = 1;
            p->info_retried = 0;
            p->info_time = GetTickCount();
        }
    } else if (b[4] == 0x55 || b[4] == 0x56 || b[4] == 0x57) {
        pending_t *p = find_slot(s, sin->sin_addr.s_addr, sin->sin_port, 1);
        if (p)
            p->chal_req_time = GetTickCount();
    }
    LeaveCriticalSection(&lock);
}

static int on_recv(const char *api, SOCKET s, const struct sockaddr *from, const char *buf, int n)
{
    const unsigned char *b = (const unsigned char *)buf;
    logpkt(api, from, b, n);
    if (!from || from->sa_family != AF_INET || !is_header(b, n))
        return 0;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)from;
    char as[32];
    DWORD now = GetTickCount();

    if (b[4] == 0x49 || b[4] == 0x6D) {
        EnterCriticalSection(&lock);
        pending_t *p = find_slot(s, sin->sin_addr.s_addr, sin->sin_port, 0);
        if (p)
            p->info_pending = 0;
        LeaveCriticalSection(&lock);
        return 0;
    }
    if (b[4] != 0x41 || n < 9)
        return 0;

    EnterCriticalSection(&lock);
    pending_t *p = find_slot(s, sin->sin_addr.s_addr, sin->sin_port, 0);
    int resend = p && p->info_pending && !p->info_retried && now - p->info_time < PENDING_TTL_MS;
    int pass = 1;
    if (resend) {
        p->info_retried = 1;
        if (now - p->chal_req_time < PENDING_TTL_MS)
            p->chal_req_time = now - PENDING_TTL_MS - 1;
        else
            pass = 0;
    }
    LeaveCriticalSection(&lock);
    if (!resend)
        return 0;

    unsigned char req[A2S_INFO_LEN + 4];
    memcpy(req, a2s_info, A2S_INFO_LEN);
    memcpy(req + A2S_INFO_LEN, b + 5, 4);
    int r = orig_sendto(s, (const char *)req, sizeof(req), 0, from, sizeof(struct sockaddr_in));
    logmsg("challenge %02X%02X%02X%02X from %s, resent A2S_INFO (%d), %s", b[5], b[6], b[7], b[8],
           addrstr(sin->sin_addr.s_addr, sin->sin_port, as), r, pass ? "passed through" : "swallowed");
    return pass ? 0 : 1;
}

static int WINAPI hook_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    on_send("sendto", s, to, buf, len);
    return orig_sendto(s, buf, len, flags, to, tolen);
}

static int WINAPI hook_send(SOCKET s, const char *buf, int len, int flags)
{
    struct sockaddr_in peer;
    int plen = sizeof(peer);
    if (ws_getpeername(s, (struct sockaddr *)&peer, &plen) == 0 && peer.sin_family == AF_INET)
        on_send("send", s, (struct sockaddr *)&peer, buf, len);
    return orig_send(s, buf, len, flags);
}

static int WINAPI hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags,
                                 const struct sockaddr *to, int tolen, LPWSAOVERLAPPED ovl,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (bufs && count >= 1)
        on_send("WSASendTo", s, to, bufs[0].buf, (int)bufs[0].len);
    return orig_WSASendTo(s, bufs, count, sent, flags, to, tolen, ovl, cr);
}

static int WINAPI hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    for (;;) {
        struct sockaddr_storage ss;
        int sslen = sizeof(ss);
        int n = orig_recvfrom(s, buf, len, flags, (struct sockaddr *)&ss, &sslen);
        if (n == SOCKET_ERROR)
            return n;
        if (!(flags & MSG_PEEK) && on_recv("recvfrom", s, (struct sockaddr *)&ss, buf, n))
            continue;
        if (from && fromlen) {
            int c = sslen < *fromlen ? sslen : *fromlen;
            memcpy(from, &ss, c);
            *fromlen = sslen;
        }
        return n;
    }
}

static int WINAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    for (;;) {
        int n = orig_recv(s, buf, len, flags);
        if (n == SOCKET_ERROR || (flags & MSG_PEEK))
            return n;
        struct sockaddr_in peer;
        int plen = sizeof(peer);
        if (ws_getpeername(s, (struct sockaddr *)&peer, &plen) == 0 && peer.sin_family == AF_INET &&
            on_recv("recv", s, (struct sockaddr *)&peer, buf, n))
            continue;
        return n;
    }
}

static int WINAPI hook_WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD recvd, LPDWORD flags,
                                   struct sockaddr *from, LPINT fromlen, LPWSAOVERLAPPED ovl,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    if (ovl || cr || !bufs || count < 1 || !recvd) {
        static int warned;
        if (!warned++)
            logmsg("WSARecvFrom overlapped call seen, not inspected");
        return orig_WSARecvFrom(s, bufs, count, recvd, flags, from, fromlen, ovl, cr);
    }
    for (;;) {
        struct sockaddr_storage ss;
        int sslen = sizeof(ss);
        DWORD fl = flags ? *flags : 0;
        int r = orig_WSARecvFrom(s, bufs, count, recvd, &fl, (struct sockaddr *)&ss, &sslen, NULL, NULL);
        if (r == SOCKET_ERROR)
            return r;
        if (!(fl & MSG_PEEK) && on_recv("WSARecvFrom", s, (struct sockaddr *)&ss, bufs[0].buf, (int)*recvd))
            continue;
        if (flags)
            *flags = fl;
        if (from && fromlen) {
            int c = sslen < *fromlen ? sslen : *fromlen;
            memcpy(from, &ss, c);
            *fromlen = sslen;
        }
        return r;
    }
}

static void *hotpatch(HMODULE mod, const char *name, void *hook)
{
    unsigned char *p = (unsigned char *)GetProcAddress(mod, name);
    if (!p)
        return NULL;
    if (p[0] != 0x8B || p[1] != 0xFF) {
        logmsg("cannot hook %s: unexpected prologue %02X %02X", name, p[0], p[1]);
        return NULL;
    }
    for (int i = 1; i <= 5; i++)
        if (p[-i] != 0xCC && p[-i] != 0x90) {
            logmsg("cannot hook %s: no patch space", name);
            return NULL;
        }
    DWORD old;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    p[-5] = 0xE9;
    *(DWORD *)(p - 4) = (DWORD)((unsigned char *)hook - p);
    *(volatile WORD *)p = 0xF9EB;
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    return p + 2;
}

static void open_log(HINSTANCE self)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    char *slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy(slash + 1, "wsock32_a2s_verbose.log");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        verbose = 1;
        logf = fopen(path, "a");
        return;
    }
    strcpy(slash + 1, "wsock32_a2s.log");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        logf = fopen(path, "a");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&lock);

    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n + 14 >= MAX_PATH)
        return FALSE;
    strcat(path, "\\wsock32.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real)
        return FALSE;
    for (int i = 0; i < EXPORT_COUNT; i++)
        real_fn[i] = (void *)GetProcAddress(real, export_names[i]);

    open_log(inst);

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2)
        ws2 = LoadLibraryA("ws2_32.dll");
    if (!ws2)
        return TRUE;
    ws_getpeername = (getpeername_t)GetProcAddress(ws2, "getpeername");
    orig_sendto = (sendto_t)hotpatch(ws2, "sendto", (void *)hook_sendto);
    orig_recvfrom = (recvfrom_t)hotpatch(ws2, "recvfrom", (void *)hook_recvfrom);
    orig_send = (send_t)hotpatch(ws2, "send", (void *)hook_send);
    orig_recv = (recv_t)hotpatch(ws2, "recv", (void *)hook_recv);
    orig_WSASendTo = (wsasendto_t)hotpatch(ws2, "WSASendTo", (void *)hook_WSASendTo);
    orig_WSARecvFrom = (wsarecvfrom_t)hotpatch(ws2, "WSARecvFrom", (void *)hook_WSARecvFrom);
    logmsg("A2S challenge shim loaded: sendto=%d recvfrom=%d send=%d recv=%d WSASendTo=%d WSARecvFrom=%d",
           !!orig_sendto, !!orig_recvfrom, !!orig_send, !!orig_recv, !!orig_WSASendTo, !!orig_WSARecvFrom);
    return TRUE;
}
