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

#include "proxy_linux.h"

#if PROXY_PLATFORM == PROXY_PLATFORM_LINUX

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <alloca.h>

// ---------------------------------------------------

static double time_start;

bool proxy_platform_init()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    time_start = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;

    return true;
}

void proxy_platform_term()
{
    // ...
}

const char * proxy_platform_getenv( const char * var )
{
    return getenv( var );
}

// ---------------------------------------------------

uint16_t proxy_platform_ntohs( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

uint16_t proxy_platform_htons( uint16_t in )
{
    return (uint16_t)( ( ( in << 8 ) & 0xFF00 ) | ( ( in >> 8 ) & 0x00FF ) );
}

bool proxy_platform_inet_pton4( const char * address_string, uint32_t * address_out )
{
    sockaddr_in sockaddr4;
    bool success = inet_pton( AF_INET, address_string, &sockaddr4.sin_addr ) == 1;
    *address_out = sockaddr4.sin_addr.s_addr;
    return success;
}

bool proxy_platform_inet_pton6( const char * address_string, uint16_t * address_out )
{
    return inet_pton( AF_INET6, address_string, address_out ) == 1;
}

bool proxy_platform_inet_ntop6( const uint16_t * address, char * address_string, size_t address_string_size )
{
    return inet_ntop( AF_INET6, (void*)address, address_string, socklen_t( address_string_size ) ) != NULL;
}

bool proxy_platform_hostname_resolve( const char * hostname, const char * port, proxy_address_t * address )
{
    addrinfo hints;
    memset( &hints, 0, sizeof(hints) );
    addrinfo * result;
    if ( getaddrinfo( hostname, port, &hints, &result ) == 0 )
    {
        if ( result )
        {
            if ( result->ai_addr->sa_family == AF_INET6 )
            {
                sockaddr_in6 * addr_ipv6 = (sockaddr_in6 *)( result->ai_addr );
                address->type = PROXY_ADDRESS_IPV6;
                for ( int i = 0; i < 8; ++i )
                {
                    address->data.ipv6[i] = proxy_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
                }
                address->port = proxy_platform_ntohs( addr_ipv6->sin6_port );
                freeaddrinfo( result );
                return true;
            }
            else if ( result->ai_addr->sa_family == AF_INET )
            {
                sockaddr_in * addr_ipv4 = (sockaddr_in *)( result->ai_addr );
                address->type = PROXY_ADDRESS_IPV4;
                address->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
                address->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
                address->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
                address->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
                address->port = proxy_platform_ntohs( addr_ipv4->sin_port );
                freeaddrinfo( result );
                return true;
            }
            else
            {
                assert( 0 );
                freeaddrinfo( result );
                return false;
            }
        }
    }

    return false;
}

int proxy_platform_id()
{
    return PROXY_PLATFORM_LINUX;
}

// ---------------------------------------------------

double proxy_platform_time()
{
    timespec ts;
    clock_gettime( CLOCK_MONOTONIC_RAW, &ts );
    double current = ts.tv_sec + ( (double) ( ts.tv_nsec ) ) / 1000000000.0;
    return current - time_start;
}

void proxy_platform_sleep( double time )
{
    usleep( (int) ( time * 1000000 ) );
}

// ---------------------------------------------------

void proxy_platform_socket_destroy( proxy_platform_socket_t * socket );

proxy_platform_socket_t * proxy_platform_socket_create( proxy_address_t * address, int socket_type, float timeout_seconds, int send_buffer_size, int receive_buffer_size )
{
    assert( address );
    assert( address->type != PROXY_ADDRESS_NONE );

    proxy_platform_socket_t * socket = (proxy_platform_socket_t*) malloc( sizeof( proxy_platform_socket_t ) );

    assert( socket );

    // create socket

    socket->type = socket_type;

    socket->handle = ::socket( ( address->type == PROXY_ADDRESS_IPV6 ) ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP );

    if ( socket->handle < 0 )
    {
        proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to create socket" );
        return NULL;
    }

    // force IPv6 only if necessary

    if ( address->type == PROXY_ADDRESS_IPV6 )
    {
        int yes = 1;
        if ( setsockopt( socket->handle, IPPROTO_IPV6, IPV6_V6ONLY, (char*)( &yes ), sizeof( yes ) ) != 0 )
        {
            proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to set socket ipv6 only" );
            proxy_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // increase socket send and receive buffer sizes

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_SNDBUF, (char*)( &send_buffer_size ), sizeof( int ) ) != 0 )
    {
        proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to set socket send buffer size" );
        return NULL;
    }

    if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVBUF, (char*)( &receive_buffer_size ), sizeof( int ) ) != 0 )
    {
        proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to set socket receive buffer size" );
        proxy_platform_socket_destroy( socket );
        return NULL;
    }

    // bind to port

    if ( address->type == PROXY_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( sockaddr_in6 ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = proxy_platform_htons( address->data.ipv6[i] );
        }
        socket_address.sin6_port = proxy_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to bind socket (ipv6)" );
            proxy_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        sockaddr_in socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin_family = AF_INET;
        socket_address.sin_addr.s_addr = ( ( (uint32_t) address->data.ipv4[0] ) )      | 
                                         ( ( (uint32_t) address->data.ipv4[1] ) << 8 )  | 
                                         ( ( (uint32_t) address->data.ipv4[2] ) << 16 ) | 
                                         ( ( (uint32_t) address->data.ipv4[3] ) << 24 );
        socket_address.sin_port = proxy_platform_htons( address->port );

        if ( bind( socket->handle, (sockaddr*) &socket_address, sizeof( socket_address ) ) < 0 )
        {
            proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to bind socket (ipv4)" );
            proxy_platform_socket_destroy( socket );
            return NULL;
        }
    }

    // if bound to port 0 find the actual port we got

    if ( address->port == 0 )
    {
        if ( address->type == PROXY_ADDRESS_IPV6 )
        {
            sockaddr_in6 sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to get socket port (ipv6)" );
                proxy_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = proxy_platform_ntohs( sin.sin6_port );
        }
        else
        {
            sockaddr_in sin;
            socklen_t len = sizeof( sin );
            if ( getsockname( socket->handle, (sockaddr*)( &sin ), &len ) == -1 )
            {
                proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to get socket port (ipv4)" );
                proxy_platform_socket_destroy( socket );
                return NULL;
            }
            address->port = proxy_platform_ntohs( sin.sin_port );
        }
    }

    // set non-blocking io and receive timeout

    if ( socket_type == PROXY_PLATFORM_SOCKET_NON_BLOCKING )
    {
        // non-blocking
        if ( fcntl( socket->handle, F_SETFL, O_NONBLOCK, 1 ) == -1 )
        {
            proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to set socket to non-blocking" );
            proxy_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else if ( timeout_seconds > 0.0f )
    {
        // blocking with receive timeout
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (int) ( timeout_seconds * 1000000.0f );
        if ( setsockopt( socket->handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) < 0 )
        {
            proxy_printf( PROXY_LOG_LEVEL_ERROR, "failed to set socket receive timeout" );
            proxy_platform_socket_destroy( socket );
            return NULL;
        }
    }
    else
    {
        // blocking with no timeout
    }

    return socket;
}

void proxy_platform_socket_destroy( proxy_platform_socket_t * socket )
{
    assert( socket );
    if ( socket->handle != 0 )
    {
        close( socket->handle );
    }
    free( socket );
}

void proxy_platform_socket_send_packet( proxy_platform_socket_t * socket, const proxy_address_t * to, const void * packet_data, int packet_bytes )
{
    assert( socket );
    assert( to );
    assert( to->type == PROXY_ADDRESS_IPV6 || to->type == PROXY_ADDRESS_IPV4 );
    assert( packet_data );
    assert( packet_bytes > 0 );

    if ( to->type == PROXY_ADDRESS_IPV6 )
    {
        sockaddr_in6 socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin6_family = AF_INET6;
        for ( int i = 0; i < 8; ++i )
        {
            ( (uint16_t*) &socket_address.sin6_addr ) [i] = proxy_platform_htons( to->data.ipv6[i] );
        }
        socket_address.sin6_port = proxy_platform_htons( to->port );
        int result = int( sendto( socket->handle, (char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in6) ) );
        if ( result < 0 )
        {
            char address_string[PROXY_MAX_ADDRESS_STRING_LENGTH];
            proxy_address_to_string( to, address_string );
            proxy_printf( PROXY_LOG_LEVEL_DEBUG, "sendto (%s) failed: %s", address_string, strerror( errno ) );
        }
    }
    else if ( to->type == PROXY_ADDRESS_IPV4 )
    {
        sockaddr_in socket_address;
        memset( &socket_address, 0, sizeof( socket_address ) );
        socket_address.sin_family = AF_INET;
        socket_address.sin_addr.s_addr = ( ( (uint32_t) to->data.ipv4[0] ) )        | 
                                         ( ( (uint32_t) to->data.ipv4[1] ) << 8 )   | 
                                         ( ( (uint32_t) to->data.ipv4[2] ) << 16 )  | 
                                         ( ( (uint32_t) to->data.ipv4[3] ) << 24 );
        socket_address.sin_port = proxy_platform_htons( to->port );
        int result = int( sendto( socket->handle, (const char*)( packet_data ), packet_bytes, 0, (sockaddr*)( &socket_address ), sizeof(sockaddr_in) ) );
        if ( result < 0 )
        {
            char address_string[PROXY_MAX_ADDRESS_STRING_LENGTH];
            proxy_address_to_string( to, address_string );
            proxy_printf( PROXY_LOG_LEVEL_DEBUG, "sendto (%s) failed: %s", address_string, strerror( errno ) );
        }
    }
    else
    {
        proxy_printf( PROXY_LOG_LEVEL_ERROR, "invalid address type. could not send packet" );
    }
}

void proxy_platform_socket_send_packets( proxy_platform_socket_t * socket, const proxy_address_t * to, void ** packet_data, int * packet_bytes, int num_packets )
{
    assert( socket );
    assert( to );
    assert( packet_data );
    assert( packet_bytes );
    assert( num_packets >= 0 );

    if ( num_packets == 0 )
        return;

    iovec * msg = (iovec*) alloca( sizeof(iovec) * num_packets );

    for ( int i = 0; i < num_packets; ++i )
    {
        msg[i].iov_base = packet_data[i];
        msg[i].iov_len = packet_bytes[i];
    }

    sockaddr_in * socket_address = (sockaddr_in*) alloca( sizeof(sockaddr_in) * num_packets );

    for ( int i = 0; i < num_packets; ++i )
    {
        assert( to[i].type == PROXY_ADDRESS_IPV4 );                 // note: ipv6 not supported
        memset( &socket_address[i], 0, sizeof(sockaddr_in) );
        socket_address[i].sin_family = AF_INET;
        socket_address[i].sin_addr.s_addr = ( ( (uint32_t) to[i].data.ipv4[0] ) )        | 
                                            ( ( (uint32_t) to[i].data.ipv4[1] ) << 8 )   | 
                                            ( ( (uint32_t) to[i].data.ipv4[2] ) << 16 )  | 
                                            ( ( (uint32_t) to[i].data.ipv4[3] ) << 24 );
        socket_address[i].sin_port = proxy_platform_htons( to->port );
    }

    mmsghdr * packet_array = (mmsghdr*) alloca( sizeof(mmsghdr) * num_packets );

    for ( int i = 0; i < num_packets; ++i )
    {
        packet_array[i].msg_hdr.msg_name = &socket_address[i];
        packet_array[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        packet_array[i].msg_hdr.msg_iov = &msg[i];
        packet_array[i].msg_hdr.msg_iovlen = 1;
    }

    int result = sendmmsg( socket->handle, packet_array, num_packets, 0 );
    
    if ( result == -1 )
    {
        proxy_printf( PROXY_LOG_LEVEL_ERROR, "sendmmsg failed to send packets" );
    }
}

int proxy_platform_socket_receive_packet( proxy_platform_socket_t * socket, proxy_address_t * from, void * packet_data, int max_packet_size )
{
    assert( socket );
    assert( from );
    assert( packet_data );
    assert( max_packet_size > 0 );

    sockaddr_storage sockaddr_from;
    socklen_t from_length = sizeof( sockaddr_from );

    int result = int( recvfrom( socket->handle, (char*) packet_data, max_packet_size, socket->type == PROXY_PLATFORM_SOCKET_NON_BLOCKING ? MSG_DONTWAIT : 0, (sockaddr*) &sockaddr_from, &from_length ) );

    if ( result <= 0 )
    {
        if ( errno == EAGAIN || errno == EINTR )
        {
            return 0;
        }

        proxy_printf( PROXY_LOG_LEVEL_DEBUG, "recvfrom failed with error %d", errno );
        
        return -1;
    }

    if ( sockaddr_from.ss_family == AF_INET6 )
    {
        sockaddr_in6 * addr_ipv6 = (sockaddr_in6*) &sockaddr_from;
        from->type = PROXY_ADDRESS_IPV6;
        for ( int i = 0; i < 8; ++i )
        {
            from->data.ipv6[i] = proxy_platform_ntohs( ( (uint16_t*) &addr_ipv6->sin6_addr ) [i] );
        }
        from->port = proxy_platform_ntohs( addr_ipv6->sin6_port );
    }
    else if ( sockaddr_from.ss_family == AF_INET )
    {
        sockaddr_in * addr_ipv4 = (sockaddr_in*) &sockaddr_from;
        from->type = PROXY_ADDRESS_IPV4;
        from->data.ipv4[0] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x000000FF ) );
        from->data.ipv4[1] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x0000FF00 ) >> 8 );
        from->data.ipv4[2] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0x00FF0000 ) >> 16 );
        from->data.ipv4[3] = (uint8_t) ( ( addr_ipv4->sin_addr.s_addr & 0xFF000000 ) >> 24 );
        from->port = proxy_platform_ntohs( addr_ipv4->sin_port );
    }
    else
    {
        assert( 0 );
        return -1;
    }
  
    assert( result >= 0 );

    return result;
}

// ---------------------------------------------------

proxy_platform_thread_t * proxy_platform_thread_create( proxy_platform_thread_func_t * thread_function, void * arg )
{
    proxy_platform_thread_t * thread = (proxy_platform_thread_t*) malloc( sizeof( proxy_platform_thread_t) );

    assert( thread );

    if ( pthread_create( &thread->handle, NULL, thread_function, arg ) != 0 )
    {
        free( thread );
        return NULL;
    }

    return thread;
}

void proxy_platform_thread_join( proxy_platform_thread_t * thread )
{
    assert( thread );
    pthread_join( thread->handle, NULL );
}

void proxy_platform_thread_destroy( proxy_platform_thread_t * thread )
{
    assert( thread );
    free( thread );
}

bool proxy_platform_thread_high_priority( proxy_platform_thread_t * thread )
{
    struct sched_param param;
    param.sched_priority = sched_get_priority_max( SCHED_FIFO );
    return pthread_setschedparam( thread->handle, SCHED_FIFO, &param ) == 0;
}

// ---------------------------------------------------

bool proxy_platform_mutex_create( proxy_platform_mutex_t * mutex )
{
    assert( mutex );

	memset( mutex, 0, sizeof(proxy_platform_mutex_t) );
	
	pthread_mutexattr_t attr;
    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, 0 );
    int result = pthread_mutex_init( &mutex->handle, &attr );
    pthread_mutexattr_destroy( &attr );

    if ( result != 0 )
        return false;

    mutex->ok = true;

    return true;
}

void proxy_platform_mutex_acquire( proxy_platform_mutex_t * mutex )
{
    assert( mutex );
    assert( mutex->ok );
    pthread_mutex_lock( &mutex->handle );
}

void proxy_platform_mutex_release( proxy_platform_mutex_t * mutex )
{
    assert( mutex );
    assert( mutex->ok );
    pthread_mutex_unlock( &mutex->handle );
}

void proxy_platform_mutex_destroy( proxy_platform_mutex_t * mutex )
{
    assert( mutex );
    if ( mutex->ok )
    {
        pthread_mutex_destroy( &mutex->handle );
        memset( mutex, 0, sizeof(proxy_platform_mutex_t) );
    }
}

// ---------------------------------------------------

#else // #if PROXY_PLATFORM == PROXY_PLATFORM_LINUX

int proxy_linux_dummy_symbol = 0;

#endif // #if PROXY_PLATFORM == PROXY_PLATFORM_LINUX
