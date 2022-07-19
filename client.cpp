/*
    Network Next SDK. Copyright © 2017 - 2022 Network Next, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following 
    conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions 
       and the following disclaimer in the documentation and/or other materials provided with the distribution.

    3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote 
       products derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
    IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; 
    OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
    NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "next.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

static int numClients;
static int packetBytes;
static int packetsPerSecond;
static int packetBufferSize;
static next_address_t bindAddress;
static next_address_t serverAddress;

const char * customer_public_key = "leN7D7+9vr24uT4f1Ba8PEEvIQA/UkGZLlT+sdeLRHKsVqaZq723Zw==";

int read_env_int( const char * env, int default_value )
{
	assert( env );
	const char * env_string = getenv( env );
	if ( env_string )
	{
		return atoi( env_string );
	}
	else
	{
		return default_value;
	}
}

next_address_t read_env_address( const char * env, const char * default_value )
{
	assert( env );
	const char * address_string = default_value;
	const char * env_string = getenv( env );
	if ( env_string )
	{
		address_string = env_string;
	}
	next_address_t address;
	next_address_parse( &address, address_string );
	return address;
}

static volatile int quit = 0;

extern void next_write_uint64( uint8_t ** p, uint64_t value );

extern uint64_t next_read_uint64( const uint8_t ** p );

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

struct thread_data_t
{
	next_client_t * client;
	uint64_t sent;
	uint64_t received;
	uint64_t lost;
	uint64_t * received_packets;
};

void client_packet_received( next_client_t * client, void * context, const next_address_t * from, const uint8_t * packet_data, int packet_bytes )
{
    (void) client; (void) from;

	if ( packet_bytes < 8 )
		return;

    next_assert( context );

    thread_data_t * thread_data = (thread_data_t*) context;

	const uint8_t * p = packet_data;

	uint64_t sequence = next_read_uint64( &p );

    // printf( "client received %d byte packet %" PRId64 "\n", packet_bytes, sequence );

	thread_data->received_packets[sequence%packetBufferSize] = sequence;

	thread_data->received++;
}

#if NEXT_PLATFORM != NEXT_PLATFORM_WINDOWS
#define strncpy_s strncpy
#endif // #if NEXT_PLATFORM != NEXT_PLATFORM_WINDOWS

int main()
{
	numClients = read_env_int( "NUM_CLIENTS", 1 );
	packetBytes = read_env_int( "PACKET_BYTES", 100 );
	packetsPerSecond = read_env_int( "PACKETS_PER_SECOND", 1 );
	packetBufferSize = packetsPerSecond * 10;
	bindAddress = read_env_address( "BIND_ADDRESS", "0.0.0.0:0" );
	serverAddress = read_env_address( "SERVER_ADDRESS", "127.0.0.1:65000" );

	if ( numClients > 1 )
	{
		printf( "%d clients\n", numClients );
	}
	else
	{
		printf( "1 client\n" );
	}

	printf( "%d byte packets\n", packetBytes );

	if ( packetsPerSecond == 1 ) 
	{
		printf( "1 packet per-second\n" );
	}
	else
	{
		printf( "%d packets per-second\n", packetsPerSecond );
	}

	char buffer[1024];
	printf( "bind address is %s\n", next_address_to_string( &bindAddress, buffer ) );
	printf( "server address is %s\n", next_address_to_string( &serverAddress, buffer ) );

	assert( numClients > 0 );
	assert( packetBytes > 0 );
	assert( packetsPerSecond > 0 );
	assert( packetBufferSize > 0 );

    signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    next_quiet( true );

    next_config_t config;
    next_default_config( &config );
    config.force_passthrough_direct = true;
    strncpy_s( config.customer_public_key, customer_public_key, sizeof(config.customer_public_key) - 1 );

    if ( next_init( NULL, &config ) != NEXT_OK )
    {
        printf( "error: could not initialize network next\n" );
        return 1;
    }

    // --------------------------------------

    thread_data_t ** thread_data = (thread_data_t**) malloc( sizeof(thread_data_t*) * numClients );

    for ( int i = 0; i < numClients; ++i )
    {
    	// printf( "creating client %d\n", i );
    	thread_data[i] = (thread_data_t*) malloc( 1 * 1024 * 1024 );
        thread_data[i]->received_packets = (uint64_t*) malloc( 8 * packetBufferSize );
    	memset( thread_data[i]->received_packets, 0xFF, 8 * packetBufferSize );    

	    char bind_address[1024];
	    next_address_to_string( &bindAddress, bind_address );
	    thread_data[i]->client = next_client_create( NULL, bind_address, client_packet_received );
	    if ( thread_data[i]->client == NULL )
	    {
	        printf( "error: failed to create client\n" );
	        exit(1);
	    }

	    // printf( "client port is %d\n", next_client_port( thread_data[i]->client ) );
	}

	/*
    char server_address[1024];
    next_address_to_string( &serverAddress, server_address );
    next_client_open_session( client, server_address );

    uint8_t packet_data[packetBytes];
    memset( packet_data, 0, sizeof( packet_data ) );

    uint64_t sequence = 0;

    double last_print_time = next_time();

    next_sleep( 1.0 );

    while ( !quit )
    {
        next_client_update( client );

        if ( next_client_ready( client ) )
        {
	        uint8_t * p = packet_data;

	        next_write_uint64( &p, sequence );

	        next_client_send_packet( client, packet_data, sizeof(packet_data) );
	        
	        sequence++;
	        sent++;

	        const int lookback = packetsPerSecond * 2;

	        if ( sequence >= uint64_t(lookback) )
	        {
	        	int index = ( sequence - lookback ) % packetBufferSize;
	        	if ( received_packets[index] != sequence - lookback )
	        	{
	        		// printf( "lost packet %" PRId64 "\n", sequence - 100 );
	        		lost++;
	        	}
	        }

	        double current_time = next_time();

	        if ( current_time - last_print_time > 5.0 )
	        {
			    const next_client_stats_t * stats = next_client_stats( client );

			    const float latency = stats->next ? stats->next_rtt : stats->direct_min_rtt;
			    const float jitter = ( stats->jitter_client_to_server + stats->jitter_server_to_client ) / 2;
			    const float packet_loss = stats->next ? stats->next_packet_loss : stats->direct_packet_loss;

	        	printf( "sent %" PRId64 ", received %" PRId64 ", lost %" PRId64 ", latency %.2fms, jitter %.2fms, packet loss %.1f%%\n", 
	        		sent, received, lost, latency, jitter, packet_loss );

	        	last_print_time = current_time;
	        }
	    }

        next_sleep( 1.0 / packetsPerSecond );
    }
    */

	// ----------------------------------------------------
    
    for ( int i = 0; i < numClients; ++i )
    {
    	// printf( "destroying client %d\n", i );
	    next_client_destroy( thread_data[i]->client );
	}

    next_term();

    for ( int i = 0; i < numClients; ++i )
    {
    	free( thread_data[i]->received_packets );
    	free( thread_data[i] );
    }
    
    free( thread_data );
    
    return 0;
}
