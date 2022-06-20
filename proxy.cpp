/*
    Network Next Proxy. Copyright © 2017 - 2022 Network Next, Inc.

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

#include "proxy.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>

// ---------------------------------------------------------------------

extern bool proxy_platform_init();

extern void proxy_platform_term();

extern int proxy_platform_num_cores();

struct proxy_config_t
{
	proxy_address_t bind_address;
	int num_threads;
	int max_packet_size;
	int thread_data_bytes;
    int socket_send_buffer_size;
    int socket_receive_buffer_size;
};

static proxy_config_t config;

bool proxy_init()
{
	if ( !proxy_platform_init() )
		return false;

	config.num_threads = 0;

	config.max_packet_size = 1500;

	config.thread_data_bytes = 10 * 1024 * 1024;

	memset( &config.bind_address, 0, sizeof(proxy_address_t) );
	config.bind_address.type = PROXY_ADDRESS_IPV4;
	config.bind_address.port = 40000;

	config.socket_send_buffer_size = 10000000;
	config.socket_receive_buffer_size = 10000000;

	// todo: env overrides

	if ( config.num_threads <= 0 )
	{
		config.num_threads = proxy_platform_num_cores();
	}

    return true;
}

void proxy_term()
{
	proxy_platform_term();
}

// ---------------------------------------------------------------------

static int log_level = PROXY_LOG_LEVEL_INFO;

const char * proxy_log_level_string( int level )
{
    if ( level == PROXY_LOG_LEVEL_DEBUG )
        return "debug";
    else if ( level == PROXY_LOG_LEVEL_INFO )
        return "info";
    else if ( level == PROXY_LOG_LEVEL_ERROR )
        return "error";
    else if ( level == PROXY_LOG_LEVEL_WARN )
        return "warning";
    else
        return "???";
}

static void default_log_function( int level, const char * format, ... )
{
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    if ( level != PROXY_LOG_LEVEL_NONE )
    {
        const char * level_string = proxy_log_level_string( level );
        printf( "%.6f: %s: %s\n", proxy_time(), level_string, buffer );
    }
    else
    {
        printf( "%s\n", buffer );
    }
    va_end( args );
    fflush( stdout );
}

static void (*log_function)( int level, const char * format, ... ) = default_log_function;

void proxy_printf( const char * format, ... )
{
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof(buffer), format, args );
    log_function( PROXY_LOG_LEVEL_NONE, "%s", buffer );
    va_end( args );
}

void proxy_printf( int level, const char * format, ... )
{
    if ( level > log_level )
        return;
    va_list args;
    va_start( args, format );
    char buffer[1024];
    vsnprintf( buffer, sizeof( buffer ), format, args );
    log_function( level, "%s", buffer );
    va_end( args );
}

uint16_t proxy_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t proxy_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

#include "proxy_mac.h"
#include "proxy_linux.h"

// ---------------------------------------------------------------------

extern const char * proxy_platform_getenv( const char * );

extern double proxy_platform_time();

extern void proxy_platform_sleep( double seconds );

extern bool proxy_platform_inet_pton4( const char * address_string, uint32_t * address_out );

extern bool proxy_platform_inet_pton6( const char * address_string, uint16_t * address_out );

extern bool proxy_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size );

extern proxy_platform_socket_t * proxy_platform_socket_create( proxy_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size );

extern void proxy_platform_socket_destroy( proxy_platform_socket_t * socket );

extern void proxy_platform_socket_close( proxy_platform_socket_t * socket );

extern void proxy_platform_socket_send_packet( proxy_platform_socket_t * socket, const proxy_address_t * to, const void * packet_data, int packet_bytes );

extern int proxy_platform_socket_receive_packet( proxy_platform_socket_t * socket, proxy_address_t * from, void * packet_data, int max_packet_size );

extern int proxy_platform_id();

extern int proxy_platform_connection_type();

extern int proxy_platform_hostname_resolve( const char * hostname, const char * port, proxy_address_t * address );

extern proxy_platform_thread_t * proxy_platform_thread_create( proxy_platform_thread_func_t * func, void * arg );

extern void proxy_platform_thread_join( proxy_platform_thread_t * thread );

extern void proxy_platform_thread_destroy( proxy_platform_thread_t * thread );

extern bool proxy_platform_thread_high_priority( proxy_platform_thread_t * thread );

extern bool proxy_platform_mutex_create( proxy_platform_mutex_t * mutex );

extern void proxy_platform_mutex_acquire( proxy_platform_mutex_t * mutex );

extern void proxy_platform_mutex_release( proxy_platform_mutex_t * mutex );

extern void proxy_platform_mutex_destroy( proxy_platform_mutex_t * mutex );

struct proxy_platform_mutex_helper_t
{
    proxy_platform_mutex_t * mutex;
    proxy_platform_mutex_helper_t( proxy_platform_mutex_t * mutex );
    ~proxy_platform_mutex_helper_t();
};

#define proxy_platform_mutex_guard( _mutex ) proxy_platform_mutex_helper_t __mutex_helper( _mutex )

proxy_platform_mutex_helper_t::proxy_platform_mutex_helper_t( proxy_platform_mutex_t * mutex ) : mutex( mutex )
{
    assert( mutex );
    proxy_platform_mutex_acquire( mutex );
}

proxy_platform_mutex_helper_t::~proxy_platform_mutex_helper_t()
{
    assert( mutex );
    proxy_platform_mutex_release( mutex );
    mutex = NULL;
}

// ---------------------------------------------------------------------

double proxy_time()
{
    return proxy_platform_time();
}

void proxy_sleep( double time_seconds )
{
    proxy_platform_sleep( time_seconds );
}

// ---------------------------------------------------------------------

bool proxy_address_parse( proxy_address_t * address, const char * address_string_in )
{
    assert( address );
    assert( address_string_in );

    if ( !address )
        return false;

    if ( !address_string_in )
        return false;

    memset( address, 0, sizeof( proxy_address_t ) );

    // first try to parse the string as an IPv6 address:
    // 1. if the first character is '[' then it's probably an ipv6 in form "[addr6]:portnum"
    // 2. otherwise try to parse as an IPv6 address using inet_pton

    char buffer[PROXY_MAX_ADDRESS_STRING_LENGTH + PROXY_ADDRESS_BUFFER_SAFETY*2];

    char * address_string = buffer + PROXY_ADDRESS_BUFFER_SAFETY;
    strncpy( address_string, address_string_in, PROXY_MAX_ADDRESS_STRING_LENGTH );
    address_string[PROXY_MAX_ADDRESS_STRING_LENGTH-1] = '\0';

    int address_string_length = (int) strlen( address_string );

    if ( address_string[0] == '[' )
    {
        const int base_index = address_string_length - 1;

        // note: no need to search past 6 characters as ":65535" is longest possible port value
        for ( int i = 0; i < 6; ++i )
        {
            const int index = base_index - i;
            if ( index < 0 )
            {
                return false;
            }
            if ( address_string[index] == ':' )
            {
                address->port = (uint16_t) ( atoi( &address_string[index + 1] ) );
                address_string[index-1] = '\0';
                break;
            }
            else if ( address_string[index] == ']' )
            {
                // no port number
                address->port = 0;
                address_string[index] = '\0';
                break;
            }
        }
        address_string += 1;
    }

    uint16_t addr6[8];
    if ( proxy_platform_inet_pton6( address_string, addr6 ) )
    {
        address->type = PROXY_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            address->data.ipv6[i] = proxy_ntohs( addr6[i] );
        }
        return true;
    }

    // otherwise it's probably an IPv4 address:
    // 1. look for ":portnum", if found save the portnum and strip it out
    // 2. parse remaining ipv4 address via inet_pton

    address_string_length = (int) strlen( address_string );
    const int base_index = address_string_length - 1;
    for ( int i = 0; i < 6; ++i )
    {
        const int index = base_index - i;
        if ( index < 0 )
            break;
        if ( address_string[index] == ':' )
        {
            address->port = (uint16_t)( atoi( &address_string[index + 1] ) );
            address_string[index] = '\0';
        }
    }

    uint32_t addr4;
    if ( proxy_platform_inet_pton4( address_string, &addr4 ) )
    {
        address->type = PROXY_ADDRESS_IPV4;
        address->data.ipv4[3] = (uint8_t) ( ( addr4 & 0xFF000000 ) >> 24 );
        address->data.ipv4[2] = (uint8_t) ( ( addr4 & 0x00FF0000 ) >> 16 );
        address->data.ipv4[1] = (uint8_t) ( ( addr4 & 0x0000FF00 ) >> 8  );
        address->data.ipv4[0] = (uint8_t) ( ( addr4 & 0x000000FF )     );
        return true;
    }

    return false;
}

const char * proxy_address_to_string( const proxy_address_t * address, char * buffer )
{
    assert( buffer );

    if ( address->type == PROXY_ADDRESS_IPV6 )
    {
#if defined(WINVER) && WINVER <= 0x0502
        // ipv6 not supported
        buffer[0] = '\0';
        return buffer;
#else
        uint16_t ipv6_network_order[8];
        for ( int i = 0; i < 8; ++i )
            ipv6_network_order[i] = proxy_htons( address->data.ipv6[i] );
        char address_string[PROXY_MAX_ADDRESS_STRING_LENGTH];
        proxy_platform_inet_ntop6( ipv6_network_order, address_string, sizeof( address_string ) );
        if ( address->port == 0 )
        {
            strncpy( buffer, address_string, PROXY_MAX_ADDRESS_STRING_LENGTH );
            address_string[PROXY_MAX_ADDRESS_STRING_LENGTH-1] = '\0';
            return buffer;
        }
        else
        {
            if ( snprintf( buffer, PROXY_MAX_ADDRESS_STRING_LENGTH, "[%s]:%hu", address_string, address->port ) < 0 )
            {
                proxy_printf( PROXY_LOG_LEVEL_ERROR, "address string truncated: [%s]:%hu", address_string, address->port );
            }
            return buffer;
        }
#endif
    }
    else if ( address->type == PROXY_ADDRESS_IPV4 )
    {
        if ( address->port != 0 )
        {
            snprintf( buffer,
                      PROXY_MAX_ADDRESS_STRING_LENGTH,
                      "%d.%d.%d.%d:%d",
                      address->data.ipv4[0],
                      address->data.ipv4[1],
                      address->data.ipv4[2],
                      address->data.ipv4[3],
                      address->port );
        }
        else
        {
            snprintf( buffer,
                      PROXY_MAX_ADDRESS_STRING_LENGTH,
                      "%d.%d.%d.%d",
                      address->data.ipv4[0],
                      address->data.ipv4[1],
                      address->data.ipv4[2],
                      address->data.ipv4[3] );
        }
        return buffer;
    }
    else
    {
        snprintf( buffer, PROXY_MAX_ADDRESS_STRING_LENGTH, "%s", "NONE" );
        return buffer;
    }
}

bool proxy_address_equal( const proxy_address_t * a, const proxy_address_t * b )
{
    assert( a );
    assert( b );

    if ( a->type != b->type )
        return false;

    if ( a->type == PROXY_ADDRESS_IPV4 )
    {
        if ( a->port != b->port )
            return false;

        for ( int i = 0; i < 4; ++i )
        {
            if ( a->data.ipv4[i] != b->data.ipv4[i] )
                return false;
        }
    }
    else if ( a->type == PROXY_ADDRESS_IPV6 )
    {
        if ( a->port != b->port )
            return false;

        for ( int i = 0; i < 8; ++i )
        {
            if ( a->data.ipv6[i] != b->data.ipv6[i] )
                return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------

struct proxy_thread_data_t
{
	int thread_number;
	proxy_platform_thread_t * thread;
	proxy_platform_socket_t * socket;
	// ...
};

static proxy_platform_thread_return_t PROXY_PLATFORM_THREAD_FUNC proxy_thread_function( void * data )
{
	proxy_thread_data_t * thread_data = (proxy_thread_data_t*) data;

	printf( "thread %d started\n", thread_data->thread_number );

    thread_data->socket = proxy_platform_socket_create( &config.bind_address, PROXY_PLATFORM_SOCKET_BLOCKING, 0.1f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

    if ( !thread_data->socket )
    {
    	printf( "error: could not create socket\n" );
    	exit(1);
    }

	while ( true )
	{
		uint8_t buffer[config.max_packet_size];

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, buffer, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;

		if ( packet_bytes == 0 )
			continue;

		proxy_platform_socket_send_packet( thread_data->socket, &from, buffer, packet_bytes );
	}

	proxy_platform_socket_destroy( thread_data->socket );

	printf( "thread %d stopped\n", thread_data->thread_number );	

	fflush( stdout );

    PROXY_PLATFORM_THREAD_RETURN();
}

// ---------------------------------------------------------------------

static volatile int quit = 0;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

int main()
{
	signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

	printf( "network next proxy\n" );

    if ( !proxy_init() )
    {
        printf( "error: failed to initialize proxy\n" );
        exit(1);
    }

    proxy_thread_data_t * thread_data[config.num_threads];

	for ( int i = 0; i < config.num_threads; i++ )
	{
		thread_data[i] = (proxy_thread_data_t*) malloc( config.thread_data_bytes );
		if ( !thread_data[i] )
		{
			printf( "error: could not allocate thread data\n" );
			exit(1);
		}

		thread_data[i]->thread_number = i;

	    thread_data[i]->thread = proxy_platform_thread_create( proxy_thread_function, thread_data[i] );
	    if ( !thread_data[i]->thread )
	    {
	        printf( "error: failed to create thread\n" );
	        exit(1);
	    }
	}

	while ( !quit )
	{
		proxy_sleep( 1.0 );
	}

	printf( "\nshutting down...\n" );

	printf( "closing sockets\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_socket_close( thread_data[i]->socket );
	}

	printf( "joining threads\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_thread_join( thread_data[i]->thread );
	}
    
	printf( "destroying threads\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_thread_destroy( thread_data[i]->thread );
		free( thread_data[i] );
		thread_data[i] = NULL;
	}

	printf( "done.\n" );	

    proxy_term();

    fflush( stdout );

    return 0;
}
