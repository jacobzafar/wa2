# Assignment 2 - TCP and UDP calculation servers

Two servers that speak the same calculation protocol as assignment 1 and
serve TEXT and BINARY clients on one port.

* `tcpserver` - one `fork()` per connection.
* `udpserver` - single process, `poll()` on the socket(s).

## Build

```
make
```

Produces `tcpserver` and `udpserver`. `make clean` removes the build output.

## Run

```
./tcpserver <host>:<port>
./udpserver <host>:<port>
```

`<host>` is an IPv4 dotted address or a DNS name (which may resolve to
IPv4 and/or IPv6); the server binds every address it resolves to.

## Protocol notes

### TCP

The server sends the supported protocols on connect:

```
TEXT TCP 1.1
BINARY TCP 1.1

```

The client answers with `TEXT TCP 1.1 OK` or `BINARY TCP 1.1 OK`.

* TEXT: server sends `<op> <v1> <v2>`, client sends the result, server
  replies `OK` or `ERROR`.
* BINARY: server sends a `calcProtocol` (version 1.1), client returns it
  with `inResult` filled, server replies with a `calcMessage`
  (`message` 1 = OK, 2 = NOT OK).

Every operation must finish within 5 seconds. On expiry the server sends
`ERROR TO`, closes the socket and the child exits.

### UDP

The client sends first. `TEXT UDP 1.1` starts a text exchange; a
`calcMessage` (type 21-23, version 1.1) starts a binary exchange. The
server stores the client, sends the task, and validates the answer in the
follow-up datagram. A client that does not answer within 10 seconds is
removed and a later answer is rejected.

Division tasks always use a non-zero divisor.
