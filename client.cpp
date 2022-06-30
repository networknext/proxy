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

const char * bind_address = "0.0.0.0:2000";
const char * server_address = "127.0.0.1:40000";
const char * customer_public_key = "leN7D7+9vr24uT4f1Ba8PEEvIQA/UkGZLlT+sdeLRHKsVqaZq723Zw==";

static volatile int quit = 0;

extern void next_write_uint64( uint8_t ** p, uint64_t value );

extern uint64_t next_read_uint64( const uint8_t ** p );

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

static uint64_t sent, received, lost;
static uint64_t received_packets[1024];

void client_packet_received( next_client_t * client, void * context, const next_address_t * from, const uint8_t * packet_data, int packet_bytes )
{
    (void) client; (void) context; (void) packet_data; (void) packet_bytes; (void) from;

	if ( packet_bytes < 8 )
	{
		// printf( "packet too small: %d\n", packet_bytes );
		return;
	}

	const uint8_t * p = packet_data;

	uint64_t sequence = next_read_uint64( &p );

    // printf( "client received packet %" PRId64 "\n", sequence );

	received_packets[sequence%1024] = sequence;

	received++;
}

#if NEXT_PLATFORM != NEXT_PLATFORM_WINDOWS
#define strncpy_s strncpy
#endif // #if NEXT_PLATFORM != NEXT_PLATFORM_WINDOWS

int main()
{
    signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    memset( received_packets, 0xFF, sizeof(received_packets) );
    
 	// next_quiet( true );

    next_config_t config;
    next_default_config( &config );
    strncpy_s( config.customer_public_key, customer_public_key, sizeof(config.customer_public_key) - 1 );

    if ( next_init( NULL, &config ) != NEXT_OK )
    {
        printf( "error: could not initialize network next\n" );
        return 1;
    }

    next_client_t * client = next_client_create( NULL, bind_address, client_packet_received );
    if ( client == NULL )
    {
        printf( "error: failed to create client\n" );
        return 1;
    }

    printf( "client port is %d\n", next_client_port( client ) );

    next_client_open_session( client, server_address );

    uint8_t packet_data[100];
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

	        if ( sequence >= 100 )
	        {
	        	int index = ( sequence - 100 ) % 1024;
	        	if ( received_packets[index] != sequence - 100 )
	        	{
	        		// printf( "lost packet %" PRId64 "\n", sequence - 100 );
	        		lost++;
	        	}
	        }

	        double current_time = next_time();

	        if ( current_time - last_print_time > 5.0 )
	        {
	        	printf( "sent %" PRId64 ", received %" PRId64 ", lost %" PRId64 "\n", sent, received, lost );

	        	last_print_time = current_time;
	        }
	    }

        next_sleep( 0.01 );
    }

    next_client_destroy( client );
    
    next_term();
    
    return 0;
}
