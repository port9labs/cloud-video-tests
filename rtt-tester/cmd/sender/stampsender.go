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
	"database/sql"
	"encoding/binary"
	"flag"
	"fmt"
	_ "github.com/mattn/go-sqlite3"
	"golang.org/x/net/ipv4"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	MaxPacketLen = 10000
	SenderTTL    = 123
)

type VarParam struct {
	start   int
	end     int
	current int
}

func (vp VarParam) String() string {
	if vp.end != vp.start {
		return fmt.Sprintf("%d-%d", vp.start, vp.end)
	}
	return fmt.Sprintf("%d", vp.start)
}

type Report struct {
	SequenceNumber int
	Dropped        bool
	WindowSize     int
	PacketLength   int
	MeasuredRTT    int64
	TTL            int64
}

type StampClient struct {
	conn          *ipv4.PacketConn
	reflectorAddr *net.UDPAddr
	nextSendSeqNo uint32
	packet        []byte
	windowSize    VarParam
	packetLen     VarParam
	lastRecvSeqNo uint32
	dbChan        chan Report
	duration      int64
	received      bool
}

func newClient(listenAddr, reflectorAddrStr string, windowSize, pktLen VarParam, duration int) (StampClient, error) {
	reflectorAddr, err := net.ResolveUDPAddr("udp4", reflectorAddrStr)
	if err != nil {
		log.Fatal("error resolving reflector address: ", err)
	}
	uconn, err := net.ListenPacket("udp4", listenAddr)
	if err != nil {
		log.Fatal("error in listenpacket:", err)
	}
	//defer uconn.Close()
	conn := ipv4.NewPacketConn(uconn)
	err = conn.SetTTL(SenderTTL)
	if err != nil {
		log.Fatal("error in SetTTL:", err)
	}
	return StampClient{
		conn:          conn,
		reflectorAddr: reflectorAddr,
		nextSendSeqNo: uint32(0),
		dbChan:        make(chan Report, 100),
		packet:        make([]byte, MaxPacketLen),
		windowSize:    windowSize,
		packetLen:     pktLen,
		duration:      (time.Duration(duration) * time.Second).Nanoseconds(),
		received:      false,
	}, nil
}

/*  7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0 7 6 5 4 3 2 1 0
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                        sequence number                        | <- idx = 0
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                           timestamp                           | <- idx = 4
* |                                                               |
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
* |                          window size                          | <- idx = 12
* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

// send runs a loop that sends current window size of packets and then sleeps for 1 second before sending again
func (c *StampClient) send(durationElapsed chan bool) {
	start := time.Now().UnixNano()
	for {
		now := time.Now().UnixNano()
		if c.duration != 0 {
			percent := float64(now-start) / float64(c.duration)
			if percent >= 1 {
				// finish when the duration has elapsed
				if c.windowSize.current == c.windowSize.end && c.packetLen.current == c.packetLen.end {
					durationElapsed <- true
					return
				} else {
					c.windowSize.current = c.windowSize.end
					c.packetLen.current = c.packetLen.end
				}
			} else {
				if c.windowSize.current != c.windowSize.end {
					c.windowSize.current = c.windowSize.start + int(float64(c.windowSize.end-c.windowSize.start)*percent)
				}
				if c.packetLen.current != c.packetLen.end {
					c.packetLen.current = c.packetLen.start + int(float64(c.packetLen.end-c.packetLen.start)*percent)
				}
			}
		}
		c.sendPacketWindow(c.windowSize.current, c.packetLen.current)
		time.Sleep(1 * time.Second)
	}
}

// sendPacketWindow sends n packets of size m (n = numPackets, m = packetLen) to the reflector.
// Each packet has the current time as the timestamp
// and an incremented sequence number from the previous packet sequence number.
func (c *StampClient) sendPacketWindow(numPackets int, packetLen int) {
	for i := 0; i < numPackets; i++ {
		// timestamp
		timestamp := time.Now().UnixNano()
		// send packet
		idx := 0
		binary.BigEndian.PutUint32(c.packet[idx:], c.nextSendSeqNo)
		c.nextSendSeqNo += 1
		idx += 4
		binary.BigEndian.PutUint64(c.packet[idx:], uint64(timestamp))
		idx += 8
		binary.BigEndian.PutUint32(c.packet[idx:], uint32(c.windowSize.current))

		_, err := c.conn.WriteTo(c.packet[:packetLen], nil, c.reflectorAddr)
		if err != nil {
			log.Print("write error: ", err)
		} else {
			//log.Print("wrote ", len, " bytes")
		}
	}
}

func (c *StampClient) reporter(dbPath string, done chan bool) {
	os.Remove(dbPath)
	db, err := sql.Open("sqlite3", dbPath)
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()

	sqlStmt := `
	create table rtt (id integer primary key asc, sequence_number integer not null, window_size integer, packet_length integer, rtt numeric, delta_ttl numeric);
	delete from rtt;
	`
	_, err = db.Exec(sqlStmt)
	if err != nil {
		log.Printf("%q: %s\n", err, sqlStmt)
		return
	}
	stmt, err := db.Prepare("insert into rtt(sequence_number, window_size, packet_length, rtt, delta_ttl) values(?, ?, ?, ?, ?)")
	if err != nil {
		log.Fatal(err)
	}
	defer stmt.Close()
	for {
		select {
		case <-done:
			log.Printf("reporter received done signal\n")
			return
		case r := <-c.dbChan:
			if r.Dropped {
				log.Printf("seq %d was dropped", r.SequenceNumber)
				_, err = stmt.Exec(r.SequenceNumber, sql.NullInt32{}, sql.NullInt32{}, sql.NullInt64{}, sql.NullInt64{})
				if err != nil {
					log.Fatal(err)
				}
			} else {
				_, err = stmt.Exec(r.SequenceNumber, r.WindowSize, r.PacketLength, r.MeasuredRTT, r.TTL)
				if err != nil {
					log.Fatal(err)
				}
			}
		}
	}

}

func (c *StampClient) receiver() {
	//log.Printf("receiving on %+v", c.conn.LocalAddr())
	packet := make([]byte, 10000)
	err := c.conn.SetControlMessage(ipv4.FlagTTL, true)
	if err != nil {
		log.Printf("error setting control message: %+v", err)
	}
	c.sendPacketWindow(c.windowSize.current, c.packetLen.current)
	for {
		//ttl := uint8(0)
		//c.conn.SetReadDeadline(time.Now().Add(time.Second * 10))
		n, _, src, err := c.conn.ReadFrom(packet)
		if err != nil {
			log.Print("read error: ", err)
		} else {
			receiveTime := time.Now().UnixNano()
			if n != 44 { // reflector packet size = 44
				log.Printf("bad packet length %d: expected 44 bytes", n)
			}
			if !c.received {
				c.received = true
				log.Printf("received first packet from %s", src)
			}
			//if cm != nil {
			//	ttl = uint8(cm.TTL)
			//}
			idx := 0
			//reflectorSequenceNumber := binary.BigEndian.Uint32(packet[idx:])
			idx += 4
			idx += 8 // skip timestamp
			//reflectorTimestamp := binary.BigEndian.Uint64(packet[idx:])
			idx += 8
			myPacketSequenceNumber := binary.BigEndian.Uint32(packet[idx:])
			idx += 4
			myPacketTimestamp := binary.BigEndian.Uint64(packet[idx:])
			idx += 8
			myWindowSize := binary.BigEndian.Uint32(packet[idx:])
			idx += 4
			myPacketLen := binary.BigEndian.Uint32(packet[idx:])
			idx += 4
			myPacketTTL := packet[idx]
			rtt := uint64(receiveTime) - myPacketTimestamp

			for i := 0; i < int(myPacketSequenceNumber-c.lastRecvSeqNo)-1; i++ {
				report := Report{
					SequenceNumber: int(c.lastRecvSeqNo + 1),
					Dropped:        true,
				}
				c.dbChan <- report
			}
			// received packet
			report := Report{
				SequenceNumber: int(myPacketSequenceNumber),
				Dropped:        false,
				WindowSize:     int(myWindowSize),
				PacketLength:   int(myPacketLen),
				MeasuredRTT:    int64(rtt),
				TTL:            int64(myPacketTTL - SenderTTL),
			}
			c.dbChan <- report
			c.lastRecvSeqNo = myPacketSequenceNumber
		}
	}
}

func main() {
	log.Print(VersionString())
	fs := flag.NewFlagSet("stampsender", flag.ExitOnError)
	defaultReflectorAddr := "127.0.0.1:9996"
	e, ok := os.LookupEnv("STAMP_REFLECTOR_ADDR")
	if ok {
		defaultReflectorAddr = e
	}
	defaultListenAddr := "0.0.0.0:9998"
	e, ok = os.LookupEnv("STAMP_CLIENT_ADDR")
	if ok {
		defaultListenAddr = e
	}
	defaultWindowSize := "100"
	e, ok = os.LookupEnv("WINDOW_SIZE")
	if ok {
		defaultWindowSize = e
	}
	defaultPktLen := "100"
	e, ok = os.LookupEnv("PACKET_LENGTH")
	if ok {
		defaultPktLen = e
	}
	defaultDuration := "0"
	e, ok = os.LookupEnv("DURATION_IN_SECONDS")
	if ok {
		defaultDuration = e
	}

	reflectorAddrArg := fs.String("r", defaultReflectorAddr, "address:port of reflector (env: STAMP_REFLECTOR_ADDR)")
	listenAddrArg := fs.String("l", defaultListenAddr, "listen address:port (env: STAMP_CLIENT_ADDR)")
	windowSizeArg := fs.String("w", defaultWindowSize, "window size (can be a range) e.g. 100, 100-200 (env: WINDOW_SIZE)")
	pktLenArg := fs.String("p", defaultPktLen, "packet length (can be a range) e.g. 100, 100-200 (env: PACKET_LENGTH)")
	durationArg := fs.String("d", defaultDuration, "time duration in seconds (env: DURATION_IN_SECONDS)")

	_ = fs.Parse(os.Args[1:])
	duration, err := strconv.Atoi(*durationArg)
	if err != nil {
		log.Fatal(fmt.Sprintf("error parsing packet length: %s\n", *pktLenArg))
	}
	// window size
	windowSize := VarParam{}
	if strings.Contains(*windowSizeArg, "-") == true {
		sizes := strings.Split(*windowSizeArg, "-")
		if len(sizes) > 2 {
			log.Fatal("error parsing window size")
		}
		windowSize.start, err = strconv.Atoi(sizes[0])
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing window size: %s\n", *windowSizeArg))
		}
		windowSize.end, err = strconv.Atoi(sizes[1])
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing window size: %s\n", *windowSizeArg))
		}
	} else {
		windowSize.start, err = strconv.Atoi(*windowSizeArg)
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing window size: %s\n", *windowSizeArg))
		}
		windowSize.end = windowSize.start
	}
	windowSize.current = windowSize.start
	// packet length
	pktLen := VarParam{}
	if strings.Contains(*pktLenArg, "-") == true {
		sizes := strings.Split(*pktLenArg, "-")
		if len(sizes) > 2 {
			log.Fatal("error parsing packet length")
		}
		pktLen.start, err = strconv.Atoi(sizes[0])
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing packet length: %s\n", *pktLenArg))
		}
		pktLen.end, err = strconv.Atoi(sizes[1])
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing packet length: %s\n", *pktLenArg))
		}
	} else {
		pktLen.start, err = strconv.Atoi(*pktLenArg)
		if err != nil {
			log.Fatal(fmt.Sprintf("error parsing packet length: %s\n", *pktLenArg))
		}
		pktLen.end = pktLen.start
	}
	pktLen.current = pktLen.start
	if (pktLen.start > MaxPacketLen) || (pktLen.end > MaxPacketLen) {
		log.Fatalf("requested packet length is larger than the maximum permitted size of %d", MaxPacketLen)
	}
	client, err := newClient(*listenAddrArg, *reflectorAddrArg, windowSize, pktLen, duration)
	if err != nil {
		log.Fatal("could not create client: ", err)
	}

	done := make(chan bool)
	durationElapsed := make(chan bool)
	const dbPath = "/tmp/rtt.db"
	log.Printf("sending to %s, window %s packets, packet size %s bytes, duration %d sec, results to %s",
		*reflectorAddrArg, windowSize, pktLen, duration, dbPath)
	go client.reporter(dbPath, done)
	go client.receiver()
	go client.send(durationElapsed)
	<-durationElapsed
	// keep receiving the final window, then exit / timeout a second after duration elapses
	time.Sleep(1 * time.Second)
	done <- true // terminate reporter goroutine
}
