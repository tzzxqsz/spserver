/*
 * Copyright 2008 Stephen Liu
 * For license terms, see the file COPYING along with this library.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>

#include "spiocpserver.hpp"
#include "spwin32iocp.hpp"
#include "sphandler.hpp"
#include "spsession.hpp"
#include "spexecutor.hpp"
#include "sputils.hpp"
#include "spioutils.hpp"

SP_IocpServer :: SP_IocpServer( const char * bindIP, int port,
		SP_HandlerFactory * handlerFactory )
{
	snprintf( mBindIP, sizeof( mBindIP ), "%s", bindIP );
	mPort = port;
	mIsShutdown = 0;
	mIsRunning = 0;

	mHandlerFactory = handlerFactory;

	mTimeout = 600;
	mMaxThreads = 64;
	mReqQueueSize = 128;
	mMaxConnections = 256;
	mRefusedMsg = strdup( "System busy, try again later." );
}

SP_IocpServer :: ~SP_IocpServer()
{
	if( NULL != mHandlerFactory ) delete mHandlerFactory;
	mHandlerFactory = NULL;

	if( NULL != mRefusedMsg ) free( mRefusedMsg );
	mRefusedMsg = NULL;
}

void SP_IocpServer :: setTimeout( int timeout )
{
	mTimeout = timeout > 0 ? timeout : mTimeout;
}

void SP_IocpServer :: setMaxThreads( int maxThreads )
{
	mMaxThreads = maxThreads > 0 ? maxThreads : mMaxThreads;
}

void SP_IocpServer :: setMaxConnections( int maxConnections )
{
	mMaxConnections = maxConnections > 0 ? maxConnections : mMaxConnections;
}

void SP_IocpServer :: setReqQueueSize( int reqQueueSize, const char * refusedMsg )
{
	mReqQueueSize = reqQueueSize > 0 ? reqQueueSize : mReqQueueSize;

	if( NULL != mRefusedMsg ) free( mRefusedMsg );
	mRefusedMsg = strdup( refusedMsg );
}

void SP_IocpServer :: shutdown()
{
	mIsShutdown = 1;
}

int SP_IocpServer :: isRunning()
{
	return mIsRunning;
}

int SP_IocpServer :: run()
{
	int ret = -1;

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	assert( pthread_attr_setstacksize( &attr, 1024 * 1024 ) == 0 );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );

	pthread_t thread;
	ret = pthread_create( &thread, &attr, reinterpret_cast<void*(*)(void*)>(eventLoop), this );
	pthread_attr_destroy( &attr );
	if( 0 == ret ) {
		sp_syslog( LOG_NOTICE, "Thread #%ld has been created to listen on port [%d]", thread, mPort );
	} else {
		mIsRunning = 0;
		sp_syslog( LOG_WARNING, "Unable to create a thread for TCP server on port [%d], %s",
			mPort, strerror( errno ) ) ;
	}

	return ret;
}

void SP_IocpServer :: runForever()
{
	eventLoop( this );
}

void * SP_IocpServer :: eventLoop( void * arg )
{
	SP_IocpServer * server = (SP_IocpServer*)arg;

	server->mIsRunning = 1;

	server->start();

	server->mIsRunning = 0;

	return NULL;
}

void SP_IocpServer :: sigHandler( int, short, void * arg )
{
	SP_IocpServer * server = (SP_IocpServer*)arg;
	server->shutdown();
}

void SP_IocpServer :: outputCompleted( void * arg )
{
	SP_CompletionHandler * handler = ( SP_CompletionHandler * ) ((void**)arg)[0];
	SP_Message * msg = ( SP_Message * ) ((void**)arg)[ 1 ];

	handler->completionMessage( msg );

	free( arg );
}

void * SP_IocpServer :: acceptThread( void * arg )
{
	DWORD recvBytes = 0;

	SP_IocpAcceptArg_t * acceptArg = (SP_IocpAcceptArg_t*)arg;
	
	for( ; ; ) {
		acceptArg->mClientSocket = (HANDLE)WSASocket( AF_INET, SOCK_STREAM,
				IPPROTO_IP, NULL, 0, WSA_FLAG_OVERLAPPED );
		memset( &( acceptArg->mOverlapped ), 0, sizeof( OVERLAPPED ) );

		int ret = AcceptEx( (SOCKET)acceptArg->mListenSocket, (SOCKET)acceptArg->mClientSocket,
				acceptArg->mBuffer,	0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
				&recvBytes, &( acceptArg->mOverlapped ) );

		if( ret == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()) ) {
			sp_syslog( LOG_ERR, "AcceptEx() fail, errno %d", WSAGetLastError() );
		}

		WaitForSingleObject( acceptArg->mAcceptEvent, INFINITE );
		ResetEvent( acceptArg->mAcceptEvent );
	}

	return NULL;
}

int SP_IocpServer :: start()
{
#ifdef SIGPIPE
	/* Don't die with SIGPIPE on remote read shutdown. That's dumb. */
	signal( SIGPIPE, SIG_IGN );
#endif

	int ret = 0;
	int listenFD = -1;

	ret = SP_IOUtils::tcpListen( mBindIP, mPort, &listenFD, 0 );

	if( 0 == ret ) {

		SP_IocpEventArg eventArg( mTimeout );
		eventArg.loadDisconnectEx( listenFD );

		SP_IocpAcceptArg_t acceptArg;
		memset( &acceptArg, 0, sizeof( acceptArg ) );

		acceptArg.mHandlerFactory = mHandlerFactory;
		acceptArg.mReqQueueSize = mReqQueueSize;
		acceptArg.mMaxConnections = mMaxConnections;
		acceptArg.mRefusedMsg = mRefusedMsg;
		acceptArg.mAcceptEvent = CreateEvent( NULL, TRUE, FALSE, NULL );

		acceptArg.mEventArg = &eventArg;
		acceptArg.mListenSocket = (HANDLE)listenFD;

		if( NULL == CreateIoCompletionPort( acceptArg.mListenSocket,
				eventArg.getCompletionPort(), SP_IocpEventCallback::eKeyAccept, 0 ) ) {
			sp_syslog( LOG_ERR, "CreateIoCompletionPort fail, errno %d", WSAGetLastError() );
			return -1;		
		}

		pthread_t thread;
		ret = pthread_create( &thread, NULL, reinterpret_cast<void*(*)(void*)>(acceptThread), &acceptArg );
		if( 0 == ret ) {
			sp_syslog( LOG_NOTICE, "Thread #%ld has been created to accept socket", thread );
		} else {
			sp_syslog( LOG_WARNING, "Unable to create a thread to accept socket, %s", strerror( errno ) );
			return -1;
		}

		SP_Executor actExecutor( 1, "act" );
		SP_Executor workerExecutor( mMaxThreads, "work" );
		SP_CompletionHandler * completionHandler = mHandlerFactory->createCompletionHandler();

		/* Start the event loop. */
		while( 0 == mIsShutdown ) {
			SP_IocpEventCallback::eventLoop( &eventArg, &acceptArg );

			for( ; NULL != eventArg.getInputResultQueue()->top(); ) {
				SP_Task * task = (SP_Task*)eventArg.getInputResultQueue()->pop();
				workerExecutor.execute( task );
			}

			for( ; NULL != eventArg.getOutputResultQueue()->top(); ) {
				SP_Message * msg = (SP_Message*)eventArg.getOutputResultQueue()->pop();

				void ** arg = ( void** )malloc( sizeof( void * ) * 2 );
				arg[ 0 ] = (void*)completionHandler;
				arg[ 1 ] = (void*)msg;

				actExecutor.execute( outputCompleted, arg );
			}
		}

		delete completionHandler;

		sp_syslog( LOG_NOTICE, "Server is shutdown." );

		sp_close( listenFD );
	}

	return ret;
}

