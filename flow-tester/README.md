
# Flow sender/receiver

These programs send video rate flowlets and receive them, collecting statistics.

The sender creates a sequence of video sized packets and sends them to the receiver using
a specified number of flows. The current code will choose the
flow in a round-robin manner to make it easier to debug certain
problems that may be harder to detect if the flows were assigned
randomly.

Packets contain two sequence numbers:

* An overall sequence number corresponding to the packet's order in the video stream, and
* A flow-specific sequence number that gets incremented for each packet on each specific flow.

The overall sequence number is used for placement in a receiver
reordering buffer, and subsequently for detection of packet loss and duplicated packets.

The flow-specific sequence numbers are used to detect discontinuities on each flow including packets being
received out of order.

## Interpreting results

The measurements are made after the packets have passed through two user mode programs, two linux networking stacks, and the associated network. Each part of this chain can add some errors, so don't just blame the network.


## Our results on various clouds

Port 9 Labs will periodically publish measurements made on several public clouds using this software. Look for posts on our [blog](http://blog.port9labs.com) tagged [testing](http://blog.port9labs.com/category/testing).

## Build Prerequisites

To install dependencies on a Mac using homebrew:

`brew install spdlog boost pkg-config libuv`

### To build


```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
```

This should create `flow_sender` and `flow_receiver` binaries. Note that it is very important to use a release build to ensure the code runs quickly enough to handle the high packet rate.

## To run

We suggest running the receiver first and then running the sender within a second or so to work around a startup bug we need to fix.

### Receiver

```
Allowed options:
  --help                produce help message
  --port arg (=5678)    listen port
  --flowlets arg (=1)   number of flowlets
```

Typical command line:

`./flow_receiver --port 5678 --flowlets 8`


The receiver will periodically print per stream and per-flow statistics to stdout, and will store readings in a sqlite3 datbase file at `/tmp/cloudnet.db`.

#### Sender

```
Allowed options:
  --help                      produce help message
  --dst arg (=127.0.0.0:5678) destination address:port
  --flowlets arg (=1)         number of flowlets
  --plen arg (=8100)          payload length (suggest 1400 on azure)
  --fmt arg (=422)            video format: 422, 444, 4444
  --bpf arg                   bytes per frame (overrides --fmt setting)
  --rate arg (=60)            frame rate in Hz
```

The sender will periodically print some statistics to stdout, but the important measurements are on the receiver side.

**Payload Length and MTU**

Note that the default payload length of 8100 bytes creates packets
of size 8160, which requires jumbo frames.

We suggest you run a tool such as `tracepath` to verify the path MTU
between sender and receiver, and ensure that 

`payload_length <= (PATH_MTU - 80)`.


**Format Definitions**

As a convenience, instead of specifying a particular number of
bytes per frame, the formats we typically use are provided (1080p59.94):

| format | bytes per frame | description             |
|--------|-----------------|-------------------------|
| 422    | 5,184,000       | ST 2210-20 10 bit 4:2:2 |
| 444    | 12,441,600      | 16 bit component RGB    |
| 4444   | 16,588,800      | 16 bit component RGBA   |


Typical command line:

`./flow_sender --dst 123.45.67.8:5678 --fmt 444 --flowlets 8`

## Data collection

The receiver will write statistics into a sqlite file at `/tmp/cloudnet.db`.

### schema

Time stamps are stored as unix epoch nanoseconds.

```sqlite
CREATE TABLE drops
(
    x INTEGER PRIMARY KEY ASC,
    packets_dropped NUMERIC,
    packets_total   NUMERIC,
    duplicates      NUMERIC,
    media_rate     REAL,
    timestamp        NUMERIC
);
```

Contains overall stream statistics. `media_rate` is in Gbits/s.

```sqlite
CREATE TABLE bursts
(
    x INTEGER PRIMARY KEY ASC,
    port             INTEGER,
    packets_received NUMERIC,
    bytes_received   NUMERIC,
    burst_errors     INTEGER,
    burst_length     INTEGER,
    timestamp        NUMERIC
);
```

Contains per-flowlet-burst information. Each time a burst error is detected on a flow it will be recorded here. Note that each flowlet is identified by port number.

```
CREATE TABLE sbursts
(
    x INTEGER PRIMARY KEY ASC,
    burst_length     INTEGER,
    timestamp        NUMERIC
);
```

Contains media stream burst information. Each time a burst error is detected after reordering it will be recorded here.

```
CREATE TABLE flows
(
    x INTEGER PRIMARY KEY ASC,
    port     INTEGER,
    burst_count     INTEGER,
    reverses        NUMERIC,
    duplicates      NUMERIC,
    longest_burst   NUMERIC,
    sequence_breaks NUMERIC,
    timestamp        NUMERIC
);
```

Contains per-flowlet statistics. Note that each flowlet is identified by port number.

