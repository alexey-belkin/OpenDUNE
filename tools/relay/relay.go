// Command relay forwards OpenDUNE multiplayer packets between the players of a
// match.
//
// It is deliberately ignorant of the game. A packet arrives, it goes to
// everybody else in the room, and that is the whole of it: the relay never
// parses a command, never holds state a client could disagree with, and never
// decides anything. Two clients that disagree about the world find out from
// each other's checksums, not from here -- which is what keeps this a pipe
// rather than a server, and keeps a bug in it from becoming a bug in the game.
//
// Why a relay at all, rather than a direct connection: peer to peer over the
// internet needs hole punching and still fails behind symmetric NAT, which is
// most home routers and every mobile network. Both clients dialling out to one
// public address works everywhere, and it gives room codes for free.
//
// Traffic is nothing. A client sends one packet per turn -- 7.5 per second at
// the default turn length -- of a few dozen bytes. A match is under a kilobyte
// per second in both directions together.
//
// The wire protocol is line-oriented and readable on purpose. When a match
// desyncs, the packets are the evidence, and being able to watch a room with
// netcat is worth more than the bytes it costs.
//
//	client -> relay   JOIN <room> <slot>\n
//	relay  -> client  WELCOME <slot> <members>\n
//	relay  -> client  READY <members>\n          once the room is full
//	relay  -> client  LEFT <slot>\n              when somebody drops
//	client -> relay   PKT <length>\n<length bytes>
//	relay  -> client  PKT <slot> <length>\n<length bytes>
//
// The slot in an outgoing PKT is filled in by the relay from the connection it
// arrived on, never from what the sender claims: a client cannot speak for its
// opponent.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	maxSlots      = 2
	maxPacketSize = 64 * 1024
	joinTimeout   = 60 * time.Second
	idleTimeout   = 120 * time.Second
)

type client struct {
	conn net.Conn
	slot int
	out  chan []byte
	once sync.Once
}

// send never blocks the reader that calls it. A client too slow to drain its
// own socket is a client that has already lost the match; dropping it is
// better than letting it stall the writer for everybody else.
func (c *client) send(b []byte) {
	select {
	case c.out <- b:
	default:
		c.close()
	}
}

func (c *client) close() {
	c.once.Do(func() {
		close(c.out)
		c.conn.Close()
	})
}

type room struct {
	mu      sync.Mutex
	name    string
	members map[int]*client
}

type server struct {
	mu    sync.Mutex
	rooms map[string]*room
}

func (s *server) room(name string) *room {
	s.mu.Lock()
	defer s.mu.Unlock()

	r, ok := s.rooms[name]
	if !ok {
		r = &room{name: name, members: make(map[int]*client)}
		s.rooms[name] = r
	}

	return r
}

func (s *server) forget(name string) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if r, ok := s.rooms[name]; ok {
		r.mu.Lock()
		empty := len(r.members) == 0
		r.mu.Unlock()

		if empty {
			delete(s.rooms, name)
		}
	}
}

func (r *room) join(c *client) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, taken := r.members[c.slot]; taken {
		return fmt.Errorf("slot %d is taken", c.slot)
	}
	if len(r.members) >= maxSlots {
		return fmt.Errorf("room is full")
	}

	r.members[c.slot] = c

	return nil
}

func (r *room) leave(c *client) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.members[c.slot] == c {
		delete(r.members, c.slot)
	}

	for _, other := range r.members {
		other.send([]byte(fmt.Sprintf("LEFT %d\n", c.slot)))
	}
}

func (r *room) count() int {
	r.mu.Lock()
	defer r.mu.Unlock()

	return len(r.members)
}

// lag holds every forwarded frame back by a fixed delay, so a match played
// between two processes on one machine can be made to feel like a match played
// across a continent. It is here rather than in the game because the game must
// not know: a client that could tell the difference between a slow relay and a
// slow opponent would be a client with a second source of truth.
//
// Ordering survives because the delay is the same for every frame, and
// time.AfterFunc fires timers armed in order at the same duration in order.
var lag time.Duration

func (r *room) broadcast(from *client, payload []byte) {
	header := []byte(fmt.Sprintf("PKT %d %d\n", from.slot, len(payload)))
	frame := append(header, payload...)

	r.mu.Lock()
	defer r.mu.Unlock()

	for slot, other := range r.members {
		if slot == from.slot {
			continue
		}

		if lag == 0 {
			other.send(frame)
			continue
		}

		target := other
		time.AfterFunc(lag, func() { target.send(frame) })
	}
}

func (r *room) announceReady() {
	r.mu.Lock()
	defer r.mu.Unlock()

	if len(r.members) < maxSlots {
		return
	}

	for _, c := range r.members {
		c.send([]byte(fmt.Sprintf("READY %d\n", len(r.members))))
	}
}

// writer owns the socket's write end for the lifetime of the connection, so no
// two goroutines ever interleave halfway through a frame.
func (c *client) writer(done chan<- struct{}) {
	defer close(done)

	for b := range c.out {
		if _, err := c.conn.Write(b); err != nil {
			return
		}
	}
}

func (s *server) handle(conn net.Conn, verbose bool) {
	defer conn.Close()

	reader := bufio.NewReaderSize(conn, maxPacketSize+64)

	conn.SetReadDeadline(time.Now().Add(joinTimeout))

	line, err := reader.ReadString('\n')
	if err != nil {
		return
	}

	fields := strings.Fields(strings.TrimSpace(line))
	if len(fields) != 3 || fields[0] != "JOIN" {
		fmt.Fprintf(conn, "ERROR expected JOIN <room> <slot>\n")
		return
	}

	slot, err := strconv.Atoi(fields[2])
	if err != nil || slot < 0 || slot >= maxSlots {
		fmt.Fprintf(conn, "ERROR slot must be 0..%d\n", maxSlots-1)
		return
	}

	roomName := fields[1]
	if len(roomName) == 0 || len(roomName) > 64 {
		fmt.Fprintf(conn, "ERROR room name must be 1..64 characters\n")
		return
	}

	c := &client{conn: conn, slot: slot, out: make(chan []byte, 256)}
	r := s.room(roomName)

	if err := r.join(c); err != nil {
		fmt.Fprintf(conn, "ERROR %s\n", err)
		s.forget(roomName)
		return
	}

	defer func() {
		r.leave(c)
		c.close()
		s.forget(roomName)
	}()

	written := make(chan struct{})
	go c.writer(written)

	c.send([]byte(fmt.Sprintf("WELCOME %d %d\n", slot, r.count())))
	r.announceReady()

	if verbose {
		log.Printf("room %q: slot %d joined from %s (%d present)", roomName, slot, conn.RemoteAddr(), r.count())
	}

	packets := 0
	body := make([]byte, maxPacketSize)

	for {
		conn.SetReadDeadline(time.Now().Add(idleTimeout))

		line, err := reader.ReadString('\n')
		if err != nil {
			if verbose && err != io.EOF {
				log.Printf("room %q: slot %d read: %v", roomName, slot, err)
			}
			break
		}

		fields := strings.Fields(strings.TrimSpace(line))
		if len(fields) != 2 || fields[0] != "PKT" {
			c.send([]byte("ERROR expected PKT <length>\n"))
			break
		}

		length, err := strconv.Atoi(fields[1])
		if err != nil || length < 0 || length > maxPacketSize {
			c.send([]byte("ERROR bad packet length\n"))
			break
		}

		if _, err := io.ReadFull(reader, body[:length]); err != nil {
			break
		}

		payload := make([]byte, length)
		copy(payload, body[:length])

		r.broadcast(c, payload)
		packets++
	}

	c.close()
	<-written

	if verbose {
		log.Printf("room %q: slot %d left after %d packets", roomName, slot, packets)
	}
}

func main() {
	address := flag.String("listen", ":31337", "address to listen on")
	verbose := flag.Bool("verbose", false, "log joins, leaves and errors")
	delay := flag.Duration("lag", 0, "hold every forwarded packet back this long, to emulate a ping")
	flag.Parse()

	lag = *delay

	listener, err := net.Listen("tcp", *address)
	if err != nil {
		log.Fatalf("listen %s: %v", *address, err)
	}

	if lag != 0 {
		log.Printf("opendune relay listening on %s, forwarding with %s of added lag", listener.Addr(), lag)
	} else {
		log.Printf("opendune relay listening on %s", listener.Addr())
	}

	s := &server{rooms: make(map[string]*room)}

	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("accept: %v", err)
			continue
		}

		// Every turn carries one small packet and the game stalls until it
		// arrives, so waiting to fill a segment is exactly the wrong trade.
		if tcp, ok := conn.(*net.TCPConn); ok {
			tcp.SetNoDelay(true)
		}

		go s.handle(conn, *verbose)
	}
}
