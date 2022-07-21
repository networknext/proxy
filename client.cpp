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

#if NEXT_PLATFORM == NEXT_PLATFORM_MAC
#include "next_mac.h"
#elif NEXT_PLATFORM == NEXT_PLATFORM_LINUX
#include "next_linux.h"
#endif 

const char * customer_public_key = "87imaWGyq+J7p3DpwJwstjHGrPQBEl3eCQmsEYWpN8nmi2lCfWD9VA==";

static int numClients;
static int packetBytes;
static int packetsPerSecond;
static int packetBufferSize;
static next_address_t bindAddress;
static next_address_t serverAddress;

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

struct next_platform_thread_t;

struct thread_data_t
{
	int thread_index;
	next_platform_thread_t * thread;
	next_client_t * client;
	uint64_t sent;
	uint64_t received;
	uint64_t lost;
	uint64_t * received_packets;
	
	next_platform_mutex_t stats_mutex;
	uint64_t stats_packets_sent;
	uint64_t stats_packets_received;
	uint64_t stats_packets_lost;
	float stats_latency;
	float stats_jitter;
	float stats_packet_loss;
	bool stats_next;
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

extern next_platform_thread_t * next_platform_thread_create( void * context, next_platform_thread_func_t * func, void * arg );
extern void next_platform_thread_join( next_platform_thread_t * thread );
extern void next_platform_thread_destroy( next_platform_thread_t * thread );

extern int next_platform_mutex_create( next_platform_mutex_t * mutex );
extern void next_platform_mutex_acquire( next_platform_mutex_t * mutex );
extern void next_platform_mutex_release( next_platform_mutex_t * mutex );
extern void next_platform_mutex_destroy( next_platform_mutex_t * mutex );

static next_platform_thread_return_t NEXT_PLATFORM_THREAD_FUNC client_thread_function( void * context )
{
    thread_data_t * thread_data = (thread_data_t*) context;

    next_assert( thread_data );

    printf( "thread %d started\n", thread_data->thread_index );

    char server_address[1024];
    next_address_to_string( &serverAddress, server_address );
    next_client_open_session( thread_data->client, server_address );

    uint8_t packet_data[packetBytes];
    memset( packet_data, 0, sizeof( packet_data ) );

    uint64_t sequence = 0;

    next_sleep( 1.0 );

    while ( !quit )
    {
        next_client_update( thread_data->client );

        if ( next_client_ready( thread_data->client ) )
        {
	        uint8_t * p = packet_data;

	        next_write_uint64( &p, sequence );

	        next_client_send_packet( thread_data->client, packet_data, sizeof(packet_data) );
	        
	        sequence++;
	        thread_data->sent++;

	        const int lookback = packetsPerSecond * 2;

	        if ( sequence >= uint64_t(lookback) )
	        {
	        	int index = ( sequence - lookback ) % packetBufferSize;
	        	if ( thread_data->received_packets[index] != sequence - lookback )
	        	{
	        		// printf( "lost packet %" PRId64 "\n", sequence - lookback );
	        		thread_data->lost++;
	        	}
	        }

		    const next_client_stats_t * stats = next_client_stats( thread_data->client );

		    const float latency = stats->next ? stats->next_rtt : stats->direct_min_rtt;
		    const float jitter = ( stats->jitter_client_to_server + stats->jitter_server_to_client ) / 2;
		    const float packet_loss = stats->next ? stats->next_packet_loss : stats->direct_packet_loss;

			next_platform_mutex_acquire( &thread_data->stats_mutex );
			thread_data->stats_packets_sent = thread_data->sent;
			thread_data->stats_packets_received = thread_data->received;
			thread_data->stats_packets_lost = thread_data->lost;
			thread_data->stats_latency = latency;
			thread_data->stats_jitter = jitter;
			thread_data->stats_packet_loss = packet_loss;
			thread_data->stats_next = stats->next;
			next_platform_mutex_release( &thread_data->stats_mutex );
	    }

        next_sleep( 1.0 / packetsPerSecond );
    }

    printf( "thread %d stopped\n", thread_data->thread_index );

    NEXT_PLATFORM_THREAD_RETURN();
}

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

    // initialize clients

    thread_data_t ** thread_data = (thread_data_t**) malloc( sizeof(thread_data_t*) * numClients );

    for ( int i = 0; i < numClients; ++i )
    {
    	// printf( "creating client %d\n", i );
    	thread_data[i] = (thread_data_t*) malloc( 1 * 1024 * 1024 );
        thread_data[i]->received_packets = (uint64_t*) malloc( 8 * packetBufferSize );
    	memset( thread_data[i]->received_packets, 0xFF, 8 * packetBufferSize );    

	    char bind_address[1024];
	    next_address_to_string( &bindAddress, bind_address );
	    thread_data[i]->client = next_client_create( thread_data[i], bind_address, client_packet_received );
	    if ( thread_data[i]->client == NULL )
	    {
	        printf( "error: failed to create client\n" );
	        exit(1);
	    }

	    // todo
	    printf( "client port is %d\n", next_client_port( thread_data[i]->client ) );
	}

	// create client threads

	for ( int i = 0; i < numClients; ++i )
	{
		thread_data[i]->thread_index = i;
		next_platform_mutex_create( &thread_data[i]->stats_mutex );
		thread_data[i]->thread = next_platform_thread_create( NULL, client_thread_function, thread_data[i] );
		if ( !thread_data[i]->thread )
		{
			printf( "error: could not create client thread\n" );
			exit(1);
		}
	}

	// print stats

	double last_stats_time = next_time();

	while ( !quit )
	{
		next_sleep( 0.1f );

		double current_time = next_time();

		if ( current_time - last_stats_time < 5.0 )
			continue;

		last_stats_time = current_time;

		uint64_t total_sent = 0;
		uint64_t total_received = 0;
		uint64_t total_lost = 0;
		uint64_t total_next = 0;
		float max_latency = 0.0f;
		float max_jitter = 0.0f;
		float max_packet_loss = 0.0f;

		for ( int i = 0; i < numClients; ++i )
		{
			next_platform_mutex_acquire( &thread_data[i]->stats_mutex );
			total_sent += thread_data[i]->stats_packets_sent;
			total_received += thread_data[i]->stats_packets_received;
			total_lost += thread_data[i]->stats_packets_lost;
			if ( thread_data[i]->stats_latency > max_latency )
			{
				max_latency = thread_data[i]->stats_latency;
			}
			if ( thread_data[i]->stats_jitter > max_jitter )
			{
				max_jitter = thread_data[i]->stats_jitter;
			}
			if ( thread_data[i]->stats_packet_loss > max_packet_loss )
			{
				max_packet_loss = thread_data[i]->stats_packet_loss;
			}
			if ( thread_data[i]->stats_next )
			{
				total_next++;
			}
			next_platform_mutex_release( &thread_data[i]->stats_mutex );
		}

		printf( "sent %" PRId64 ", received %" PRId64 ", lost %" PRId64 ", max latency %.2fms, max jitter %.2fms, max packet loss %.1f%%, next %" PRId64 "/%d\n", 
			total_sent, total_received, total_lost, max_latency, max_jitter, max_packet_loss, total_next, numClients );
	}

    // join client threads and destroy them

	for ( int i = 0; i < numClients; ++i )
	{
		next_platform_thread_join( thread_data[i]->thread );
		next_platform_thread_destroy( thread_data[i]->thread );
	}

	// destroy clients

    for ( int i = 0; i < numClients; ++i )
    {
    	// printf( "destroying client %d\n", i );
	    next_client_destroy( thread_data[i]->client );
		next_platform_mutex_destroy( &thread_data[i]->stats_mutex );
	}

    // destroy thread data

    for ( int i = 0; i < numClients; ++i )
    {
    	free( thread_data[i]->received_packets );
    	free( thread_data[i] );
    }
    
    free( thread_data );
    
	// shut down network next

    next_term();

    return 0;
}
