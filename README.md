UDP Samplicator
===============

This small program receives UDP datagrams on a given port, and resends
those datagrams to a specified set of receivers.  In addition, a
sampling divisor N may be specified individually for each receiver,
which will then only receive one in N of the received packets.

DOWNLOADING
-----------

This package is distributed under
	https://github.com/sleinen/samplicator/

INSTALLATION
------------

See [INSTALL.md](INSTALL.md).

AUTHORS
-------

See the `AUTHORS` file.

USAGE
-----

The usage convention for the program is

	$ samplicate [<option>...] [<destination>...]

Where each `<option>` can be one of

	-d <level>	to set the debugging level
	-s <address>	to set interface address on which to listen
			for incoming packets (default any)
	-p <port>	to set the UDP port on which to listen for
			incoming packets (default 2000)
	-b <buflen>	size of receive buffer (default 65536)
	-c <configfile>	specify a config file to read
	-x <delay>	to specify a transmission delay after each packet,
		    in units of	microseconds
	-S		maintain (spoof) source addresses
	-n		don't compute UDP checksum (only relevant with -S)
	-f		fork program into background
	-m <pidfile>	write the process ID to a file
	-4		IPv4 only
	-6		IPv6 only
	-h		to print a usage message and exit
	-u <pdulen>	size of max pdu on listened socket (default 65536)

and each `<destination>` should be specified as
`<addr>[/<port>[/<interval>[,ttl]]]`, where

	<addr>		IP address of the receiver
	<port>		port UDP number of the receiver (default 2000)
	<freq>		number of received datagrams between successive
			copied datagrams for this receiver.
	<ttl>		The TTL (IPv4) or hop-limit (IPv6) for
			outgoing datagrams.

Config file format:

    a.b.c.d[/e.f.g.h]: receiver ...

where:

	a.b.c.d     is the sender's IP address
    e.f.g.h     is a mask to apply to the sender (default 255.255.255.255)
    receiver    see above.

Receivers specified on the command line will get all packets, those
specified in the config-file will get only packets with a matching
source.


AT SCALE
---------

When running samplicator at scale you may discover excessive UDP packet loss.
To combat data loss and enable higher throughput the following features were
added:

- multi threaded receive
- port sharding (each thread shares the IP/recv port)
- asynchronous transmission using transient threads
- buffering of transmit data

Multi-threaded receiving can be enabled using the `-w N` option where N
is the number of worker threads. Multi-threaded receiving may be used with
both synchronous (default) and asynchronous transmission depening on
throughput requirements. With synchronous transmission recv threads will block
until transmission compeltes.

Asynchronous transmission can be enabled using the `-a` option. When coupled
with the `-w-` option for multi-threaded recieve this provides maximum
throughput. When transmitting data asynchornously incoming data is buffered
until the threshold set with `-f N` (default 32Kb) is reached. Once this
threshold is met a seperate transmit thread will be spun up and detached
from the receive thread.
