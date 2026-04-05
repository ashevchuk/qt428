#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <sys/types.h>

class MJPEGServer {
public:
    MJPEGServer(int port, int fps, int quality);
    ~MJPEGServer();

    bool start();
    void stop();
    void writeH264(const uint8_t* data, size_t len);

private:
    bool spawnFFmpeg();
    void acceptLoop();
    void clientLoop(int clientFd);
    void ffmpegReadLoop();

    int _port;
    int _fps;
    int _quality;
    int _serverFd = -1;
    int _toFFmpeg[2]   = {-1, -1};
    int _fromFFmpeg[2] = {-1, -1};
    pid_t _ffmpegPid = -1;

    std::atomic<bool> _running{false};
    std::thread _acceptThread;
    std::thread _ffmpegThread;

    std::mutex _frameMutex;
    std::condition_variable _frameCv;
    std::vector<uint8_t> _latestFrame;
    uint64_t _frameSeq = 0;
};
