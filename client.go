package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"
)

const PacketBytes = 256
const NumThreads = 10000
const BaseClientPort = 5000

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
	
	threadConnection := make([]*net.UDPConn, NumThreads)

	go func() {

		serverIP := ParseAddress("10.128.0.2:40000")

		threadPacketSent := make([]uint64, NumThreads)
		threadPacketReceived := make([]uint64, NumThreads)

		packetData := make([]byte, PacketBytes)

		wg.Add(NumThreads * 2)

		for i := 0; i < NumThreads; i++ {

			go func(ctx context.Context, thread int) {

				fmt.Printf("started thread %d\n", thread)

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

				if err := conn.SetReadBuffer(10000000); err != nil {
					fmt.Printf("error: could not set connection read buffer size: %v\n", err)
				}
				if err := conn.SetWriteBuffer(10000000); err != nil {
					fmt.Printf("error: could not set connection write buffer size: %v\n", err)
				}

				// write packets

				go func() {

				    ticker := time.NewTicker(time.Millisecond)
				    for {
				        select {
				        case <-ticker.C:
							if _, err := conn.WriteToUDP(packetData, serverIP); err == nil {
								threadPacketSent[thread]++
							}
				        case <-ctx.Done():
				            break
				        }
				    }

					wg.Done()
				}()

				// read packets

				go func() {

					packetReceived := make([]byte, PacketBytes)
					
					for {
						_, _, err := conn.ReadFromUDP(packetReceived)
						if err == nil {
							threadPacketReceived[thread]++
						}
					}

					wg.Done()

				}()

			}(ctx, i)

		}

		// print stats

		go func() {
			var totalSent, totalReceived uint64
			for range time.Tick(time.Second * 5) {
				for i := 0; i < NumThreads; i++ {
					totalSent += threadPacketSent[i]
					totalReceived += threadPacketReceived[i]
				}
				fmt.Printf("sent %d, received %d\n", totalSent, totalReceived)
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

	// todo: shut down clean
	/*
	fmt.Printf("set context as done\n")

	<-ctx.Done()

	fmt.Printf("waiting for threads to join\n")

	wg.Wait()

	for i := range threadConnection {
		threadConnection[i].Close()
	}

	fmt.Printf("done.\n")
	*/
}