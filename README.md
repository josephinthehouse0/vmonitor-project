# vmonitor

A virtual temperature monitoring system consisting of a Linux kernel character
device driver, a non-blocking TCP/UDP server, and a command-line client.

## Architecture

```
        [monitor_cli #1]  [monitor_cli #2]  [any TCP client]
                \                |                  /
                 \               |                 /
                  \——— TCP connections (commands + events) ———/
                                  |
                          [monitor_server]  <——  sole owner  ——>  [/dev/vmonitor]
                                  |                                     |
                          UDP broadcast (127.0.0.1:5001)      [timer, queue, ioctl, sysfs]
                                  |
                        [any UDP listener, e.g. monitor_cli udp]
```

- **`kernel/vmonitor.c`** — a character device driver that generates a
  synthetic temperature reading on a kernel timer, stores samples in a
  64-entry circular queue, and exposes control via `ioctl` (START/STOP/
  GET_STATUS) and `sysfs` (`period_ms`, `threshold_mC`). Only one process
  may have `/dev/vmonitor` open at a time (`-EBUSY` otherwise).
- **`server/monitor_server.c`** — the single process allowed to open
  `/dev/vmonitor`. A single-threaded, `epoll`-based TCP server (no
  thread/fork) that accepts multiple clients, parses a line-based ASCII
  protocol, and relays commands to the driver. It also broadcasts a UDP
  telemetry packet once per second, independent of any TCP client.
- **`cli/monitor_cli.c`** — a command-line client with three modes:
  single command, live event watching, and UDP telemetry listening.

## Building

Requires kernel headers matching your running kernel (`linux-headers-$(uname -r)`)
and standard build tools.

```bash
sudo apt install -y build-essential linux-headers-$(uname -r)

cd kernel  && make && cd ..
cd server  && make && cd ..
cd cli     && make && cd ..
```

## Running

### 1. Load the driver

```bash
cd kernel
sudo insmod vmonitor.ko
ls -l /dev/vmonitor
```

To remove it later: `sudo rmmod vmonitor`.

### 2. Start the server

The server must run as root (it needs to open `/dev/vmonitor` and write to
its `sysfs` attributes).

```bash
cd server
sudo ./monitor_server            # listens on TCP 5000, UDP telemetry on 5001
```

### 3. Use the CLI

```bash
cd cli

./monitor_cli get                 # query current status/counters
./monitor_cli period 500          # set the sample period to 500 ms
./monitor_cli threshold 30000     # set the alarm threshold to 30.0 C
./monitor_cli start               # start automatic sample generation
./monitor_cli inject 42000        # manually inject one 42.0 C sample
./monitor_cli stop                # stop automatic generation

./monitor_cli watch               # live EVT SAMPLE stream, Ctrl+C to stop
./monitor_cli udp                 # live UDP STAT stream, Ctrl+C to stop
```

## Protocol reference

Commands are sent as a single ASCII line: `<id> <COMMAND> [argument]`. The
`id` is chosen by the client and echoed back in the response so replies can
be matched to requests; it carries no other meaning.

| Command | Argument | Effect | Response |
|---|---|---|---|
| `GET` | — | Query full driver status and counters | `RSP <id> OK running=... period_ms=... threshold_mC=... produced_total=... enqueued_total=... dropped_total=... read_total=... queued=... last_seq=... last_value_mC=... last_alarm=...` |
| `START` | — | Start the kernel timer (automatic sample generation) | `RSP <id> OK` |
| `STOP` | — | Stop the kernel timer | `RSP <id> OK` |
| `PERIOD` | `<ms>` (> 0) | Set the sample generation interval | `RSP <id> OK` |
| `THRESHOLD` | `<mC>` | Set the alarm threshold (milli-Celsius) | `RSP <id> OK` |
| `INJECT` | `<mC>` | Manually push one sample onto the queue | `RSP <id> OK` |
| `WATCH` | `0` or `1` | Enable/disable live `EVT SAMPLE` streaming to this connection | `RSP <id> OK` |

Any malformed line or invalid argument returns `RSP <id> ERR <reason>`
instead (using `?` for `<id>` if the line couldn't even be parsed for one).

While `WATCH 1` is active, every new sample produced by the driver (from
the timer or from `INJECT`) is pushed to that connection as:

```
EVT SAMPLE <seq> <timestamp_ns> <value_mC> <alarm>
```

## Example TCP session

```
$ nc 127.0.0.1 5000
1 GET
RSP 1 OK running=0 period_ms=1000 threshold_mC=35000 produced_total=0 enqueued_total=0 dropped_total=0 read_total=0 queued=0 last_seq=0 last_value_mC=0 last_alarm=0
2 PERIOD 500
RSP 2 OK
3 THRESHOLD 30000
RSP 3 OK
4 START
RSP 4 OK
5 WATCH 1
RSP 5 OK
EVT SAMPLE 1 1234567890 10500 0
EVT SAMPLE 2 1234568390 21000 0
EVT SAMPLE 3 1234568890 31500 1
6 STOP
RSP 6 OK
```

## Example UDP telemetry

```
$ python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 5001))
while True:
    data, _ = s.recvfrom(1024)
    print(data.decode().strip())
"
STAT 0 3 31500 1 0 1
STAT 1 5 40000 1 0 1
STAT 2 7 20000 0 0 1
```

Format: `STAT <udp_seq> <last_seq> <last_value_mC> <last_alarm> <dropped_total> <running>`,
sent every second regardless of TCP client activity.

## Testing

See [`tests/TEST_REPORT.md`](tests/TEST_REPORT.md) for the negative-test
results, and `tests/test_negative.py` to re-run them against a live server.

```bash
sudo ./server/monitor_server &
python3 tests/test_negative.py
```

## Design notes

- **Single-open driver lock**: only one process may hold `/dev/vmonitor`
  open, enforced with `atomic_cmpxchg`. This is why the server must be the
  sole owner; all other consumers go through it over the network.
- **Queue-full policy**: the driver's 64-entry circular queue drops the
  *newest* sample when full rather than overwriting unread data;
  `dropped_total` tracks how many were lost this way.
- **No threads or forking anywhere in the server**: a single `epoll` loop
  multiplexes the listening socket, `/dev/vmonitor` (via the driver's
  `poll()`/wait-queue support), a 1-second `timerfd` for UDP telemetry,
  and all connected clients.
- **Per-client buffering**: an `rx_buf` reassembles command lines that
  arrive fragmented across multiple TCP reads; a 64 KiB `tx_buf` queues
  outbound data when a socket can't accept it immediately, and a client
  that never drains it (falls too far behind) is disconnected.