#include "QT428.hpp"
#include "MJPEGServer.hpp"
#include "Utils.hpp"
#include "TCPSocket.hpp"
#include "Logger.hpp"

#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

#include <memory>

static void usage()
{
    std::cerr << "Usage:" << std::endl;
    std::cerr << "    qt428 [-v] [-u <user>] [-p <pass>] [-c <ch>] [-s <port> [-f <fps>] [-q <quality>]] host[:port]" << std::endl;
    std::cerr << std::endl;
    std::cerr << "    -v            Verbose output." << std::endl;
    std::cerr << "    -u <username> Username (default: admin)." << std::endl;
    std::cerr << "    -p <password> Password (default: 123456)." << std::endl;
    std::cerr << "    -c <channel>  Channel number (default: 1)." << std::endl;
    std::cerr << "    -s <port>     HTTP MJPEG server port (disables stdout stream)." << std::endl;
    std::cerr << "    -f <fps>      MJPEG output framerate (default: 5)." << std::endl;
    std::cerr << "    -q <quality>  JPEG quality 1-31, lower=better (default: 5)." << std::endl;
    std::cerr << "    host[:port]   DVR address, DVR port defaults to 6036." << std::endl;
    std::cerr << std::endl;
    std::cerr << "Examples:" << std::endl;
    std::cerr << "  qt428 -u admin -p 123456 -c 1 192.168.1.2 | ffplay -" << std::endl;
    std::cerr << "  qt428 -u admin -p 123456 -c 1 -s 8080 -f 10 -q 3 192.168.1.2" << std::endl;
    std::cerr << std::endl;
}

int main(int argc, char *argv[])
{
    std::string user     = "admin";
    std::string password = "123456";
    std::string hostname;
    unsigned int port    = 6036;
    unsigned int channel = 1;
    int logLevel  = 0;
    int serverPort = -1;
    int fps       = 5;
    int quality   = 5;
    int opt;

    signal(SIGPIPE, SIG_IGN);

    while ((opt = getopt(argc, argv, "vu:p:c:s:f:q:")) != -1) {
        switch (opt) {
            case 'u': user.assign(optarg);         break;
            case 'p': password.assign(optarg);     break;
            case 'c': channel    = atoi(optarg);   break;
            case 'v': logLevel++;                  break;
            case 's': serverPort = atoi(optarg);   break;
            case 'f': fps        = atoi(optarg);   break;
            case 'q': quality    = atoi(optarg);   break;
            case 'h':
            case '?':
            default:
                usage();
                return -1;
        }
    }

    if (optind >= argc) { usage(); return -1; }

    hostname.assign(argv[optind]);
    int sepIndex = hostname.find(':');
    if (sepIndex != -1) {
        port = atoi(hostname.substr(sepIndex + 1).c_str());
        hostname.resize(sepIndex);
    }

    Logger logger;
    logger.setLevel(logLevel);
    TCPSocket tcpSocket(logger);
    QT428 qt428(logger, tcpSocket, user, password, channel, std::cout);

    std::unique_ptr<MJPEGServer> server;
    if (serverPort > 0) {
        server = std::make_unique<MJPEGServer>(serverPort, fps, quality);
        if (!server->start()) {
            fprintf(stderr, "Failed to start MJPEG server on port %d\n", serverPort);
            return -1;
        }
        fprintf(stderr, "MJPEG server listening on http://0.0.0.0:%d/  (fps=%d quality=%d)\n",
                serverPort, fps, quality);
        qt428.setVideoCallback([&server](const uint8_t* data, size_t len) {
            server->writeH264(data, len);
        });
    }

    if (!tcpSocket.connect(hostname, port)) {
        logger.error(format("Unable to connect to [%s:%d]\n", hostname, port));
        return -1;
    }

    while (qt428.process());

    return 0;
}
