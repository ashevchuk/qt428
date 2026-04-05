#include "MJPEGServer.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include <string>
#include <chrono>
#include <algorithm>

static const char BOUNDARY[] = "mjpegframe";

MJPEGServer::MJPEGServer(int port, int fps, int quality)
    : _port(port), _fps(fps), _quality(quality) {}

MJPEGServer::~MJPEGServer()
{
    stop();
}

bool MJPEGServer::spawnFFmpeg()
{
    if (pipe(_toFFmpeg) < 0 || pipe(_fromFFmpeg) < 0) {
        perror("pipe");
        return false;
    }

    _ffmpegPid = fork();
    if (_ffmpegPid < 0) {
        perror("fork");
        return false;
    }

    if (_ffmpegPid == 0) {
        // Child: wire pipes to stdin/stdout
        dup2(_toFFmpeg[0],   STDIN_FILENO);
        dup2(_fromFFmpeg[1], STDOUT_FILENO);

        close(_toFFmpeg[0]);  close(_toFFmpeg[1]);
        close(_fromFFmpeg[0]); close(_fromFFmpeg[1]);

        // Suppress ffmpeg chatter
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        std::string vf  = "fps=" + std::to_string(_fps);
        std::string qv  = std::to_string(_quality);

        execlp("ffmpeg", "ffmpeg",
               "-f", "h264", "-i", "pipe:0",
               "-vf", vf.c_str(),
               "-q:v", qv.c_str(),
               "-f", "mjpeg", "pipe:1",
               (char*)nullptr);

        perror("execlp ffmpeg");
        _exit(1);
    }

    // Parent: close child-side ends
    close(_toFFmpeg[0]);   _toFFmpeg[0]   = -1;
    close(_fromFFmpeg[1]); _fromFFmpeg[1] = -1;
    return true;
}

bool MJPEGServer::start()
{
    _serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (_serverFd < 0) { perror("socket"); return false; }

    // Don't leak the server socket into the ffmpeg child
    fcntl(_serverFd, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(_port);

    if (bind(_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(_serverFd); _serverFd = -1; return false;
    }
    if (listen(_serverFd, 10) < 0) {
        perror("listen"); close(_serverFd); _serverFd = -1; return false;
    }

    if (!spawnFFmpeg()) {
        close(_serverFd); _serverFd = -1; return false;
    }

    _running = true;
    _acceptThread = std::thread(&MJPEGServer::acceptLoop,     this);
    _ffmpegThread = std::thread(&MJPEGServer::ffmpegReadLoop, this);
    return true;
}

void MJPEGServer::stop()
{
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) return;

    _frameCv.notify_all();

    if (_ffmpegPid > 0) kill(_ffmpegPid, SIGTERM);

    if (_toFFmpeg[1]   >= 0) { close(_toFFmpeg[1]);   _toFFmpeg[1]   = -1; }
    if (_fromFFmpeg[0] >= 0) { close(_fromFFmpeg[0]); _fromFFmpeg[0] = -1; }
    if (_serverFd      >= 0) { close(_serverFd);      _serverFd      = -1; }

    if (_ffmpegThread.joinable()) _ffmpegThread.join();
    if (_acceptThread.joinable()) _acceptThread.join();

    if (_ffmpegPid > 0) { waitpid(_ffmpegPid, nullptr, 0); _ffmpegPid = -1; }
}

void MJPEGServer::writeH264(const uint8_t* data, size_t len)
{
    if (!_running || _toFFmpeg[1] < 0) return;
    while (len > 0) {
        ssize_t n = write(_toFFmpeg[1], data, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        data += n;
        len  -= n;
    }
}

// ---------------------------------------------------------------------------
// Read JPEG frames from ffmpeg's stdout and push them to waiting clients.
// ffmpeg -f mjpeg outputs a raw stream of JPEG files: each starts with
// FF D8 and ends with FF D9 (no byte-stuffing around EOI, so it's safe
// to use as a delimiter).
// ---------------------------------------------------------------------------
void MJPEGServer::ffmpegReadLoop()
{
    std::vector<uint8_t> buf;
    buf.reserve(1 << 20);
    uint8_t tmp[65536];

    while (_running) {
        ssize_t n = read(_fromFFmpeg[0], tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break; // ffmpeg closed stdout

        buf.insert(buf.end(), tmp, tmp + n);

        // Guard against runaway buffers
        if (buf.size() > 8 * 1024 * 1024) buf.clear();

        // Extract all complete JPEG frames from buf
        while (true) {
            // Find SOI (FF D8)
            size_t soi = buf.size();
            for (size_t i = 0; i + 1 < buf.size(); ++i) {
                if (buf[i] == 0xFF && buf[i+1] == 0xD8) { soi = i; break; }
            }
            if (soi > 0) buf.erase(buf.begin(), buf.begin() + soi);
            if (buf.size() < 4 || buf[0] != 0xFF || buf[1] != 0xD8) break;

            // Find EOI (FF D9) — safe because JPEG stuffs FF→FF 00 in data
            size_t eoi = buf.size();
            for (size_t i = 2; i + 1 < buf.size(); ++i) {
                if (buf[i] == 0xFF && buf[i+1] == 0xD9) { eoi = i + 1; break; }
            }
            if (eoi == buf.size()) break; // frame not complete yet

            {
                std::lock_guard<std::mutex> lk(_frameMutex);
                _latestFrame.assign(buf.begin(), buf.begin() + eoi + 1);
                ++_frameSeq;
            }
            _frameCv.notify_all();
            buf.erase(buf.begin(), buf.begin() + eoi + 1);
        }
    }
}

void MJPEGServer::acceptLoop()
{
    while (_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(_serverFd, &fds);
        struct timeval tv = {1, 0};

        int rc = select(_serverFd + 1, &fds, nullptr, nullptr, &tv);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (rc == 0) continue;

        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int clientFd = accept(_serverFd, (struct sockaddr*)&addr, &len);
        if (clientFd < 0) continue;

        std::thread(&MJPEGServer::clientLoop, this, clientFd).detach();
    }
}

void MJPEGServer::clientLoop(int fd)
{
    // Drain the HTTP request (we don't care about the URL)
    char req[4096];
    recv(fd, req, sizeof(req) - 1, 0);

    std::string hdr =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=" +
        std::string(BOUNDARY) + "\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (send(fd, hdr.c_str(), hdr.size(), MSG_NOSIGNAL) < 0) {
        close(fd); return;
    }

    uint64_t lastSeq = 0;

    while (_running) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(_frameMutex);
            bool got = _frameCv.wait_for(lk, std::chrono::seconds(5),
                [&]{ return _frameSeq != lastSeq || !_running; });
            if (!_running) break;
            if (!got || _frameSeq == lastSeq) continue;
            frame   = _latestFrame;
            lastSeq = _frameSeq;
        }

        std::string part =
            "--" + std::string(BOUNDARY) + "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: " + std::to_string(frame.size()) + "\r\n"
            "\r\n";

        if (send(fd, part.c_str(),  part.size(),  MSG_NOSIGNAL) < 0) break;
        if (send(fd, frame.data(),  frame.size(),  MSG_NOSIGNAL) < 0) break;
        if (send(fd, "\r\n", 2,                    MSG_NOSIGNAL) < 0) break;
    }

    close(fd);
}
