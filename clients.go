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

var serverAddress *net.UDPAddr
var numClients int
var packetsPerSecond int
var packetBytes int
var baseClientPort int
var socketReadBuffer int
var socketWriteBuffer int

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

func GetEnvString(name string, defaultValue string) string {
	string, ok := os.LookupEnv(name)
	if !ok {
		return defaultValue
	}
	return string
}

func GetEnvInt(name string, defaultValue int) int {
	string, ok := os.LookupEnv(name)
	if !ok {
		return defaultValue
	}
	value, err := strconv.ParseInt(string, 10, 64)
	if err != nil {
		return defaultValue
	}
	return int(value)
}

func GetEnvAddress(name string, defaultValue string) *net.UDPAddr {
	string, ok := os.LookupEnv(name)
	if !ok {
		return ParseAddress(defaultValue)
	}
	return ParseAddress(string)
}

func main() {

	// configure

	serverAddress := GetEnvAddress("SERVER_ADDRESS", "127.0.0.1:65000")
	numClients := GetEnvInt("NUM_CLIENTS", 10)
	packetsPerSecond := GetEnvInt("PACKETS_PER_SECOND", 1)
	packetBytes := GetEnvInt("PACKET_BYTES", 100)
	baseClientPort := GetEnvInt("BASE_CLIENT_PORT", 55000)
	socketReadBuffer := GetEnvInt("SOCKET_READ_BUFFER", 1000000)
	socketWriteBuffer := GetEnvInt("SOCKET_WRITE_BUFFER", 1000000)

	// run everything in parallel in a goroutine from main thread

	var wg sync.WaitGroup

	ctx := context.Background()
	
	threadConnection := make([]*net.UDPConn, numClients)

	go func() {

		threadPacketSent := make([]uint64, numClients)
		threadPacketReceived := make([]uint64, numClients)
		threadPacketLost := make([]uint64, numClients)

		wg.Add(numClients * 2)

		for i := 0; i < numClients; i++ {

			go func(ctx context.Context, thread int) {

				fmt.Printf("started client %d\n", thread)

				port := baseClientPort + thread
				address := "0.0.0.0:" + strconv.Itoa(port)

				lc := net.ListenConfig{}

				lp, err := lc.ListenPacket(ctx, "udp", address)
				if err != nil {
					fmt.Printf("error: could not bind socket: %v\n", err)
					os.Exit(1)
				}

				conn := lp.(*net.UDPConn)
				threadConnection[thread] = conn

				if err := conn.SetReadBuffer(socketReadBuffer); err != nil {
					fmt.Printf("error: could not set connection read buffer size: %v\n", err)
				}
				if err := conn.SetWriteBuffer(socketWriteBuffer); err != nil {
					fmt.Printf("error: could not set connection write buffer size: %v\n", err)
				}

				// track packet loss

				packetBufferSize := packetsPerSecond * 2;

				receivedPackets := make([]uint64, packetBufferSize)

				// write packets

				go func() {

					writePacketData := make([]byte, packetBytes)
					writeSequence := uint64(0)

					tickRate := time.Duration(1000000000 / packetsPerSecond)

					ticker := time.NewTicker(tickRate)

				    for {
				        select {
				        case <-ticker.C:
				        	// passthrough packet
				        	writePacketData[0] = 0
			        		binary.LittleEndian.PutUint64(writePacketData[1:9], writeSequence)
							if _, err := conn.WriteToUDP(writePacketData, serverAddress); err == nil {
								lookback := uint64(packetsPerSecond)
								if writeSequence > lookback {
									oldSequence := writeSequence - lookback
									index := oldSequence % uint64(packetBufferSize)
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

					readPacketData := make([]byte, packetBytes)
					
					for {
						readPacketBytes, _, err := conn.ReadFromUDP(readPacketData)
						if err == nil && readPacketBytes == packetBytes {
							if readPacketData[0] == 0 {
								// passthrough packet
								readSequence := binary.LittleEndian.Uint64(readPacketData[1:9])
								index := readSequence % uint64(packetBufferSize)
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
			for range time.Tick(time.Second * 5) {
				var totalSent, totalReceived, totalLost uint64
				for i := 0; i < numClients; i++ {
					sent := atomic.LoadUint64(&threadPacketSent[i])
					received := atomic.LoadUint64(&threadPacketReceived[i])
					lost := atomic.LoadUint64(&threadPacketLost[i])
					totalSent += sent
					totalReceived += received
					totalLost += lost
				}
				if totalReceived > totalSent {
					totalReceived = totalSent
				}
				if totalLost > totalSent {
					totalLost = totalSent
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
