# Windowed RTT Measurement: Sender and Reflector

This began as an implementation of the client and server for [RFC 8762](https://datatracker.ietf.org/doc/rfc8762/),
but we've made some changes to optimize for use in a cloud environment.

## Client (aka 'sender')

The client sends small, timestamped UDP packets to a server, and calculates timing by reading those from packets returned from the server.

### Windowed sending mode

The client sends a *window* of several packets back-to-back, and then a gap of one second.

### Variable window size and packet length

Both the window size and the packet length are variables/parameters of the sender program. 

### Automatically changing parameters

Window size and/or packet length can increase slowly over time from a starting size to a final size. 

### Statistics collection

Measurements are written into a sqlite database file `/tmp/stamp.db`.

## Server (aka 'reflector')

The reflector receives packets from the sender and sends a udp packet back to the originating address:port containing information
obtained from the received packet including time stamps, and adds some readings it made (such as value of the TTL field).

## To build

Makefiles are in `cmd/reflector` and `cmd-sender`.

## Running tests

You can control the reflector using command line parameters or the environment.

### Reflector example

```shell
./stamp-reflector
```

```
Usage of stampreflector:
  -l string
        listen address:port (default "0.0.0.0:9996")
```

### Sender example

```shell
./stamp-sender -w '50-100' -p '100-200' -d 0 -r 10.0.1.1:9996
```

*Parameters*

```
  -d string
        time duration in seconds (env: DURATION_IN_SECONDS) (default "0")
  -l string
        listen address:port (env: STAMP_CLIENT_ADDR) (default "0.0.0.0:9998")
  -p string
        packet length (can be a range) e.g. 100, 100-200 (env: PACKET_LENGTH) (default "100")
  -r string
        address:port of reflector (env: STAMP_REFLECTOR_ADDR) (default "0.0.0.0:9996")
  -w string
        window size (can be a range) e.g. 100, 100-200 (env: WINDOW_SIZE) (default "100")

```

* `-w` (window size) and `-p` (packet length) can be either a single value, 
or a range in the format of `a-b`.
* `-d` (duration) is a single value that represents number of seconds that the window size and packet length, if a range, 
are changed over. if duration is 0, the window size and packet length will be kept as constants 
and if a range is given, the start value is going to be used.

### Result data file schema

```sqlite
CREATE TABLE rtt (id integer primary key asc, sequence_number integer not null, 
                  window_size integer, packet_length integer, rtt numeric, delta_ttl numeric);
```

### Interpreting the results:

Each packet sent gets "reflected" by the reflector program, which also adds some extra data.
When each reflected packet is received by the sender the following data is calculated and recorded.
So there should be a row in the database for each packet that was sent and then received.

| Column            | Units              | Description                                                                                                                                                                                                  |
|-------------------|--------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `id`              | integer counter    | The unique id for each row.                                                                                                                                                                                  |
| `sequence_number` | integer counter    | The sequence number from reflected packet.                                                                                                                                                                   |
| `window_size`     | integer count      | The number of packets sent in this packet's window.                                                                                                                                                          |
| `packet_length`   | bytes              | The size in bytes of this packet.                                                                                                                                                                            |
| `rtt`             | nanoseconds        | The calculated round-trip time for this packet.                                                                                                                                                              |
| `delta_ttl`       | integer difference | The change in this packet's TTL when received at the reflector. TTL is typically decremented at each router, but this often doesn't happen when the packet is encapsulated (such as in a cloud data center). |
