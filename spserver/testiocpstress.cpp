/*
 * Copyright 2008 Stephen Liu
 * For license terms, see the file COPYING along with this library.
 */

#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>

#include "spgetopt.h"

#include "spwin32port.hpp"

#pragma comment(lib,"ws2_32")
#pragma comment(lib,"mswsock")

static const char * gHost = "127.0.0.1";
static int gPort = 3333;
static int gMsgs = 10;
static int gClients = 10;

static time_t gStartTime = 0;

struct SP_TestEvent {
	enum { eEventRecv, eEventSend };

	OVERLAPPED mOverlapped;
	WSABUF mWsaBuf;
	int mType;
	char mBuffer[ 4096 ];
};

struct SP_TestClient {
	SOCKET mFd;

	SP_TestEvent mRecvEvent;
	SP_TestEvent mSendEvent;

	int mSendMsgs;
	int mRecvMsgs;
};

void showUsage( const char * program )
{
	printf( "Stress Test Tools for spserver example\n" );
	printf( "Usage: %s [-h <host>] [-p <port>] [-c <clients>] [-m <messages>]\n", program );
	printf( "\t-h default is %s\n", gHost );
	printf( "\t-p default is %d\n", gPort );
	printf( "\t-c how many clients, default is %d\n", gClients );
	printf( "\t-m messages per client, default is %d\n", gMsgs );
	printf( "\n" );
}

void close_client( SP_TestClient * client )
{
	if( INVALID_HANDLE_VALUE != (HANDLE)client->mFd ) {
		closesocket( client->mFd );
		client->mFd = (SOCKET)INVALID_HANDLE_VALUE;
		gClients--;
	}
}

void on_read( SP_TestClient * client, SP_TestEvent * event, int bytesTransferred )
{
	for( int i = 0; i < bytesTransferred; i++ ) {
		if( '\n' == event->mBuffer[i] ) client->mRecvMsgs++;
	}

	memset( &( event->mOverlapped ), 0, sizeof( OVERLAPPED ) );
	event->mType = SP_TestEvent::eEventRecv;
	event->mWsaBuf.buf = event->mBuffer;
	event->mWsaBuf.len = sizeof( event->mBuffer );

	DWORD recvBytes = 0, flags = 0;
	if( SOCKET_ERROR == WSARecv( (SOCKET)client->mFd, &( event->mWsaBuf ), 1,
			&recvBytes, &flags, &( event->mOverlapped ), NULL ) ) {
		if( ERROR_IO_PENDING != WSAGetLastError() ) {
			printf( "WSARecv fail, errno %d\n", WSAGetLastError() );
			close_client( client );
		}
	}
}

void on_write( SP_TestClient * client, SP_TestEvent * event, int bytesTransferred )
{
	if( client->mSendMsgs < gMsgs ) {
		client->mSendMsgs++;
		if( client->mSendMsgs >= gMsgs ) {
			snprintf( event->mBuffer, sizeof( event->mBuffer ), "quit\n" );
		} else {
			snprintf( event->mBuffer, sizeof( event->mBuffer ),
				"mail #%d, It's good to see how people hire; "
				"that tells us how to market ourselves to them.\n", client->mSendMsgs );
		}

		event->mType = SP_TestEvent::eEventSend;
		memset( &( event->mOverlapped ), 0, sizeof( OVERLAPPED ) );
		event->mWsaBuf.buf = event->mBuffer;
		event->mWsaBuf.len = strlen( event->mBuffer );

		DWORD sendBytes = 0;

		if( SOCKET_ERROR == WSASend( (SOCKET)client->mFd, &( event->mWsaBuf ), 1,
				&sendBytes, 0,	&( event->mOverlapped ), NULL ) ) {
			if( ERROR_IO_PENDING != WSAGetLastError() ) {
				printf( "WSASend fail, errno %d\n", WSAGetLastError() );
				close_client( client );
			}
		}
	} else {
		// do nothing
	}
}

void eventLoop( HANDLE hIocp )
{
	DWORD bytesTransferred = 0;
	SP_TestClient * client = NULL;
	SP_TestEvent * event = NULL;

	BOOL isSuccess = GetQueuedCompletionStatus( hIocp, &bytesTransferred,
			(DWORD*)&client, (OVERLAPPED**)&event, 100 );
	DWORD lastError = WSAGetLastError();

	if( ! isSuccess ) {
		if( NULL != client ) close_client( client );
		return;
	}

	if( 0 == bytesTransferred ) {
		close_client( client );
		return;
	}

	if( SP_TestEvent::eEventRecv == event->mType ) {
		on_read( client, event, bytesTransferred );
		return;
	}

	if( SP_TestEvent::eEventSend == event->mType ) {
		on_write( client, event, bytesTransferred );
		return;
	}
}

int main( int argc, char * argv[] )
{
	extern char *optarg ;
	int c ;

	while( ( c = getopt ( argc, argv, "h:p:c:m:v" )) != EOF ) {
		switch ( c ) {
			case 'h' :
				gHost = optarg;
				break;
			case 'p':
				gPort = atoi( optarg );
				break;
			case 'c' :
				gClients = atoi ( optarg );
				break;
			case 'm' :
				gMsgs = atoi( optarg );
				break;
			case 'v' :
			case '?' :
				showUsage( argv[0] );
				exit( 0 );
		}
	}

#ifdef SIGPIPE
	signal( SIGPIPE, SIG_IGN );
#endif

	WSADATA wsaData;
	
	int err = WSAStartup( MAKEWORD( 2, 0 ), &wsaData );
	if ( err != 0 ) {
		printf( "Couldn't find a useable winsock.dll.\n" );
		return -1;
	}

	HANDLE hIocp = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, 0, 0 );
	if( NULL == hIocp ) {
		printf( "CreateIoCompletionPort failed, errno %d\n", WSAGetLastError() );
		return -1;
	}

	SP_TestClient * clientList = (SP_TestClient*)calloc( gClients, sizeof( SP_TestClient ) );

	struct sockaddr_in sin;
	memset( &sin, 0, sizeof(sin) );
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr( gHost );
	sin.sin_port = htons( gPort );

	int totalClients = gClients, i = 0;

	printf( "Create %d connections to server, it will take some minutes to complete.\n", gClients );
	for( i = 0; i < gClients; i++ ) {
		SP_TestClient * client = clientList + i;
		memset( client, 0, sizeof( SP_TestClient ) );

		client->mFd = socket( AF_INET, SOCK_STREAM, 0 );
		if( client->mFd < 0 ) {
			printf( "socket failed, errno %d, %s\n", errno, strerror( errno ) );
			spwin32_pause_console();
			return -1;
		}

		if( connect( client->mFd, (struct sockaddr *)&sin, sizeof(sin) ) != 0) {
			printf( "connect failed, errno %d, %s\n", errno, strerror( errno ) );
			spwin32_pause_console();
			return -1;
		}

		if( NULL == CreateIoCompletionPort( (HANDLE)client->mFd, hIocp, (DWORD)client, 0 ) ) {
			printf( "CreateIoCompletionPort failed, errno %d\n", WSAGetLastError() );
			return -1;
		}

		if( 0 == ( i % 10 ) ) printf( "." );
	}

	for( i = 0; i < gClients; i++ ) {
		SP_TestClient * client = clientList + i;
		on_read( client, &( client->mRecvEvent ), 0 );
		on_write( client, &( client->mSendEvent ), 0 );
	}

	printf( "\n" );

	time( &gStartTime );

	struct timeval startTime, stopTime;

	sp_gettimeofday( &startTime, NULL );

	time_t lastInfoTime = time( NULL );

	// start event loop until all clients are exit
	while( gClients > 0 ) {
		eventLoop( hIocp );

		if( time( NULL ) - lastInfoTime > 5 ) {
			time( &lastInfoTime );
			printf( "waiting for %d client(s) to exit\n", gClients );
		}
	}

	sp_gettimeofday( &stopTime, NULL );

	double totalTime = (double) ( 1000000 * ( stopTime.tv_sec - startTime.tv_sec )
			+ ( stopTime.tv_usec - startTime.tv_usec ) ) / 1000000;

	// show result
	printf( "\n\nTest result :\n" );
	printf( "Clients : %d, Messages Per Client : %d\n", totalClients, gMsgs );
	printf( "ExecTimes: %.6f seconds\n\n", totalTime );

	printf( "client\tSend\tRecv\n" );
	int totalSend = 0, totalRecv = 0;
	for( i = 0; i < totalClients; i++ ) {
		SP_TestClient * client = clientList + i;

		//printf( "client#%d : %d\t%d\n", i, client->mSendMsgs, client->mRecvMsgs );

		totalSend += client->mSendMsgs;
		totalRecv += client->mRecvMsgs;

		if( INVALID_HANDLE_VALUE != (HANDLE)client->mFd ) {
			closesocket( client->mFd );
		}
	}

	printf( "total   : %d\t%d\n", totalSend, totalRecv );
	printf( "average : %.0f/s\t%.0f/s\n", totalSend / totalTime, totalRecv / totalTime );

	free( clientList );

	CloseHandle( hIocp );

	spwin32_pause_console();

	return 0;
}

