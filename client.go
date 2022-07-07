package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"encoding/binary"
)

const ServerAddress = "10.128.0.10:40000"

const NumClients = 1000
const PacketsPerSecond = 100
const PacketBytes = 1200
const BaseClientPort = 55000
const SocketReadBuffer = 10000000
const SocketWriteBuffer = 10000000

func ParseAddress(input string) *net.UDPAddr {
	address := &net.UDPAddr{}
	ip_string, port_string, err := net.SplitHostPort(input)
	if err != nil {
		address.IP = net.ParseIP(input)
		address.Port = 0
		return address
	}
	address.IP = net.ParseIP(ip_string)
	address.Port, _ = strconv.Atoi(port_string)
	return address
}

func main() {

	// run everything in parallel in a goroutine from main thread

	var wg sync.WaitGroup

	ctx := context.Background()
	
	threadConnection := make([]*net.UDPConn, NumClients)

	go func() {

		serverIP := ParseAddress(ServerAddress)

		threadPacketSent := make([]uint64, NumClients)
		threadPacketReceived := make([]uint64, NumClients)
		threadPacketLost := make([]uint64, NumClients)

		wg.Add(NumClients * 2)

		for i := 0; i < NumClients; i++ {

			go func(ctx context.Context, thread int) {

				fmt.Printf("started client %d\n", thread)

				port := BaseClientPort + thread
				address := "0.0.0.0:" + strconv.Itoa(port)

				lc := net.ListenConfig{}

				lp, err := lc.ListenPacket(ctx, "udp", address)
				if err != nil {
					fmt.Printf("error: could not bind socket: %v\n", err)
					os.Exit(1)
				}

				conn := lp.(*net.UDPConn)
				threadConnection[thread] = conn

				if err := conn.SetReadBuffer(SocketReadBuffer); err != nil {
					fmt.Printf("error: could not set connection read buffer size: %v\n", err)
				}
				if err := conn.SetWriteBuffer(SocketWriteBuffer); err != nil {
					fmt.Printf("error: could not set connection write buffer size: %v\n", err)
				}

				// track packet loss

				const PacketBufferSize = PacketsPerSecond * 2;

				receivedPackets := make([]uint64, PacketBufferSize)

				// write packets

				go func() {

					writePacketData := make([]byte, PacketBytes)
					writeSequence := uint64(0)

				    ticker := time.NewTicker(10 * time.Millisecond)
				    for {
				        select {
				        case <-ticker.C:
				        	// passthrough packet
				        	writePacketData[0] = 0
			        		binary.LittleEndian.PutUint64(writePacketData[1:9], writeSequence)
							if _, err := conn.WriteToUDP(writePacketData, serverIP); err == nil {
								if writeSequence > 100 {
									oldSequence := writeSequence - 100
									index := oldSequence % PacketBufferSize
									if atomic.LoadUint64(&receivedPackets[index]) != oldSequence {
										atomic.AddUint64(&threadPacketLost[thread], 1)
									}
								}
								atomic.AddUint64(&threadPacketSent[thread], 1)
								writeSequence++
							}
				        case <-ctx.Done():
				            break
				        }
				    }

					wg.Done()
				}()

				// read packets

				go func() {

					readPacketData := make([]byte, PacketBytes)
					
					for {
						readPacketBytes, _, err := conn.ReadFromUDP(readPacketData)
						if err == nil && readPacketBytes == PacketBytes {
							if readPacketData[0] == 0 {
								// passthrough packet
								readSequence := binary.LittleEndian.Uint64(readPacketData[1:9])
								index := readSequence % PacketBufferSize
								atomic.StoreUint64(&receivedPackets[index], readSequence)
								atomic.AddUint64(&threadPacketReceived[thread], 1)
							}
						}
					}

					wg.Done()

				}()

			}(ctx, i)

		}

		// print stats

		go func() {
			var totalSent, totalReceived, totalLost uint64
			for range time.Tick(time.Second * 5) {
				for i := 0; i < NumClients; i++ {
					sent := atomic.LoadUint64(&threadPacketSent[i])
					received := atomic.LoadUint64(&threadPacketReceived[i])
					lost := atomic.LoadUint64(&threadPacketLost[i])
					totalSent += sent
					totalReceived += received
					totalLost += lost
				}
				fmt.Printf("sent %d, received %d, lost %d\n", totalSent, totalReceived, totalLost)
			}
		}()

	}()

	// block and wait for shutdown signal

	termChan := make(chan os.Signal, 1)

	signal.Notify(termChan, os.Interrupt, syscall.SIGTERM)

	select {
	case <-termChan:
		fmt.Printf("\nshutting down...\n")
		return
	}
}
