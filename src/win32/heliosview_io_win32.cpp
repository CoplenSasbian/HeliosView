// HeliosView.dll -- Windows async I/O implementation: IOCP thread pool + async TCP/file.
// Cross-platform interface: see heliosview.h (async I/O section); this file implements only the win32 part.
#include <HeliosView/heliosview.h>
#include "../heliosview_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mswsock.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ================= Completion / task kinds ================= */

enum class OpKind : uint8_t {
    Pool,       /* thread pool task (no OVERLAPPED) */
    Connect,    /* thread pool task: initiate connect (no OVERLAPPED) */
    Open,       /* thread pool task: open file (no OVERLAPPED) */
    SocketConnect, /* IOCP completion: connect */
    SocketWrite,   /* IOCP completion: send */
    SocketRead,    /* IOCP completion: streaming receive */
    FileRead,   /* IOCP completion: file read */
    FileWrite,  /* IOCP completion: file write */
};

/* Common header for thread pool tasks */
struct task_base {
    OpKind kind;
};

struct pool_task : task_base {
    heliosview_completion_cb cb;
    void* userdata;
};

struct connect_task : task_base {
    heliosview_loop* loop;
    heliosview_socket* tcp;
    char host[256];
    uint16_t port;
    heliosview_socket_connect_cb cb;
    void* userdata;
};

struct open_task : task_base {
    heliosview_loop* loop;
    std::wstring path;
    bool write_mode;
    heliosview_file_open_cb cb;
    void* userdata;
};

/* Common header for IO ops: OVERLAPPED must be the first member (completion packet recovered via reinterpret_cast) */
struct io_op {
    OVERLAPPED ov{};
    OpKind kind;
};

struct socket_connect_op : io_op {
    SOCKET socket;
    SOCKADDR_IN addr{};
    heliosview_socket* tcp;
    heliosview_socket_connect_cb cb;
    void* userdata;
};

struct socket_write_op : io_op {
    SOCKET socket;
    WSABUF wsa;
    heliosview_transfer_cb cb;
    void* userdata;
};

struct socket_read_op : io_op {
    SOCKET socket;
    WSABUF wsa;
    char buf[64 * 1024];
    heliosview_read_cb cb;
    void* userdata;
    heliosview_socket* tcp = nullptr;   /* owning tcp (for the termination protocol) */
    std::atomic<bool> cancelled{false}; /* set by read_stop/close, read by worker threads */
    bool tcp_owned = false;          /* close() transfers tcp ownership: freed together on cancellation */
};

struct file_io_op : io_op {
    HANDLE file;
    heliosview_transfer_cb cb;
    void* userdata;
};

/* ================= Handle structures (complete the opaque declarations from the header; must be at global scope) ================= */

struct heliosview_loop {
    HANDLE iocp = nullptr;
    HANDLE stop_event = nullptr;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
};

struct heliosview_socket {
    heliosview_loop* loop;
    SOCKET socket = INVALID_SOCKET;
    socket_read_op* read_op = nullptr;  /* active streaming read (user-side access only) */
    bool read_finished = false;      /* read finished (EOF/error): read_op may already be freed; close must not touch it */
};

struct heliosview_file {
    heliosview_loop* loop;
    HANDLE handle;
};

namespace {

/* Forward declarations (called from worker_main) */
void do_connect(connect_task* task);
void do_open(open_task* task);

/* Winsock reference counting (multiple loops may coexist) */
void ensure_winsock()
{
    static std::mutex m;
    static int refs = 0;
    std::lock_guard<std::mutex> lock(m);
    if (refs++ == 0) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
}

void release_winsock()
{
    static std::mutex m;
    static int refs = 0;
    std::lock_guard<std::mutex> lock(m);
    if (--refs == 0)
        WSACleanup();
}

/* ================= Worker threads ================= */

/* SocketRead termination protocol:
 *   - cancelled and close() transferred ownership (tcp_owned) -> worker frees tcp together with the op
 *   - cancelled via read_stop only (!tcp_owned) -> clear read_op, allow read_start again
 *   - EOF/error (not cancelled) -> set read_finished; close() no longer touches this op */
void finish_read_op(socket_read_op* r)
{
    if (r->cancelled.load()) {
        if (r->tcp_owned) {
            hv::hv_dealloc(r->tcp); /* close() has transferred tcp ownership to this op */
        } else {
            r->tcp->read_op = nullptr; /* read_stop(): reading can be restarted */
        }
    } else {
        r->tcp->read_finished = true; /* EOF/error: op is finished */
    }
    hv::hv_dealloc(r);
}

void worker_main(heliosview_loop* loop)
{
    for (;;) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* ov = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(loop->iocp, &bytes, &key, &ov, INFINITE);

        /* ---- thread pool tasks (no OVERLAPPED) ---- */
        if (ov == nullptr) {
            if (!ok && GetLastError() == ERROR_ABANDONED_WAIT_0)
                return; /* IOCP was closed */
            if (key == reinterpret_cast<ULONG_PTR>(loop))
                return; /* exit sentinel */
            auto* task = reinterpret_cast<task_base*>(key);
            switch (task->kind) {
            case OpKind::Pool: {
                auto* t = static_cast<pool_task*>(task);
                t->cb(0, t->userdata);
                hv::hv_dealloc(t);
                break;
            }
            case OpKind::Connect:
                do_connect(static_cast<connect_task*>(task));
                break;
            case OpKind::Open:
                do_open(static_cast<open_task*>(task));
                break;
            default:
                hv::hv_dealloc(task);
                break;
            }
            continue;
        }

        /* ---- IO completion: ov is the io_op address (OVERLAPPED is first) ---- */
        auto* op = reinterpret_cast<io_op*>(ov);
        const int error = ok ? 0 : -(int)GetLastError();

        switch (op->kind) {
        case OpKind::SocketConnect: {
            auto* c = static_cast<socket_connect_op*>(op);
            if (error == 0) {
                setsockopt(c->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
                c->cb(0, c->tcp, c->userdata);
            } else {
                closesocket(c->socket);
                hv::hv_dealloc(c->tcp);
                c->cb(error, nullptr, c->userdata);
            }
            hv::hv_dealloc(c);
            break;
        }

        case OpKind::SocketWrite: {
            auto* w = static_cast<socket_write_op*>(op);
            w->cb(error, bytes, w->userdata);
            hv::hv_dealloc(w);
            break;
        }

        case OpKind::SocketRead: {
            auto* r = static_cast<socket_read_op*>(op);
            if (r->cancelled.load()) {
                finish_read_op(r); /* cancelled: release silently */
                break;
            }
            if (error != 0) {
                r->cb(error, nullptr, 0, r->userdata);
                finish_read_op(r);
                break;
            }
            if (bytes == 0) {
                r->cb(0, nullptr, 0, r->userdata); /* peer closed */
                finish_read_op(r);
                break;
            }
            r->cb(0, r->buf, bytes, r->userdata);
            /* continue reading */
            if (r->cancelled.load()) {
                finish_read_op(r);
                break;
            }
            r->wsa.buf = r->buf;
            r->wsa.len = sizeof(r->buf);
            std::memset(&r->ov, 0, sizeof(r->ov));
            DWORD received = 0;
            DWORD flags = 0;
            const int rc = WSARecv(r->socket, &r->wsa, 1, &received, &flags, &r->ov, nullptr);
            if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING) {
                r->cb(-(int)WSAGetLastError(), nullptr, 0, r->userdata);
                finish_read_op(r);
            }
            break;
        }

        case OpKind::FileRead:
        case OpKind::FileWrite: {
            auto* f = static_cast<file_io_op*>(op);
            f->cb(error, bytes, f->userdata);
            hv::hv_dealloc(f);
            break;
        }

        default:
            hv::hv_dealloc(op); /* unreachable */
            break;
        }
    }
}

/* ================= Thread pool task implementations ================= */

void do_connect(connect_task* task) /* on a worker thread: resolve + initiate ConnectEx */
{
    const auto fail = [&](int error) {
        if (task->tcp->socket != INVALID_SOCKET)
            closesocket(task->tcp->socket);
        hv::hv_dealloc(task->tcp);
        task->cb(error, nullptr, task->userdata);
        hv::hv_dealloc(task);
    };

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", task->port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(task->host, port_str, &hints, &res) != 0)
        return fail(-1);

    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return fail(-2);
    }
    task->tcp->socket = s;

    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), task->loop->iocp,
                                reinterpret_cast<ULONG_PTR>(task->loop), 0)) {
        freeaddrinfo(res);
        return fail(-3);
    }

    SOCKADDR_IN any{};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));

    /* ConnectEx has no exported symbol; fetch the extension function pointer via WSAIoctl */
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX connect_ex = nullptr;
    DWORD ext_bytes = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                 &connect_ex, sizeof(connect_ex), &ext_bytes, nullptr, nullptr) != 0) {
        freeaddrinfo(res);
        return fail(-4);
    }

    auto* op = hv::hv_alloc<socket_connect_op>();
    op->kind = OpKind::SocketConnect;
    op->socket = s;
    op->addr = *reinterpret_cast<SOCKADDR_IN*>(res->ai_addr);
    op->tcp = task->tcp;
    op->cb = task->cb;
    op->userdata = task->userdata;
    freeaddrinfo(res);

    if (!connect_ex(s, reinterpret_cast<sockaddr*>(&op->addr), sizeof(op->addr),
                    nullptr, 0, nullptr, &op->ov)) {
        const int err = WSAGetLastError();
        if (err != ERROR_IO_PENDING) {
            hv::hv_dealloc(op);
            return fail(-err);
        }
    }
    /* Pending or immediate: the completion is posted to the IOCP either way */
    hv::hv_dealloc(task);
}

void do_open(open_task* task) /* on a worker thread: CreateFile + associate with IOCP */
{
    const DWORD access = task->write_mode ? GENERIC_WRITE : GENERIC_READ;
    const DWORD disp = task->write_mode ? CREATE_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(task->path.c_str(), access, FILE_SHARE_READ, nullptr, disp,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        task->cb(-(int)GetLastError(), nullptr, task->userdata);
        hv::hv_dealloc(task);
        return;
    }
    if (!CreateIoCompletionPort(h, task->loop->iocp,
                                reinterpret_cast<ULONG_PTR>(task->loop), 0)) {
        const int err = -(int)GetLastError();
        CloseHandle(h);
        task->cb(err, nullptr, task->userdata);
        hv::hv_dealloc(task);
        return;
    }
    auto* file = hv::hv_alloc<heliosview_file>();
    file->loop = task->loop;
    file->handle = h;
    task->cb(0, file, task->userdata);
    hv::hv_dealloc(task);
}

} // namespace

/* ================= Loop API ================= */

heliosview_loop_t* heliosview_loop_create(unsigned thread_count)
{
    ensure_winsock();
    auto* loop = hv::hv_alloc<heliosview_loop>();
    loop->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    loop->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!loop->iocp || !loop->stop_event) {
        if (loop->iocp)
            CloseHandle(loop->iocp);
        if (loop->stop_event)
            CloseHandle(loop->stop_event);
        hv::hv_dealloc(loop);
        release_winsock();
        return nullptr;
    }
    if (thread_count == 0)
        thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0 || thread_count > 64)
        thread_count = 4;
    for (unsigned i = 0; i < thread_count; ++i)
        loop->workers.emplace_back(worker_main, loop);
    return loop;
}

void heliosview_loop_destroy(heliosview_loop_t* loop)
{
    if (!loop)
        return;
    heliosview_loop_stop(loop);
    for (auto& w : loop->workers)
        w.join();
    CloseHandle(loop->iocp);
    CloseHandle(loop->stop_event);
    hv::hv_dealloc(loop);
    release_winsock();
}

int heliosview_loop_run(heliosview_loop_t* loop)
{
    if (!loop)
        return -1;
    WaitForSingleObject(loop->stop_event, INFINITE);
    return 0;
}

void heliosview_loop_stop(heliosview_loop_t* loop)
{
    if (!loop || loop->stopping.exchange(true))
        return;
    SetEvent(loop->stop_event);
    /* one exit sentinel per worker thread */
    for (std::size_t i = 0; i < loop->workers.size(); ++i)
        PostQueuedCompletionStatus(loop->iocp, 0, reinterpret_cast<ULONG_PTR>(loop), nullptr);
}

int heliosview_loop_post(heliosview_loop_t* loop, heliosview_completion_cb fn, void* userdata)
{
    if (!loop || !fn)
        return -1;
    auto* task = hv::hv_alloc<pool_task>();
    task->kind = OpKind::Pool;
    task->cb = fn;
    task->userdata = userdata;
    if (!PostQueuedCompletionStatus(loop->iocp, 0, reinterpret_cast<ULONG_PTR>(task), nullptr)) {
        hv::hv_dealloc(task);
        return -1;
    }
    return 0;
}

/* ================= Async TCP ================= */

int heliosview_socket_connect(heliosview_loop_t* loop, const char* host, uint16_t port,
                           heliosview_socket_connect_cb on_connect, void* userdata)
{
    if (!loop || !host || !on_connect)
        return -1;

    auto* task = hv::hv_alloc<connect_task>();
    task->kind = OpKind::Connect;
    task->loop = loop;
    task->tcp = hv::hv_alloc<heliosview_socket>();
    task->tcp->loop = loop;
    task->port = port;
    task->cb = on_connect;
    task->userdata = userdata;
    std::snprintf(task->host, sizeof(task->host), "%s", host);

    if (!PostQueuedCompletionStatus(loop->iocp, 0, reinterpret_cast<ULONG_PTR>(task), nullptr)) {
        hv::hv_dealloc(task->tcp);
        hv::hv_dealloc(task);
        return -1;
    }
    return 0;
}

int heliosview_socket_write(heliosview_socket_t* tcp, const void* data, uint32_t len,
                         heliosview_transfer_cb on_write, void* userdata)
{
    if (!tcp || !data || !on_write)
        return -1;
    auto* op = hv::hv_alloc<socket_write_op>();
    op->kind = OpKind::SocketWrite;
    op->socket = tcp->socket;
    op->wsa.buf = const_cast<char*>(static_cast<const char*>(data));
    op->wsa.len = len;
    op->cb = on_write;
    op->userdata = userdata;
    DWORD sent = 0;
    const int rc = WSASend(tcp->socket, &op->wsa, 1, &sent, 0, &op->ov, nullptr);
    if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        const int err = -(int)WSAGetLastError();
        hv::hv_dealloc(op);
        return err;
    }
    return 0;
}

int heliosview_socket_read_start(heliosview_socket_t* tcp, heliosview_read_cb on_read, void* userdata)
{
    if (!tcp || !on_read)
        return -1;
    if (tcp->read_op && !tcp->read_finished)
        return -2; /* already reading */
    tcp->read_op = nullptr; /* clear stale pointer from a finished read */
    auto* op = hv::hv_alloc<socket_read_op>();
    op->kind = OpKind::SocketRead;
    op->socket = tcp->socket;
    op->tcp = tcp;
    op->wsa.buf = op->buf;
    op->wsa.len = sizeof(op->buf);
    op->cb = on_read;
    op->userdata = userdata;
    tcp->read_op = op;

    DWORD received = 0;
    DWORD flags = 0;
    const int rc = WSARecv(tcp->socket, &op->wsa, 1, &received, &flags, &op->ov, nullptr);
    if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        tcp->read_op = nullptr;
        hv::hv_dealloc(op);
        return -(int)WSAGetLastError();
    }
    return 0;
}

void heliosview_socket_read_stop(heliosview_socket_t* tcp)
{
    if (!tcp || !tcp->read_op || tcp->read_finished)
        return;
    tcp->read_op->cancelled = true;
    CancelIoEx(reinterpret_cast<HANDLE>(tcp->socket), &tcp->read_op->ov);
    /* op freed by the worker when cancellation completes (tcp stays caller-owned; read_start may restart) */
}

void heliosview_socket_close(heliosview_socket_t* tcp)
{
    if (!tcp)
        return;
    const bool active_read = tcp->read_op != nullptr && !tcp->read_finished;
    if (active_read) {
        tcp->read_op->cancelled = true;
        tcp->read_op->tcp_owned = true; /* active read: tcp ownership moves to this op, freed with it on cancellation */
        CancelIoEx(reinterpret_cast<HANDLE>(tcp->socket), &tcp->read_op->ov);
    }
    closesocket(tcp->socket);
    if (!active_read)
        hv::hv_dealloc(tcp); /* no active read (incl. finished): free directly */
}

/* ================= Async file ================= */

int heliosview_file_open(heliosview_loop_t* loop, const char* path, int write_mode,
                         heliosview_file_open_cb on_open, void* userdata)
{
    if (!loop || !path || !on_open)
        return -1;

    auto* task = hv::hv_alloc<open_task>();
    task->kind = OpKind::Open;
    task->loop = loop;
    task->write_mode = write_mode != 0;
    task->cb = on_open;
    task->userdata = userdata;

    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 0) {
        hv::hv_dealloc(task);
        return -1;
    }
    task->path.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, task->path.data(), n);

    if (!PostQueuedCompletionStatus(loop->iocp, 0, reinterpret_cast<ULONG_PTR>(task), nullptr)) {
        hv::hv_dealloc(task);
        return -1;
    }
    return 0;
}

int heliosview_file_read(heliosview_file_t* file, void* buf, uint32_t len, int64_t offset,
                         heliosview_transfer_cb on_read, void* userdata)
{
    if (!file || !buf || !on_read)
        return -1;
    auto* op = hv::hv_alloc<file_io_op>();
    op->kind = OpKind::FileRead;
    op->file = file->handle;
    op->cb = on_read;
    op->userdata = userdata;
    op->ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
    op->ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    const BOOL ok = ReadFile(file->handle, buf, len, nullptr, &op->ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        const int err = -(int)GetLastError();
        hv::hv_dealloc(op);
        return err;
    }
    return 0;
}

int heliosview_file_write(heliosview_file_t* file, const void* buf, uint32_t len, int64_t offset,
                          heliosview_transfer_cb on_write, void* userdata)
{
    if (!file || !buf || !on_write)
        return -1;
    auto* op = hv::hv_alloc<file_io_op>();
    op->kind = OpKind::FileWrite;
    op->file = file->handle;
    op->cb = on_write;
    op->userdata = userdata;
    op->ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
    op->ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    const BOOL ok = WriteFile(file->handle, buf, len, nullptr, &op->ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        const int err = -(int)GetLastError();
        hv::hv_dealloc(op);
        return err;
    }
    return 0;
}

void heliosview_file_close(heliosview_file_t* file)
{
    if (!file)
        return;
    CloseHandle(file->handle);
    hv::hv_dealloc(file);
}
