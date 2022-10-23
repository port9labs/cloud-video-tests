package main

/*
Copyright (c) 2022 Port 9 Labs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
import (
	"encoding/binary"
	"flag"
	"log"
	"net"
	"os"
	"time"

	"golang.org/x/net/ipv4"
	_ "golang.org/x/net/ipv4"
)

type StampReflector struct {
	conn      *ipv4.PacketConn
	gotSender bool
}

func (c *StampReflector) now() time.Time {
	return time.Now()
}

/*  7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                        sequence number                        | <- idx = 0
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                           timestamp                           | <- idx = 4
* |                                                               |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                      receive  timestamp                       | <- idx = 12
* |                                                               |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                     sender sequence number                    | <- idx = 20
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                       sender timestamp                        | <- idx = 24
* |                                                               |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                      sender window size                       | <- idx = 32
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                      sender packet size                       | <- idx = 36
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |     TTL       |                (padding zeros)                | <- idx = 40
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

func (c *StampReflector) receiver() {
	log.Printf("receiving on %+v", c.conn.LocalAddr())
	packet := make([]byte, 10000)
	err := c.conn.SetControlMessage(ipv4.FlagTTL, true)
	if err != nil {
		log.Printf("error setting control message: %+v", err)
	}
	srcMap := make(map[string]uint32)
	for {
		ttl := uint8(0)
		//c.conn.SetReadDeadline(time.Now().Add(time.Second * 10))
		n, cm, src, err := c.conn.ReadFrom(packet)
		if err != nil {
			log.Print(err)
		} else {
			if cm != nil {
				ttl = uint8(cm.TTL)
			}
			//log.Print(string(packet[:n]))
			if !c.gotSender {
				c.gotSender = true
				log.Printf("got first packet from %s", src)
			}
			count := srcMap[src.String()]
			srcMap[src.String()] = count + 1
			if n < 16 {
				log.Printf("unexpected received packet size %d: expected larger than 16", n)
				continue
			}
			//log.Printf("from %+v, ttl %d, count %d", src, ttl, count)
			senderSequenceNumber := binary.BigEndian.Uint32(packet[0:])
			senderTimestamp := binary.BigEndian.Uint64(packet[4:])
			senderWindowSize := binary.BigEndian.Uint32(packet[12:])

			myTimestamp := uint64(time.Now().UnixNano())
			//timeDiff := myTimestamp - senderTimestamp

			//log.Printf("their time delta from now is %+v", timeDiff)

			idx := 0
			binary.BigEndian.PutUint32(packet[idx:], count) // Sequence Number
			idx += 4
			binary.BigEndian.PutUint64(packet[idx:], myTimestamp) // Timestamp
			idx += 8
			binary.BigEndian.PutUint64(packet[idx:], myTimestamp) // Receive Timestamp
			idx += 8
			binary.BigEndian.PutUint32(packet[idx:], senderSequenceNumber)
			idx += 4
			binary.BigEndian.PutUint64(packet[idx:], senderTimestamp)
			idx += 8
			binary.BigEndian.PutUint32(packet[idx:], senderWindowSize)
			idx += 4
			binary.BigEndian.PutUint32(packet[idx:], uint32(n)) // sender packet size
			idx += 4
			binary.BigEndian.PutUint32(packet[idx:], 0)
			packet[idx] = ttl
			idx += 4
			_, err = c.conn.WriteTo(packet[:idx], nil, src) // reflector packet is not necessarily the same size as sender packet.
			if err != nil {
				log.Print("write error: ", err)
			} else {
				//log.Print("wrote ", sent, " bytes")
			}
		}
	}
}

func newClient(listenAddr string) (StampReflector, error) {
	uconn, err := net.ListenPacket("udp4", listenAddr)
	if err != nil {
		log.Fatal("error in listenpacket:", err)
	}
	conn := ipv4.NewPacketConn(uconn)
	return StampReflector{
		conn: conn,
	}, nil
}

func main() {
	log.Print(VersionString())
	fs := flag.NewFlagSet("stampreflector", flag.ExitOnError)
	defaultListenAddr := "0.0.0.0:9996"
	e, ok := os.LookupEnv("STAMP_REFLECTOR_ADDR")
	if ok {
		defaultListenAddr = e
	}
	listenAddrArg := fs.String("l", defaultListenAddr, "listen address:port")
	_ = fs.Parse(os.Args[1:])
	client, err := newClient(*listenAddrArg)
	if err != nil {
		log.Fatal("could not create client: ", err)
	}
	client.receiver()
}
