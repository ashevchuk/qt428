qt428
=====
Streams live H.264 video from a QSee QT428 DVR over its proprietary TCP
protocol (port 6036).  The protocol was reverse-engineered from packet
captures; many structures and fields are still unknown or guesses.

Two output modes are supported:

* **Pipe mode** — raw H.264 is written to stdout and can be piped directly
  into `ffplay` or `mplayer`.
* **HTTP MJPEG server mode** — an embedded HTTP server converts the stream
  to MJPEG on-the-fly via `ffmpeg` and serves it to any number of clients
  (browser, VLC, Home Assistant, etc.).

The code originally derived from [zmodopipe](https://github.com/zmodopipe),
which did not work with firmware 3.2.0+.


Compiling
---------
Requires a C++11 compiler, POSIX threads, and `ffmpeg` on `$PATH` (only
needed for MJPEG server mode).

```
make
```


Usage
-----
```
qt428 [-v] [-u <user>] [-p <pass>] [-c <ch>] [-s <port> [-f <fps>] [-q <quality>]] host[:port]

  -v            Verbose output.
  -u <username> DVR username (default: admin).
  -p <password> DVR password (default: 123456).
  -c <channel>  Channel number (default: 1).
  -s <port>     Start HTTP MJPEG server on <port> (disables stdout stream).
  -f <fps>      MJPEG output framerate (default: 5).
  -q <quality>  JPEG quality 1-31, lower is better (default: 5).
  host[:port]   DVR address; DVR port defaults to 6036.
```

### Pipe mode

```bash
# Single channel to ffplay
qt428 -u admin -p 123456 -c 1 192.168.1.2 | ffplay -

# Multiple channels in parallel
qt428 -u admin -p 123456 -c 1 192.168.1.2 | ffplay - &
qt428 -u admin -p 123456 -c 2 192.168.1.2 | ffplay - &
```

### HTTP MJPEG server mode

```bash
# Channel 1, 10 fps, high quality
qt428 -u admin -p 123456 -c 1 -s 8080 -f 10 -q 3 192.168.1.2
```

Open `http://<host>:8080/` in a browser or any MJPEG-capable client.

To serve all channels simultaneously, run one instance per channel on
different ports:

```bash
qt428 -u admin -p 123456 -c 1 -s 8081 192.168.1.2 &
qt428 -u admin -p 123456 -c 2 -s 8082 192.168.1.2 &
qt428 -u admin -p 123456 -c 3 -s 8083 192.168.1.2 &
qt428 -u admin -p 123456 -c 4 -s 8084 192.168.1.2 &
```


License
-------
Apache 2.0
