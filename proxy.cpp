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
#include "next.h"

const char * next_bind_address = "0.0.0.0:60000";
const char * next_public_address = "127.0.0.1:60000";
const char * next_datacenter = "local";
const char * next_backend_hostname = "dev5.spacecats.net";
const char * next_customer_private_key = "leN7D7+9vr3TEZexVmvbYzdH1hbpwBvioc6y1c9Dhwr4ZaTkEWyX2Li5Ph/UFrw8QS8hAD9SQZkuVP6x14tEcqxWppmrvbdn";

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
const char * server_address = "10.128.0.7:40000";		// google cloud
const int proxy_port = 40000;
const int server_port = 40000;
#else
const char * server_address = "127.0.0.1:50000";		// local testing
const int proxy_port = 40000;
const int server_port = 50000;
#endif

//#define debug_printf printf
#define debug_printf(...) ((void)0)

static volatile int quit = 0;

void interrupt_handler( int signal )
{
    (void) signal; quit = 1;
}

// ---------------------------------------------------------------------

extern bool proxy_platform_init();

extern void proxy_platform_term();

extern int proxy_platform_num_cores();

struct proxy_config_t
{
	int num_threads;
	int num_slots_per_thread;
	int slot_base_port;
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
	proxy_address_t server_address;
};

static proxy_config_t config;

bool proxy_init()
{
	if ( !proxy_platform_init() )
		return false;

	config.num_threads = 0;

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
	config.num_slots_per_thread = 1000;
#else
	config.num_slots_per_thread = 10;
#endif

	config.max_packet_size = 1500;

	config.proxy_thread_data_bytes = 10 * 1024 * 1024;
	config.slot_thread_data_bytes = 1 * 1024 * 1024;
	config.next_thread_data_bytes = 10 * 1024 * 1024;

	config.slot_timeout_seconds = 60;

	memset( &config.slot_bind_address, 0, sizeof(proxy_address_t) );
	config.slot_bind_address.type = PROXY_ADDRESS_IPV4;

	memset( &config.proxy_bind_address, 0, sizeof(proxy_address_t) );
	config.proxy_bind_address.type = PROXY_ADDRESS_IPV4;
	config.proxy_bind_address.port = proxy_port;

	memset( &config.server_bind_address, 0, sizeof(proxy_address_t) );
	config.server_bind_address.type = PROXY_ADDRESS_IPV4;
	config.server_bind_address.port = server_port;

	proxy_address_parse( &config.server_address, server_address );

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
	config.socket_send_buffer_size = 10000000;
	config.socket_receive_buffer_size = 10000000;
#else 
	config.socket_send_buffer_size = 1000000;
	config.socket_receive_buffer_size = 1000000;
#endif

	if ( config.num_threads <= 0 )
	{
		config.num_threads = proxy_platform_num_cores();

        if ( config.num_threads > 16 )
        {
            config.num_threads = 16;
        }
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

void session_table_insert( session_table_t * table, const proxy_address_t * key, int value )
{
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

void test_session_table()
{
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

// ---------------------------------------------------------------------

struct slot_thread_data_t
{
	int thread_number;
	int slot_number;
	proxy_platform_thread_t * thread;
	proxy_platform_socket_t * socket;
	proxy_platform_socket_t ** thread_sockets;

	// protected by mutex
	proxy_platform_mutex_t mutex;
	bool allocated;
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
		uint8_t buffer[1 + config.max_packet_size];

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, buffer + 1, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;

		if ( packet_bytes == 0 )
			continue;

		proxy_platform_mutex_acquire( &thread_data->mutex );
		bool allocated = thread_data->allocated;
		proxy_address_t client_address = thread_data->client_address;
		proxy_platform_mutex_release( &thread_data->mutex );

		if ( allocated )
		{
			// forward packet to client
			debug_printf( "proxy thread %d forwarded packet to client for slot %d (%s)\n", thread_data->thread_number, thread_data->slot_number, proxy_address_to_string( &client_address, string_buffer ) );
            buffer[0] = 0;
			uint64_t hash = hash_address( &client_address );
			int index = hash % config.num_threads;
			proxy_platform_socket_send_packet( thread_data->thread_sockets[index], &client_address, buffer, packet_bytes + 1 );
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
};

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
		uint8_t buffer[config.max_packet_size];

		proxy_address_t from;

		int packet_bytes = proxy_platform_socket_receive_packet( thread_data->socket, &from, buffer, config.max_packet_size );

		if ( packet_bytes < 0 )
			break;

		if ( packet_bytes == 0 )
			continue;

		double current_time = proxy_time();

		if ( current_time - last_swap_time > config.slot_timeout_seconds / 2 )
		{
			printf( "thread %d swap\n", thread_data->thread_number );
			session_table_swap( thread_data->session_table );
			last_swap_time = current_time;
		}

		if ( buffer[0] == 0 )
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
					proxy_platform_socket_send_packet( thread_data->slot_thread_data[slot]->socket, &config.server_address, buffer + 1, packet_bytes - 1 );
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
		  				printf( "thread %d slot %d has new client %s\n", thread_data->thread_number, i, proxy_address_to_string( &from, string_buffer ) );
	  					
	  					slot = i;

						proxy_platform_mutex_acquire( &thread_data->slot_thread_data[slot]->mutex );
						thread_data->slot_thread_data[slot]->allocated = true;
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

	  			debug_printf( "proxy thread %d forwarded packet to server for slot %d\n", thread_data->thread_number, slot );
				
				proxy_platform_socket_send_packet( thread_data->slot_thread_data[slot]->socket, &config.server_address, buffer + 1, packet_bytes - 1 );
	  		}
		}
		else
		{
			// other packet types: drop for now

			debug_printf( "not passthrough packet\n" );
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
};

void next_packet_received( next_server_t * server, void * context, const next_address_t * from, const uint8_t * packet_data, int packet_bytes )
{
    (void) server;
    (void) context;
    (void) from;
    (void) packet_data;
    (void) packet_bytes;

    // todo

    /*
    next_server_send_packet( server, from, packet_data, packet_bytes );

    // next_printf( NEXT_LOG_LEVEL_INFO, "server received packet from client (%d bytes)", packet_bytes );

    if ( next_server_ready( server ) && !next_server_session_upgraded( server, from ) )
    {
        const char * user_id_string = "12345";
        next_server_upgrade_session( server, from, user_id_string );
    }
    */
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
	signal( SIGINT, interrupt_handler ); signal( SIGTERM, interrupt_handler );

    if ( !proxy_init() )
    {
        printf( "error: failed to initialize\n" );
        exit(1);
    }

    test_session_table();

    const bool server_mode = ( argc == 2 ) && strcmp( argv[1], "server" ) == 0;

    if ( server_mode )
    {
		printf( "network next server\n" );
    }
    else
    {
		printf( "network next proxy\n" );    	
    }

    // create slot sockets in a flat array

    const int num_slot_sockets = config.num_threads * config.num_slots_per_thread;

    proxy_platform_socket_t * slot_sockets[num_slot_sockets];

    memset( slot_sockets, 0, sizeof(proxy_platform_socket_t*) );

    if ( !server_mode )
    {
	    for ( int i = 0; i < num_slot_sockets; ++i )
	    {
			proxy_address_t bind_address = config.slot_bind_address;

			bind_address.port = 5000 + i;

		    slot_sockets[i] = proxy_platform_socket_create( &bind_address, PROXY_PLATFORM_SOCKET_BLOCKING, 0.1f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !slot_sockets[i] )
		    {
		    	printf( "error: could not create slot socket\n" );
		    	exit(1);
		    }
		}
	}

    // create thread sockets prior to actually creating threads, to avoid race conditions

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
		    thread_sockets[i] = proxy_platform_socket_create( &config.server_bind_address, PROXY_PLATFORM_SOCKET_NON_BLOCKING, 0.0f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !thread_sockets[i] )
		    {
		    	printf( "error: could not create socket\n" );
		    	exit(1);
		    }
		}
		else
		{
		    thread_sockets[i] = proxy_platform_socket_create( &config.proxy_bind_address, PROXY_PLATFORM_SOCKET_BLOCKING, 0.1f, config.socket_send_buffer_size, config.socket_receive_buffer_size );

		    if ( !thread_sockets[i] )
		    {
		    	printf( "error: could not create socket\n" );
		    	exit(1);
		    }
		}
    }

    // create network next server (manages its own internal socket)

	next_server_t * next_server = NULL;

    if ( !server_mode )
    {
		next_quiet( true );

	    next_config_t next_config;
	    next_default_config( &next_config );
	    strncpy( next_config.server_backend_hostname, next_backend_hostname, sizeof(next_config.server_backend_hostname) - 1 );
	    strncpy( next_config.customer_private_key, next_customer_private_key, sizeof(next_config.customer_private_key) - 1 );

	    if ( next_init( NULL, &next_config ) != NEXT_OK )
	    {
	        printf( "error: could not initialize network next\n" );
	        exit(1);
	    }

	    next_server = next_server_create( NULL, next_public_address, next_bind_address, next_datacenter, next_packet_received, NULL );
	    if ( next_server == NULL )
	    {
	        printf( "error: failed to create next server\n" );
	        exit(1);
	    }
	}

    // create proxy|server threads

	for ( int i = 0; i < config.num_threads; ++i )
	{
		thread_data[i]->socket = thread_sockets[i];
		thread_data[i]->thread_sockets = thread_sockets;
		thread_data[i]->slot_sockets = slot_sockets;

	    thread_data[i]->thread = proxy_platform_thread_create( server_mode ? server_thread_function : proxy_thread_function, thread_data[i] );

	    if ( !thread_data[i]->thread )
	    {
	        printf( "error: failed to create thread\n" );
	        exit(1);
	    }

	    if ( server_mode )
	    {
		    if ( !proxy_platform_thread_affinity( thread_data[i]->thread, i ) )
		    {
		    	printf( "error: failed to set thread affinity to core %d\n", i );
		    	exit(1);
		    }

		    proxy_platform_thread_high_priority( thread_data[i]->thread );
		}
	}

	proxy_platform_thread_t * next_thread = NULL;
	next_thread_data_t * next_thread_data = NULL;

	if ( !server_mode )
	{
		// create next thread

		next_thread_data = (next_thread_data_t*) calloc( 1, config.next_thread_data_bytes );

		next_thread_data->next_server = next_server;
		next_thread_data->thread_sockets = thread_sockets;
		next_thread_data->slot_sockets = slot_sockets;

	    next_thread = proxy_platform_thread_create( next_thread_function, next_thread_data );

	    if ( !next_thread )
	    {
	        printf( "error: failed to create thread\n" );
	        exit(1);
	    }
	}

	// wait for CTRL-C

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
