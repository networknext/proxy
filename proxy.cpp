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
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include "next.h"

const char * next_bind_address = "0.0.0.0:60000";
const char * next_public_address = "127.0.0.1:60000";
const char * next_datacenter = "local";
const char * next_backend_hostname = "127.0.0.1:40000";
const char * next_customer_private_key = "leN7D7+9vr3TEZexVmvbYzdH1hbpwBvioc6y1c9Dhwr4ZaTkEWyX2Li5Ph/UFrw8QS8hAD9SQZkuVP6x14tEcqxWppmrvbdn";

// todo: doubling up on port numbers below is super dumb. drive this all from env vars

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
const char * proxy_address = "10.128.0.7:40000";		// google cloud
const char * server_address = "10.128.0.3:40000";		// google cloud
const int proxy_port = 40000;
const int server_port = 40000;
#else
const char * proxy_address = "127.0.0.1:65000";			// local testing
const char * server_address = "127.0.0.1:65001";		// local testing
const int proxy_port = 65000;
const int server_port = 65001;
#endif

// ---------------------------------------------------------------------

#define NEXT_PASSTHROUGH_PACKET                                         0
#define NEXT_DIRECT_PACKET                                              1
#define NEXT_DIRECT_PING_PACKET                                         2
#define NEXT_DIRECT_PONG_PACKET                                         3
#define NEXT_UPGRADE_REQUEST_PACKET                                     4
#define NEXT_UPGRADE_RESPONSE_PACKET                                    5
#define NEXT_UPGRADE_CONFIRM_PACKET                                     6
#define NEXT_OLD_RELAY_PING_PACKET                                      7
#define NEXT_OLD_RELAY_PONG_PACKET                                      8
#define NEXT_ROUTE_REQUEST_PACKET                                       9
#define NEXT_ROUTE_RESPONSE_PACKET                                     10
#define NEXT_CLIENT_TO_SERVER_PACKET                                   11
#define NEXT_SERVER_TO_CLIENT_PACKET                                   12
#define NEXT_PING_PACKET                                               13
#define NEXT_PONG_PACKET                                               14
#define NEXT_CONTINUE_REQUEST_PACKET                                   15
#define NEXT_CONTINUE_RESPONSE_PACKET                                  16
#define NEXT_CLIENT_STATS_PACKET                                       17
#define NEXT_ROUTE_UPDATE_PACKET                                       18
#define NEXT_ROUTE_UPDATE_ACK_PACKET                                   19
#define NEXT_RELAY_PING_PACKET                                         20
#define NEXT_RELAY_PONG_PACKET                                         21
#define NEXT_FORWARD_PACKET_TO_CLIENT                                 254

#define debug_printf printf
//#define debug_printf(...) ((void)0)

static volatile int quit = 0;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

// ---------------------------------------------------------------------

extern bool proxy_platform_init();

extern void proxy_platform_term();

extern int proxy_platform_num_cores();

extern const char * proxy_platform_getenv( const char * );

// ---------------------------------------------------------------------

void proxy_read_int_env( const char * env, int * value )
{
	assert( env );
	assert( value );
	const char * env_string = proxy_platform_getenv( env );
	if ( env_string )
	{
		*value = atoi( env_string );
	}
}

void proxy_read_address_env( const char * env, proxy_address_t * address )
{
	assert( env );
	assert( address );
	const char * env_string = proxy_platform_getenv( env );
	if ( env_string )
	{
		proxy_address_t env_address;
		if ( proxy_address_parse( &env_address, env_string ) )
		{
			*address = env_address;
		}
	}
}

struct proxy_config_t
{
	int num_threads;
	int num_slots_per_thread;
	int slot_base_port;
	int next_base_port;
	int max_packet_size;
	int proxy_thread_data_bytes;
	int slot_thread_data_bytes;
	int next_thread_data_bytes;	
    int slot_timeout_seconds;
    int socket_send_buffer_size;
    int socket_receive_buffer_size;
    proxy_address_t slot_bind_address;
	proxy_address_t server_bind_address;
	proxy_address_t proxy_bind_address;
	proxy_address_t next_bind_address;
	proxy_address_t proxy_address;
	proxy_address_t server_address;
	proxy_address_t next_address;
	proxy_address_t next_local_address;
};

static proxy_config_t config;

bool proxy_init()
{
	if ( !proxy_platform_init() )
		return false;

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
	config.num_threads = 16;
	config.num_slots_per_thread = 1000;
#else
	config.num_threads = 1;
	config.num_slots_per_thread = 10;
#endif

	config.max_packet_size = 1500;

	config.proxy_thread_data_bytes = 1 * 1024 * 1024;
	config.slot_thread_data_bytes = 1 * 1024 * 1024;
	config.next_thread_data_bytes = 1 * 1024 * 1024;

	config.slot_timeout_seconds = 60;

	config.slot_base_port = 10000;

	memset( &config.slot_bind_address, 0, sizeof(proxy_address_t) );
	config.slot_bind_address.type = PROXY_ADDRESS_IPV4;

	memset( &config.proxy_bind_address, 0, sizeof(proxy_address_t) );
	config.proxy_bind_address.type = PROXY_ADDRESS_IPV4;
	config.proxy_bind_address.port = proxy_port;

	memset( &config.server_bind_address, 0, sizeof(proxy_address_t) );
	config.server_bind_address.type = PROXY_ADDRESS_IPV4;
	config.server_bind_address.port = server_port;

	proxy_address_parse( &config.next_bind_address, next_bind_address );

	proxy_address_parse( &config.proxy_address, proxy_address );
	proxy_address_parse( &config.server_address, server_address );
	proxy_address_parse( &config.next_address, next_public_address );

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
	config.socket_send_buffer_size = 10000000;
	config.socket_receive_buffer_size = 10000000;
#else 
	config.socket_send_buffer_size = 1000000;
	config.socket_receive_buffer_size = 1000000;
#endif

	// env var overrides

	proxy_read_int_env( "NUM_THREADS", &config.num_threads );
	proxy_read_int_env( "NUM_SLOTS_PER_THREAD", &config.num_slots_per_thread );

	proxy_read_address_env( "PROXY_ADDRESS", &config.proxy_address );
	proxy_read_address_env( "SERVER_ADDRESS", &config.server_address );
	proxy_read_address_env( "NEXT_ADDRESS", &config.next_address );

	proxy_read_address_env( "PROXY_BIND_ADDRESS", &config.proxy_bind_address );
	proxy_read_address_env( "SERVER_BIND_ADDRESS", &config.server_bind_address );
	proxy_read_address_env( "NEXT_BIND_ADDRESS", &config.next_bind_address );

	// dependent vars

	config.next_local_address.type = PROXY_ADDRESS_IPV4;
	config.next_local_address.data.ipv4[0] = 127;
	config.next_local_address.data.ipv4[1] = 0;
	config.next_local_address.data.ipv4[2] = 0;
	config.next_local_address.data.ipv4[3] = 1;
	config.next_local_address.port = config.next_address.port;

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

extern proxy_platform_socket_t * proxy_platform_socket_create( proxy_address_t * address, uint32_t socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size );

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

extern bool proxy_platform_thread_affinity( proxy_platform_thread_t * thread, int core );

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

typedef uint64_t proxy_fnv_t;

void proxy_fnv_init( proxy_fnv_t * fnv )
{
    *fnv = 0xCBF29CE484222325;
}

void proxy_fnv_write( proxy_fnv_t * fnv, const uint8_t * data, size_t size )
{
    for ( size_t i = 0; i < size; i++ )
    {
        (*fnv) ^= data[i];
        (*fnv) *= 0x00000100000001B3;
    }
}

uint64_t proxy_fnv_finalize( proxy_fnv_t * fnv )
{
    return *fnv;
}

uint64_t hash_address( const proxy_address_t * key )
{
	assert( key );
    proxy_fnv_t fnv;
    proxy_fnv_init( &fnv );
    proxy_fnv_write( &fnv, (const uint8_t*) &key->port, 2 );
    proxy_fnv_write( &fnv, (const uint8_t*) &key->data.ipv4, 4 );
    return proxy_fnv_finalize( &fnv );
}

// ---------------------------------------------------------------------

static void proxy_generate_pittle( uint8_t * output, const uint8_t * from_address, int from_address_bytes, uint16_t from_port, const uint8_t * to_address, int to_address_bytes, uint16_t to_port, int packet_length )
{
    assert( output );
    assert( from_address );
    assert( from_address_bytes > 0 );
    assert( to_address );
    assert( to_address_bytes >= 0 );
    assert( packet_length > 0 );
#if PROXY_BIG_ENDIAN
    proxy_bswap( from_port );
    proxy_bswap( to_port );
    proxy_bswap( packet_length );
#endif // #if PROXY_BIG_ENDIAN
    uint16_t sum = 0;
    for ( int i = 0; i < from_address_bytes; ++i ) { sum += uint8_t(from_address[i]); }
    const char * from_port_data = (const char*) &from_port;
    sum += uint8_t(from_port_data[0]);
    sum += uint8_t(from_port_data[1]);
    for ( int i = 0; i < to_address_bytes; ++i ) { sum += uint8_t(to_address[i]); }
    const char * to_port_data = (const char*) &to_port;
    sum += uint8_t(to_port_data[0]);
    sum += uint8_t(to_port_data[1]);
    const char * packet_length_data = (const char*) &packet_length;
    sum += uint8_t(packet_length_data[0]);
    sum += uint8_t(packet_length_data[1]);
    sum += uint8_t(packet_length_data[2]);
    sum += uint8_t(packet_length_data[3]);
#if PROXY_BIG_ENDIAN
    proxy_bswap( sum );
#endif // #if PROXY_BIG_ENDIAN
    const char * sum_data = (const char*) &sum;
    output[0] = 1 | ( uint8_t(sum_data[0]) ^ uint8_t(sum_data[1]) ^ 193 );
    output[1] = 1 | ( ( 255 - output[0] ) ^ 113 );
}

static void proxy_generate_chonkle( uint8_t * output, const uint8_t * magic, const uint8_t * from_address, int from_address_bytes, uint16_t from_port, const uint8_t * to_address, int to_address_bytes, uint16_t to_port, int packet_length )
{
    assert( output );
    assert( magic );
    assert( from_address );
    assert( from_address_bytes >= 0 );
    assert( to_address );
    assert( to_address_bytes >= 0 );
    assert( packet_length > 0 );
#if PROXY_BIG_ENDIAN
    proxy_bswap( from_port );
    proxy_bswap( to_port );
    proxy_bswap( packet_length );
#endif // #if PROXY_BIG_ENDIAN
    proxy_fnv_t fnv;
    proxy_fnv_init( &fnv );
    proxy_fnv_write( &fnv, magic, 8 );
    proxy_fnv_write( &fnv, from_address, from_address_bytes );
    proxy_fnv_write( &fnv, (const uint8_t*) &from_port, 2 );
    proxy_fnv_write( &fnv, to_address, to_address_bytes );
    proxy_fnv_write( &fnv, (const uint8_t*) &to_port, 2 );
    proxy_fnv_write( &fnv, (const uint8_t*) &packet_length, 4 );
    uint64_t hash = proxy_fnv_finalize( &fnv );
#if PROXY_BIG_ENDIAN
    proxy_bswap( hash );
#endif // #if PROXY_BIG_ENDIAN
    const char * data = (const char*) &hash;
    output[0] = ( ( data[6] & 0xC0 ) >> 6 ) + 42;
    output[1] = ( data[3] & 0x1F ) + 200;
    output[2] = ( ( data[2] & 0xFC ) >> 2 ) + 5;
    output[3] = data[0];
    output[4] = ( data[2] & 0x03 ) + 78;
    output[5] = ( data[4] & 0x7F ) + 96;
    output[6] = ( ( data[1] & 0xFC ) >> 2 ) + 100;
    if ( ( data[7] & 1 ) == 0 ) { output[7] = 79; } else { output[7] = 7; }
    if ( ( data[4] & 0x80 ) == 0 ) { output[8] = 37; } else { output[8] = 83; }
    output[9] = ( data[5] & 0x07 ) + 124;
    output[10] = ( ( data[1] & 0xE0 ) >> 5 ) + 175;
    output[11] = ( data[6] & 0x3F ) + 33;
    const int value = ( data[1] & 0x03 );
    if ( value == 0 ) { output[12] = 97; } else if ( value == 1 ) { output[12] = 5; } else if ( value == 2 ) { output[12] = 43; } else { output[12] = 13; }
    output[13] = ( ( data[5] & 0xF8 ) >> 3 ) + 210;
    output[14] = ( ( data[7] & 0xFE ) >> 1 ) + 17;
}

bool proxy_basic_packet_filter( const uint8_t * data, int packet_length )
{
    if ( packet_length == 0 )
        return false;

    if ( data[0] == 0 )
        return true;

    if ( packet_length < 18 )
        return false;

    if ( data[0] < 0x01 || data[0] > 0x63 )
        return false;

    if ( data[1] < 0x2A || data[1] > 0x2D )
        return false;

    if ( data[2] < 0xC8 || data[2] > 0xE7 )
        return false;

    if ( data[3] < 0x05 || data[3] > 0x44 )
        return false;

    if ( data[5] < 0x4E || data[5] > 0x51 )
        return false;

    if ( data[6] < 0x60 || data[6] > 0xDF )
        return false;

    if ( data[7] < 0x64 || data[7] > 0xE3 )
        return false;

    if ( data[8] != 0x07 && data[8] != 0x4F )
        return false;

    if ( data[9] != 0x25 && data[9] != 0x53 )
        return false;

    if ( data[10] < 0x7C || data[10] > 0x83 )
        return false;

    if ( data[11] < 0xAF || data[11] > 0xB6 )
        return false;

    if ( data[12] < 0x21 || data[12] > 0x60 )
        return false;

    if ( data[13] != 0x61 && data[13] != 0x05 && data[13] != 0x2B && data[13] != 0x0D )
        return false;

    if ( data[14] < 0xD2 || data[14] > 0xF1 )
        return false;

    if ( data[15] < 0x11 || data[15] > 0x90 )
        return false;

    return true;
}

void proxy_address_data( const proxy_address_t * address, uint8_t * address_data, int * address_bytes, uint16_t * address_port )
{
    assert( address );
    if ( address->type == PROXY_ADDRESS_IPV4 )
    {
        address_data[0] = address->data.ipv4[0];
        address_data[1] = address->data.ipv4[1];
        address_data[2] = address->data.ipv4[2];
        address_data[3] = address->data.ipv4[3];
        *address_bytes = 4;
    }
    else if ( address->type == PROXY_ADDRESS_IPV6 )
    {
        for ( int i = 0; i < 8; ++i )
        {
            address_data[i*2]   = address->data.ipv6[i] >> 8;
            address_data[i*2+1] = address->data.ipv6[i] & 0xFF;
        }
        *address_bytes = 16;
    }
    else
    {
        *address_bytes = 0;
    }
    *address_port = address->port;
}

bool proxy_advanced_packet_filter( const uint8_t * data, const uint8_t * magic, const uint8_t * from_address, int from_address_bytes, uint16_t from_port, const uint8_t * to_address, int to_address_bytes, uint16_t to_port, int packet_length )
{
    if ( data[0] == 0 )
        return true;
    if ( packet_length < 18 )
        return false;
    uint8_t a[15];
    uint8_t b[2];
    proxy_generate_chonkle( a, magic, from_address, from_address_bytes, from_port, to_address, to_address_bytes, to_port, packet_length );
    proxy_generate_pittle( b, from_address, from_address_bytes, from_port, to_address, to_address_bytes, to_port, packet_length );
    if ( memcmp( a, data + 1, 15 ) != 0 )
        return false;
    if ( memcmp( b, data + packet_length - 2, 2 ) != 0 )
        return false;
    return true;
}

// ---------------------------------------------------------------------

#define SESSION_TABLE_CAPACITY 4096	// must be power of 2

struct session_table_entry_t 
{
    proxy_address_t key;
    uint64_t sequence;
    int value;
};

struct session_table_t 
{
    session_table_entry_t * entries[2];
    session_table_entry_t * current_entries;
    session_table_entry_t * previous_entries;
    int entries_index;
    uint64_t current_sequence;
    uint64_t previous_sequence;
};

session_table_t * session_table_create() 
{
    session_table_t * table = (session_table_t*) calloc( 1, sizeof(session_table_t) );
    if ( table == NULL) 
        return NULL;
    table->entries[0] = (session_table_entry_t*) calloc( SESSION_TABLE_CAPACITY, sizeof( session_table_entry_t ) );
    table->entries[1] = (session_table_entry_t*) calloc( SESSION_TABLE_CAPACITY, sizeof( session_table_entry_t ) );
    if ( table->entries[0] == NULL || table->entries[1] == NULL ) 
    {
	    free( table->entries[0] );
	    free( table->entries[1] );
        free( table );
        return NULL;
    }
    table->current_entries = table->entries[0];
    table->previous_entries = table->entries[1];
    return table;
}

void session_table_destroy( session_table_t * table )
{
	assert( table );
    free( table->entries[0] );
    free( table->entries[1] );
    free( table );
}

void session_table_swap( session_table_t * table )
{
	assert( table );
	table->entries_index = ( table->entries_index + 1 ) % 2;
	table->previous_entries = table->current_entries;
	table->current_entries = table->entries[table->entries_index];
	table->previous_sequence = table->current_sequence;
	table->current_sequence++;
}

void session_table_insert( session_table_t * table, const proxy_address_t * key, int value )
{
	// IMPORTANT: key must not already exist in table

	assert( table );

    uint64_t hash = hash_address( key );

    const uint64_t mask = (uint64_t)( SESSION_TABLE_CAPACITY - 1 );

    size_t index = (size_t) ( hash & mask );

    while ( table->current_entries[index].key.type == PROXY_ADDRESS_IPV4 && table->current_entries[index].sequence == table->current_sequence ) 
    {
        index ++;
        index &= mask;
	}

    table->current_entries[index].key = *key;
    table->current_entries[index].value = value;
    table->current_entries[index].sequence = table->current_sequence;
}

int session_table_get( session_table_t * table, const proxy_address_t * key ) 
{
	assert( table );

    uint64_t hash = hash_address( key );

    const uint64_t mask = (uint64_t)( SESSION_TABLE_CAPACITY - 1 );

    size_t index = (size_t) ( hash & mask );

    // first search current table

    while ( table->current_entries[index].sequence == table->current_sequence && table->current_entries[index].key.type == PROXY_ADDRESS_IPV4 ) 
    {
        if ( proxy_address_equal( key, &table->current_entries[index].key ) )
        {
        	return table->current_entries[index].value;
        }

        index ++;
        index &= mask;
    }

    // fall back to previous table (these are entries about to time out on next swap if they aren't in current)

    index = (size_t) ( hash & mask );

    while ( table->previous_entries[index].sequence == table->previous_sequence && table->previous_entries[index].key.type == PROXY_ADDRESS_IPV4 ) 
    {
        if ( proxy_address_equal( key, &table->previous_entries[index].key ) )
        {
        	int value = table->previous_entries[index].value;
        	session_table_insert( table, key, value );
        	return value;
        }

        index ++;
        index &= mask;
    }

    return -1;
}

bool session_table_update( session_table_t * table, const proxy_address_t * key, int value )
{
	int existing_value = session_table_get( table, key );
	if ( existing_value >= 0 )
		return false;
	session_table_insert( table, key, value );
	return true;
}

void test_session_table()
{
	printf( "    test_session_table\n" );

	session_table_t * session_table = session_table_create();

	const int NumAddresses = 100;

	// add some addresses

	proxy_address_t address_a[NumAddresses];

	for ( int i = 0; i < NumAddresses; ++i ) 
	{
		char buffer[1024];
		sprintf( buffer, "127.0.0.1:%d", 50000 + i );
		proxy_address_parse( &address_a[i], buffer );
	}

	for ( int i = 0; i < NumAddresses; ++i ) 
	{
		session_table_insert( session_table, &address_a[i], i );
	}

	// verify these addresses are in the table

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == i );
	}

	// swap the table and verify the addesses are still there

	session_table_swap( session_table );

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == i );
	}

	// add a second set of addresses and verify that both sets of addresses are there

	proxy_address_t address_b[NumAddresses];

	for ( int i = 0; i < NumAddresses; ++i ) 
	{
		char buffer[1024];
		sprintf( buffer, "127.0.0.1:%d", 60000 + i );
		proxy_address_parse( &address_b[i], buffer );
		session_table_insert( session_table, &address_b[i], i );
	}

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == i );
		assert( session_table_get( session_table, &address_b[i] ) == i );
	}

	// swap again and both address sets should be there still

	session_table_swap( session_table );

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == i );
		assert( session_table_get( session_table, &address_b[i] ) == i );
	}

	// swap twice and verify that both sets of addresses are gone

	session_table_swap( session_table );
	session_table_swap( session_table );

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == -1 );
		assert( session_table_get( session_table, &address_b[i] ) == -1 );
	}

	// add addresses again and verify they are there

	for ( int i = 0; i < NumAddresses; ++i ) 
	{
		session_table_insert( session_table, &address_a[i], i );
		session_table_insert( session_table, &address_b[i], i );
	}

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == i );
		assert( session_table_get( session_table, &address_b[i] ) == i );
	}

	// swap twice again and verify they are removed

	session_table_swap( session_table );
	session_table_swap( session_table );

	for ( int i = 0; i < NumAddresses; ++i )
	{
		assert( session_table_get( session_table, &address_a[i] ) == -1 );
		assert( session_table_get( session_table, &address_b[i] ) == -1 );
	}

	// clean up

	session_table_destroy( session_table );
}

extern void next_tests();

void run_tests()
{
	next_quiet( true );

    next_config_t next_config;
    next_default_config( &next_config );
    strncpy( next_config.server_backend_hostname, next_backend_hostname, sizeof(next_config.server_backend_hostname) - 1 );
    strncpy( next_config.customer_private_key, next_customer_private_key, sizeof(next_config.customer_private_key) - 1 );
    next_config.high_priority_threads = false;

    if ( next_init( NULL, &next_config ) != NEXT_OK )
    {
        printf( "error: could not initialize network next\n" );
        exit(1);
    }

    next_test();

    test_session_table();

    next_term();
}

// ---------------------------------------------------------------------

struct next_platform_socket_t;

extern void next_platform_socket_send_packet( next_platform_socket_t * socket, const next_address_t * to, const void * packet_data, int packet_bytes );

struct slot_thread_data_t
{
	int thread_number;
	int slot_number;
	proxy_platform_thread_t * thread;
	proxy_platform_socket_t * socket;
	proxy_platform_socket_t ** thread_sockets;
	next_platform_socket_t * next_socket;

	// protected by mutex
	proxy_platform_mutex_t mutex;
	bool allocated;
	bool next;
	proxy_address_t client_address;
};

static proxy_platform_thread_return_t PROXY_PLATFORM_THREAD_FUNC slot_thread_function( void * data )
{
	slot_thread_data_t * thread_data = (slot_thread_data_t*) data;

	debug_printf( "proxy thread %d slot thread %d started\n", thread_data->thread_number, thread_data->slot_number );

	assert( thread_data->socket );

	for ( int i = 0; i < config.num_threads; ++i )
	{
		assert( thread_data->thread_sockets[i] != NULL );
	}

    char string_buffer[1024];
    
    (void) string_buffer;

	while ( true )
	{
		const int prefix = 11;

		uint8_t buffer[prefix + config.max_packet_size];

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, buffer + prefix, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;
		
		if ( packet_bytes == 0 )
			continue;

		uint8_t * packet_data = buffer + prefix;

		proxy_platform_mutex_acquire( &thread_data->mutex );
		bool allocated = thread_data->allocated;
		bool next = thread_data->next;
		proxy_address_t client_address = thread_data->client_address;
		proxy_platform_mutex_release( &thread_data->mutex );

		if ( allocated )
		{
			if ( !next )
			{
				// forward packet to client as passthrough packet

				debug_printf( "proxy thread %d forwarded %d byte packet to client for slot %d (%s)\n", thread_data->thread_number, packet_bytes + 1, thread_data->slot_number, proxy_address_to_string( &client_address, string_buffer ) );

				packet_data -= 1;
				packet_bytes += 1;

	            packet_data[0] = NEXT_PASSTHROUGH_PACKET;
				uint64_t hash = hash_address( &client_address );
				int index = hash % config.num_threads;
				proxy_platform_socket_send_packet( thread_data->thread_sockets[index], &client_address, packet_data, packet_bytes );
			}
			else
			{
				// forward packet to client through next server

				printf( "NEXT_FORWARD_PACKET_TO_CLIENT\n" );

	            buffer[0] = NEXT_FORWARD_PACKET_TO_CLIENT;
	            buffer[1] = client_address.data.ipv4[0];
	            buffer[2] = client_address.data.ipv4[1];
	            buffer[3] = client_address.data.ipv4[2];
	            buffer[4] = client_address.data.ipv4[3];
	            buffer[5] = uint8_t( client_address.port >> 8 );
	            buffer[6] = uint8_t( client_address.port );
	            buffer[7] = uint8_t( thread_data->thread_number >> 8 );
	            buffer[8] = uint8_t( thread_data->thread_number );
	            buffer[9] = uint8_t( thread_data->slot_number >> 8 );
	            buffer[10] = uint8_t( thread_data->slot_number );

	            packet_data = buffer;
	            packet_bytes += prefix;

				next_platform_socket_send_packet( thread_data->next_socket, (next_address_t*) &config.next_local_address, packet_data, packet_bytes );
			}
		}
        else
        {
            debug_printf( "proxy thread %d slot %d received packet from %s, but slot is not allocated\n", thread_data->thread_number, thread_data->slot_number, proxy_address_to_string( &from, string_buffer ) );
        }
	}

	debug_printf( "proxy thread %d slot thread %d stopped\n", thread_data->thread_number, thread_data->slot_number );

	proxy_platform_socket_destroy( thread_data->socket );

	fflush( stdout );

    PROXY_PLATFORM_THREAD_RETURN();
}

// ---------------------------------------------------------------------

struct proxy_slot_data_t
{
	double last_packet_receive_time;
};

struct proxy_thread_data_t
{
	int thread_number;
	session_table_t * session_table;
	proxy_slot_data_t * slot_data;
	proxy_platform_thread_t * thread;
	proxy_platform_socket_t * socket;
	slot_thread_data_t ** slot_thread_data;
	proxy_platform_socket_t ** thread_sockets;
	proxy_platform_socket_t ** slot_sockets;
	next_platform_socket_t * next_socket;
};

extern next_platform_socket_t * next_server_socket( next_server_t * server );

static proxy_platform_thread_return_t PROXY_PLATFORM_THREAD_FUNC proxy_thread_function( void * data )
{
	proxy_thread_data_t * thread_data = (proxy_thread_data_t*) data;

	printf( "proxy thread %d started\n", thread_data->thread_number );

	for ( int i = 0; i < config.num_threads; ++i )
	{
		assert( thread_data->thread_sockets[i] != NULL );
	}

	// create slot threads

	thread_data->slot_thread_data = (slot_thread_data_t**) malloc( sizeof(slot_thread_data_t*) * config.num_slots_per_thread );
	
	for ( int i = 0; i < config.num_slots_per_thread; ++i )
	{
		thread_data->slot_thread_data[i] = (slot_thread_data_t*) calloc( 1, config.slot_thread_data_bytes );
		
		if ( !thread_data->slot_thread_data[i] )
		{
	        printf( "error: could not allocate slot thread data\n" );
			exit(1);
		}

		thread_data->slot_thread_data[i]->thread_number = thread_data->thread_number;
		thread_data->slot_thread_data[i]->thread_sockets = thread_data->thread_sockets;
		thread_data->slot_thread_data[i]->slot_number = i;
		thread_data->slot_thread_data[i]->next_socket = thread_data->next_socket;

		const int global_slot_index = thread_data->thread_number * config.num_slots_per_thread + i;

		thread_data->slot_thread_data[i]->socket = thread_data->slot_sockets[global_slot_index];

		proxy_platform_mutex_create( &thread_data->slot_thread_data[i]->mutex );

	    thread_data->slot_thread_data[i]->thread = proxy_platform_thread_create( slot_thread_function, thread_data->slot_thread_data[i] );

	    if ( !thread_data->slot_thread_data[i]->thread )
	    {
	        printf( "error: failed to create slot thread\n" );
	        exit(1);
	    }
	}

    // process received packets

    char string_buffer[1024];

    (void) string_buffer;

    double last_swap_time = proxy_time();

	while ( true )
	{
		const int prefix = 11;

		uint8_t buffer[prefix + config.max_packet_size];

		uint8_t * packet_data = buffer + prefix;

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, packet_data, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;

		if ( packet_bytes == 0 )
			continue;

		double current_time = proxy_time();

		if ( current_time - last_swap_time > config.slot_timeout_seconds / 2 )
		{
			debug_printf( "proxy thread %d swap\n", thread_data->thread_number );
			session_table_swap( thread_data->session_table );
			last_swap_time = current_time;
		}

		if ( packet_data[0] == 0 )
		{
			// passthrough packet

			int slot = session_table_get( thread_data->session_table, &from );

			if ( slot != -1 )
	  		{
	  			// found existing slot for client
	  			
	  			assert( slot >= 0 );
	  			assert( slot < config.num_slots_per_thread );

				proxy_platform_mutex_acquire( &thread_data->slot_thread_data[slot]->mutex );
				bool allocated = thread_data->slot_thread_data[slot]->allocated;
				proxy_platform_mutex_release( &thread_data->slot_thread_data[slot]->mutex );

				if ( allocated )
				{
					// forward packet to server

					debug_printf( "proxy thread %d forwarded packet to server for slot %d\n", thread_data->thread_number, slot );
					
					proxy_platform_socket_send_packet( thread_data->slot_thread_data[slot]->socket, &config.server_address, packet_data + 1, packet_bytes - 1 );
	                
	                thread_data->slot_data[slot].last_packet_receive_time = proxy_time();
				}
				else
				{
	  				debug_printf( "proxy thread %d dropped packet because slot %d is not allocated?\n", thread_data->thread_number, slot );
				}
	  		}
	  		else
	  		{
	  			// new client. add to slot if possible

	  			int slot = -1;

	  			for ( int i = 0; i < config.num_slots_per_thread; ++i )
	  			{
	                double current_time = proxy_time();

	  				double last_packet_receive_time = thread_data->slot_data[i].last_packet_receive_time;

	  				double time_since_last_packet_receive = current_time - last_packet_receive_time;

	  				if ( time_since_last_packet_receive >= config.slot_timeout_seconds )
	  				{
		  				printf( "proxy thread %d slot %d has new client %s\n", thread_data->thread_number, i, proxy_address_to_string( &from, string_buffer ) );
		  				fflush( stdout );
	  					
	  					slot = i;

						proxy_platform_mutex_acquire( &thread_data->slot_thread_data[slot]->mutex );
						thread_data->slot_thread_data[slot]->allocated = true;
						thread_data->slot_thread_data[slot]->next = false;
						thread_data->slot_thread_data[slot]->client_address = from;

						proxy_platform_mutex_release( &thread_data->slot_thread_data[slot]->mutex );

						session_table_insert( thread_data->session_table, &from, slot );

						thread_data->slot_data[slot].last_packet_receive_time = current_time;

						break;
	  				}
	  			}

	  			if ( slot < 0 )
	  			{
	  				debug_printf( "proxy thread %d dropped packet. no client slot found for address %s\n", thread_data->thread_number, proxy_address_to_string( &from, string_buffer ) );
	  				continue;
	  			}

				// forward packet to server

	            assert( slot >= 0 );
	            assert( slot < config.num_slots_per_thread );

				proxy_platform_socket_send_packet( thread_data->slot_thread_data[slot]->socket, &config.server_address, packet_data + 1, packet_bytes - 1 );

				// send dummy passthrough packet to the next thread so it sees the new client and upgrades it

				debug_printf( "sent dummy packet through to next thread\n" );

	            packet_data[0] = NEXT_PASSTHROUGH_PACKET;
	            packet_data[1] = from.data.ipv4[0];
	            packet_data[2] = from.data.ipv4[1];
	            packet_data[3] = from.data.ipv4[2];
	            packet_data[4] = from.data.ipv4[3];
	            packet_data[5] = uint8_t( from.port >> 8 );
	            packet_data[6] = uint8_t( from.port );
	            packet_data[7] = uint8_t( thread_data->thread_number >> 8 );
	            packet_data[8] = uint8_t( thread_data->thread_number );
	            packet_data[9] = uint8_t( slot >> 8 );
	            packet_data[10] = uint8_t( slot );

				next_platform_socket_send_packet( thread_data->next_socket, (next_address_t*) &config.next_local_address, packet_data, prefix + 1 );
	  		}
		}
		else
		{
			// other packet types

			if ( !proxy_basic_packet_filter( packet_data, packet_bytes ) )
			{
				debug_printf( "basic packet filter dropped packet\n" );
				continue;
			}

            const uint8_t packet_type = packet_data[0];

            switch ( packet_type )
            {
            	case NEXT_PASSTHROUGH_PACKET:
            	case NEXT_DIRECT_PACKET:
            	case NEXT_DIRECT_PING_PACKET:
				case NEXT_UPGRADE_RESPONSE_PACKET:
				case NEXT_ROUTE_REQUEST_PACKET:
				case NEXT_CLIENT_TO_SERVER_PACKET:
				case NEXT_PING_PACKET:
				case NEXT_CONTINUE_REQUEST_PACKET:
				case NEXT_CLIENT_STATS_PACKET:
				case NEXT_ROUTE_UPDATE_ACK_PACKET:
				default:
					break;
            }
            
			int slot = session_table_get( thread_data->session_table, &from );
			if ( slot == -1 )
				continue;

            packet_data = buffer;
            packet_bytes += prefix;

            packet_data[0] = packet_type;
            packet_data[1] = from.data.ipv4[0];
            packet_data[2] = from.data.ipv4[1];
            packet_data[3] = from.data.ipv4[2];
            packet_data[4] = from.data.ipv4[3];
            packet_data[5] = uint8_t( from.port >> 8 );
            packet_data[6] = uint8_t( from.port );
            packet_data[7] = uint8_t( thread_data->thread_number >> 8 );
            packet_data[8] = uint8_t( thread_data->thread_number );
            packet_data[9] = uint8_t( slot >> 8 );
            packet_data[10] = uint8_t( slot );

            // forward packet to next server

			next_platform_socket_send_packet( thread_data->next_socket, (next_address_t*) &config.next_local_address, packet_data, packet_bytes );
		}
	}

	// shutdown

	debug_printf( "proxy thread %d stopping...\n", thread_data->thread_number );	

	proxy_platform_socket_destroy( thread_data->socket );

	for ( int i = 0; i < config.num_slots_per_thread; ++i )
	{
		proxy_platform_socket_close( thread_data->slot_thread_data[i]->socket );
	}

	debug_printf( "proxy thread %d joining slot threads\n", thread_data->thread_number );

	for ( int i = 0; i < config.num_slots_per_thread; ++i )
	{
		proxy_platform_thread_join( thread_data->slot_thread_data[i]->thread );
	}
    
	debug_printf( "proxy thread %d destroying slot threads\n", thread_data->thread_number );

	for ( int i = 0; i < config.num_slots_per_thread; ++i )
	{
		proxy_platform_thread_destroy( thread_data->slot_thread_data[i]->thread );
		proxy_platform_mutex_destroy( &thread_data->slot_thread_data[i]->mutex );
		free( thread_data->slot_thread_data[i] );
		thread_data->slot_thread_data[i] = NULL;
	}

	printf( "proxy thread %d stopped\n", thread_data->thread_number );	

	fflush( stdout );

    PROXY_PLATFORM_THREAD_RETURN();
}

// --------------------------------------------------------------------------------

static proxy_platform_thread_return_t PROXY_PLATFORM_THREAD_FUNC server_thread_function( void * data )
{
	proxy_thread_data_t * thread_data = (proxy_thread_data_t*) data;

	printf( "server thread %d started\n", thread_data->thread_number );

    char string_buffer[1024];

    (void) string_buffer;

	while ( true )
	{
		uint8_t buffer[config.max_packet_size];

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, buffer, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;

		if ( packet_bytes == 0 )
			continue;

		debug_printf( "server thread %d reflected %d byte packet back to %s\n", thread_data->thread_number, packet_bytes, proxy_address_to_string( &from, string_buffer ) );

		proxy_platform_socket_send_packet( thread_data->socket, &from, buffer, packet_bytes );
	}

	proxy_platform_socket_destroy( thread_data->socket );

	printf( "server thread %d stopped\n", thread_data->thread_number );	

	fflush( stdout );

    PROXY_PLATFORM_THREAD_RETURN();
}

// ---------------------------------------------------------------------

struct next_thread_data_t
{
	next_server_t * next_server;
	proxy_platform_socket_t ** thread_sockets;
	proxy_platform_socket_t ** slot_sockets;
	session_table_t * session_table;
	double last_session_table_swap_time;
	proxy_thread_data_t ** proxy_thread_data;
	next_platform_socket_t * next_socket;
};

void next_packet_received( next_server_t * server, void * context, const next_address_t * from, const uint8_t * packet_data, int packet_bytes )
{
    (void) server;
    (void) context;
    (void) from;
    (void) packet_data;
    (void) packet_bytes;

    // not used
    assert( false );
}

void next_packet_receive_callback( void * data, next_address_t * from, uint8_t * packet_data, int * begin, int * end )
{
	next_thread_data_t * thread_data = ( next_thread_data_t*) data;

	assert( thread_data );

	// ignore any packet that's too short to be valid

	const int packet_bytes = *end - *begin;

	if ( packet_bytes <= 11 )
	{
		debug_printf( "packet too small (%d bytes)\n", packet_bytes );
		*begin = 0;
		*end = 0;
		return;
	}

	// special case: forward packet from server to client

	const int prefix = 11;

	if ( packet_data[0] == NEXT_FORWARD_PACKET_TO_CLIENT )
	{
		next_assert( packet_bytes > prefix );

		next_address_t client_address;
		client_address.type = NEXT_ADDRESS_IPV4;
		client_address.data.ipv4[0] = packet_data[1];
		client_address.data.ipv4[1] = packet_data[2];
		client_address.data.ipv4[2] = packet_data[3];
		client_address.data.ipv4[3] = packet_data[4];
		client_address.port = ( uint16_t(packet_data[5]) << 8 ) | ( uint16_t(packet_data[6]) );

		next_server_send_packet( thread_data->next_server, &client_address, packet_data + prefix, packet_bytes - prefix );

		// todo
		char string_buffer[1024];
		printf( "next forwarded %d byte packet to client %s\n", packet_bytes - prefix, next_address_to_string( &client_address, string_buffer ) );
		fflush( stdout );

		return;
	}

	// only modify certain packet types, otherwise return

	const uint8_t packet_type = packet_data[0];

	switch ( packet_type )
	{
		case NEXT_PASSTHROUGH_PACKET:
		case NEXT_DIRECT_PACKET:
		case NEXT_DIRECT_PING_PACKET:
		case NEXT_UPGRADE_RESPONSE_PACKET:
		case NEXT_ROUTE_REQUEST_PACKET:
		case NEXT_CLIENT_TO_SERVER_PACKET:
		case NEXT_PING_PACKET:
		case NEXT_CONTINUE_REQUEST_PACKET:
		case NEXT_CLIENT_STATS_PACKET:
		case NEXT_ROUTE_UPDATE_ACK_PACKET:
			break;

		default:
			return;
	}

	// set the from address to the address that sent the packet to the proxy

	from->type = NEXT_ADDRESS_IPV4;
	from->data.ipv4[0] = packet_data[1];
	from->data.ipv4[1] = packet_data[2];
	from->data.ipv4[2] = packet_data[3];
	from->data.ipv4[3] = packet_data[4];
	from->port = ( uint16_t(packet_data[5]) << 8 ) | ( uint16_t(packet_data[6]) );

	// detect new client sessions and upgrade them

	const int thread_id = ( int(packet_data[7]) << 8 ) | ( int(packet_data[8]) );
	const int slot_id = ( int(packet_data[9]) << 8 ) | ( int(packet_data[10]) );
	const int socket_index = thread_id * config.num_slots_per_thread + slot_id;

	assert( thread_data->session_table );

	if ( session_table_update( thread_data->session_table, (proxy_address_t*) from, socket_index ) )
	{
		// new session

		if ( next_server_ready( thread_data->next_server ) )
		{
			char buffer[1024];
			const char * address_string = next_address_to_string( from, buffer );
			uint64_t session_id = next_server_upgrade_session( thread_data->next_server, from, address_string );
			printf( "next thread upgraded client %s to session %" PRIx64 "\n", address_string, session_id );
			fflush( stdout );
		}
	}

	// swap the session table double buffer every n seconds

	const double current_time = next_time();

	const double time_since_last_swap = current_time - thread_data->last_session_table_swap_time;

	if ( time_since_last_swap >= config.slot_timeout_seconds )
	{
		debug_printf( "next thread swap\n" );
		session_table_swap( thread_data->session_table );
		thread_data->last_session_table_swap_time = current_time;
	}

	// if it is a passthrough packet, stop here. these are just sent to the next server to upgrade sessions

	if ( packet_type == NEXT_PASSTHROUGH_PACKET )
	{
		*begin = 0;
		*end = 0;
		return;
	}

	// adjust begin index forward. the next server will process this packet

	*begin += 11;
}

extern const uint8_t * next_server_magic( next_server_t * server );
extern void next_address_data( const next_address_t * address, uint8_t * address_data, int * address_bytes, uint16_t * address_port );
extern bool next_basic_packet_filter( const uint8_t * data, int packet_length );
extern void next_generate_chonkle( uint8_t * output, const uint8_t * magic, const uint8_t * from_address, int from_address_bytes, uint16_t from_port, const uint8_t * to_address, int to_address_bytes, uint16_t to_port, int packet_length );
extern void next_generate_pittle( uint8_t * output, const uint8_t * from_address, int from_address_bytes, uint16_t from_port, const uint8_t * to_address, int to_address_bytes, uint16_t to_port, int packet_length );

int next_send_packet_to_address_callback( void * data, const next_address_t * address, uint8_t * packet_data, int packet_bytes )
{
	next_thread_data_t * thread_data = (next_thread_data_t*) data;

	next_assert( thread_data );

	uint64_t hash = hash_address( (proxy_address_t*) address );

	const int index = hash % config.num_threads;

	if ( packet_data[0] != NEXT_PASSTHROUGH_PACKET )
	{
		// adjust chonkle for outgoing packets. must pass advanced packet filter for relays to accept the packet

	    const uint8_t * magic = next_server_magic( thread_data->next_server );

	    uint8_t from_address_data[32];
	    uint8_t to_address_data[32];
	    uint16_t from_address_port = 0;
	    uint16_t to_address_port = 0;
	    int from_address_bytes = 0;
	    int to_address_bytes;

	    next_address_data( (const next_address_t*) &config.proxy_address, from_address_data, &from_address_bytes, &from_address_port );
	    next_address_data( address, to_address_data, &to_address_bytes, &to_address_port );

	    next_generate_chonkle( packet_data + 1, magic, from_address_data, from_address_bytes, from_address_port, to_address_data, to_address_bytes, to_address_port, packet_bytes );
	    
	    // update pittle for outgoing packets

	    next_generate_pittle( packet_data + packet_bytes - 2, from_address_data, from_address_bytes, from_address_port, to_address_data, to_address_bytes, to_address_port, packet_bytes );

	    // make sure the packet still passes basic filter after modification

		next_assert( next_basic_packet_filter( packet_data, packet_bytes ) );
	}

    // send to the client via the slot packet

	proxy_platform_socket_send_packet( thread_data->thread_sockets[index], (const proxy_address_t*) address, packet_data, packet_bytes );

	return 1;
}

int next_payload_receive_callback( void * data, const next_address_t * client_address, const uint8_t * payload_data, int payload_bytes )
{
	next_thread_data_t * thread_data = (next_thread_data_t*) data;

	next_assert( thread_data );

	int socket_index = session_table_get( thread_data->session_table, (proxy_address_t*) client_address );
	if ( socket_index < 0 )
		return 1;

	proxy_platform_socket_t * socket = thread_data->slot_sockets[socket_index];

	/*
	char buffer[1024];
	printf( "next thread forwarded %d byte packet to server for client %s\n", payload_bytes, next_address_to_string( from, buffer ) );
	*/

	proxy_platform_socket_send_packet( socket, &config.server_address, payload_data, payload_bytes );

	return 1;
}

void next_route_update_callback( void * data, const next_address_t * client_address, NEXT_BOOL next )
{
	next_thread_data_t * thread_data = (next_thread_data_t*) data;

	int index = session_table_get( thread_data->session_table, (proxy_address_t*) client_address );
	if ( index < 0 )
		return;

	const int slot_number = index % config.num_slots_per_thread;
	const int thread_number = index / config.num_threads;

	next_assert( slot_number >= 0 );
	next_assert( slot_number < config.num_slots_per_thread );

	next_assert( thread_number >= 0 );
	next_assert( thread_number < config.num_threads );

	next_assert( thread_data->proxy_thread_data );

	proxy_thread_data_t * proxy_thread_data = thread_data->proxy_thread_data[thread_number];

	slot_thread_data_t * slot_thread_data = proxy_thread_data->slot_thread_data[slot_number];

	proxy_platform_mutex_acquire( &slot_thread_data->mutex );
	slot_thread_data->next = next ? true : false;
	proxy_platform_mutex_release( &slot_thread_data->mutex );
}

static proxy_platform_thread_return_t PROXY_PLATFORM_THREAD_FUNC next_thread_function( void * data )
{
	next_thread_data_t * thread_data = (next_thread_data_t*) data;

	next_assert( thread_data );
	next_assert( thread_data->next_server );

	printf( "next thread started\n" );

    while ( !quit )
    {
        next_server_update( thread_data->next_server );

        next_sleep( 1.0 / 60.0 );
    }

    next_server_flush( thread_data->next_server );
    
    next_server_destroy( thread_data->next_server );
    
	printf( "next thread stopped\n" );

	fflush( stdout );

    PROXY_PLATFORM_THREAD_RETURN();
}

// ---------------------------------------------------------------------

int main( int argc, char * argv[] )
{
    bool server_mode = ( argc == 2 ) && strcmp( argv[1], "server" ) == 0;
    
    bool test_mode = (argc == 2 ) && strcmp( argv[1], "test" ) == 0;

    const char * mode_env = proxy_platform_getenv( "MODE" );
    if ( mode_env && strcmp( mode_env, "server" ) == 0 )
    {
    	server_mode = true;
    }

    if ( server_mode )
    {
		printf( "server mode\n" );
    }
    else if ( test_mode )
    {
		printf( "\nrunning tests:\n\n" );
    	run_tests();
    	printf( "\n" );
    	fflush( stdout );
    	return 0;
    }
    else
    {
		printf( "network next proxy\n" );    	
    }

 	signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    if ( !proxy_init() )
    {
        printf( "error: failed to initialize\n" );
        exit(1);
    }

    // create slot sockets in a flat array

    const int num_slot_sockets = config.num_threads * config.num_slots_per_thread;

    proxy_platform_socket_t * slot_sockets[num_slot_sockets];

    memset( slot_sockets, 0, sizeof(proxy_platform_socket_t*) );

    if ( !server_mode )
    {
    	printf( "creating %d slot sockets on ports [%d,%d]\n", num_slot_sockets, config.slot_base_port, config.slot_base_port + num_slot_sockets - 1 );

	    for ( int i = 0; i < num_slot_sockets; ++i )
	    {
			proxy_address_t bind_address = config.slot_bind_address;

			bind_address.port = config.slot_base_port + i;

		    slot_sockets[i] = proxy_platform_socket_create( &bind_address, 0, 0.1f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !slot_sockets[i] )
		    {
		    	printf( "error: could not create slot socket\n" );
		    	exit(1);
		    }
		}
	}

    // create thread sockets prior to actually creating threads, to avoid race conditions

    if ( !server_mode )
    {
    	printf( "creating %d proxy sockets on port %d\n", config.num_threads, config.proxy_bind_address.port );
    }
    else
    {
    	printf( "creating %d server sockets on port %d\n", config.num_threads, config.server_bind_address.port );
    }

    proxy_thread_data_t * thread_data[config.num_threads];

    proxy_platform_socket_t * thread_sockets[config.num_threads];

    for ( int i = 0;i < config.num_threads; ++i )
    {
		thread_data[i] = (proxy_thread_data_t*) calloc( 1, config.proxy_thread_data_bytes );
		if ( !thread_data[i] )
		{
			printf( "error: could not allocate thread data\n" );
			exit(1);
		}

		thread_data[i]->thread_number = i;

		thread_data[i]->session_table = session_table_create();

		thread_data[i]->slot_data = (proxy_slot_data_t*) calloc( config.num_slots_per_thread, sizeof( proxy_slot_data_t ) );
		for ( int j = 0; j < config.num_slots_per_thread; ++j )
		{
			thread_data[i]->slot_data[j].last_packet_receive_time = -1000000000.0;
		}

		if ( server_mode )
		{
		    thread_sockets[i] = proxy_platform_socket_create( &config.server_bind_address, PROXY_PLATFORM_SOCKET_REUSE_PORT | PROXY_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !thread_sockets[i] )
		    {
		    	printf( "error: could not create socket\n" );
		    	exit(1);
		    }
		}
		else
		{
		    thread_sockets[i] = proxy_platform_socket_create( &config.proxy_bind_address, PROXY_PLATFORM_SOCKET_REUSE_PORT, 0.1f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !thread_sockets[i] )
		    {
		    	printf( "error: could not create socket\n" );
		    	exit(1);
		    }
		}
    }

    // create next server (manages its own internal socket)

	next_server_t * next_server = NULL;
	next_thread_data_t * next_thread_data = NULL;

	if ( !server_mode )
    {
    	printf( "creating network next server on port %d\n", config.next_address.port );

    	// todo
		// next_quiet( true );

		next_thread_data = (next_thread_data_t*) calloc( 1, config.next_thread_data_bytes );

		if ( !next_thread_data )
		{
			printf( "error: could not create next thread data\n" );
			exit(1);
		}

		next_thread_data->proxy_thread_data = thread_data;

	    next_config_t next_config;
	    next_default_config( &next_config );
	    next_config.force_passthrough_direct = true;
	    strncpy( next_config.server_backend_hostname, next_backend_hostname, sizeof(next_config.server_backend_hostname) - 1 );
	    strncpy( next_config.customer_private_key, next_customer_private_key, sizeof(next_config.customer_private_key) - 1 );

	    if ( next_init( NULL, &next_config ) != NEXT_OK )
	    {
	        printf( "error: could not initialize network next\n" );
	        exit(1);
	    }

	    next_server_callbacks_t callbacks;
	    memset( &callbacks, 0, sizeof(callbacks) );
	    callbacks.packet_receive_callback = next_packet_receive_callback;
		callbacks.packet_receive_callback_data = next_thread_data;
		callbacks.send_packet_to_address_callback = next_send_packet_to_address_callback;
		callbacks.send_packet_to_address_callback_data = next_thread_data;
		callbacks.payload_receive_callback = next_payload_receive_callback;
		callbacks.payload_receive_callback_data = next_thread_data;
		callbacks.route_update_callback = next_route_update_callback;
		callbacks.route_update_callback_data = next_thread_data;

		char public_address[1024];
		char bind_address[1024];

		proxy_address_to_string( &config.next_address, public_address );
		proxy_address_to_string( &config.next_bind_address, bind_address );

	    next_server = next_server_create( NULL, public_address, bind_address, next_datacenter, next_packet_received, &callbacks );

	    if ( next_server == NULL )
	    {
	        printf( "error: failed to create next server\n" );
	        exit(1);
	    }

	    // IMPORTANT: block and wait until server is ready. This means all callbacks are registered and ready to go
	    while ( !quit )
	    {
	    	next_server_update( next_server );
	    	if ( next_server_ready( next_server ) )
	    		break;
	    	next_sleep( 0.1 );
	    }
	}

    // create proxy | server threads

	for ( int i = 0; i < config.num_threads; ++i )
	{
		thread_data[i]->socket = thread_sockets[i];
		thread_data[i]->thread_sockets = thread_sockets;
		thread_data[i]->slot_sockets = slot_sockets;

		if ( !server_mode )
		{
			assert( next_server );
			thread_data[i]->next_socket = next_server_socket( next_server );
			assert( thread_data[i]->next_socket );
		}

	    thread_data[i]->thread = proxy_platform_thread_create( server_mode ? server_thread_function : proxy_thread_function, thread_data[i] );

	    if ( !thread_data[i]->thread )
	    {
	        printf( "error: failed to create thread\n" );
	        exit(1);
	    }
	}

	proxy_platform_thread_t * next_thread = NULL;

	if ( !server_mode )
	{
		// create next thread

		next_thread_data->next_server = next_server;
		next_thread_data->thread_sockets = thread_sockets;
		next_thread_data->slot_sockets = slot_sockets;
		next_thread_data->session_table = session_table_create();
		next_thread_data->last_session_table_swap_time = next_time();

	    next_thread = proxy_platform_thread_create( next_thread_function, next_thread_data );

	    if ( !next_thread )
	    {
	        printf( "error: failed to create thread\n" );
	        exit(1);
	    }
	}

	// wait for CTRL-C

    fflush( stdout );

	while ( !quit )
	{
		proxy_sleep( 1.0 );
	}

	// shut down

	printf( "\nshutting down...\n" );

	debug_printf( "closing sockets\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_socket_close( thread_data[i]->socket );
	}

	debug_printf( "joining threads\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_thread_join( thread_data[i]->thread );
	}

	if ( !server_mode )
	{
		proxy_platform_thread_join( next_thread );
	}
    
	debug_printf( "destroying threads\n" );

	for ( int i = 0; i < config.num_threads; i++ )
	{
		proxy_platform_thread_destroy( thread_data[i]->thread );
		session_table_destroy( thread_data[i]->session_table );
		free( thread_data[i]->slot_data );
		free( thread_data[i] );
		thread_data[i] = NULL;
	}

	if ( !server_mode )
	{
		proxy_platform_thread_destroy( next_thread );
		free( next_thread_data );
	}

	if ( !server_mode )
	{
		next_term();	
	}

    proxy_term();

	printf( "done.\n" );	

    fflush( stdout );

    return 0;
}
