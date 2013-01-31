/*
===========================================================================

This software is licensed under the Apache 2 license, quoted below.

Copyright (C) 2013 Andrey Budnik <budnik27@gmail.com>

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations under
the License.

===========================================================================
*/

#include <cerrno>
#include <iostream>
#include <boost/program_options.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/thread.hpp>
#include <boost/array.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <Python.h>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include <sys/prctl.h>
#include "Common.h"
#include "Log.h"


using namespace std;
using boost::asio::ip::tcp;

namespace python_server {

bool isDaemon;
bool forkMode;
bool isFork;
uid_t uid;
unsigned int numThread;

boost::interprocess::shared_memory_object *sharedMemPool;
boost::interprocess::mapped_region *mappedRegion;

struct ThreadParams
{
	int pipefd[2];
	pid_t pid;
};

typedef std::map< boost::thread::id, ThreadParams > ThreadInfo;
ThreadInfo threadInfo;

boost::mutex *rParserMut;
boost::mutex *wParserMut;


template< typename BufferT >
class Request
{
public:
	Request()
	: requestLength_( 0 ),
	 bytesRead_( 0 ),
	 headerOffset_( 0 )
	{
	}

	void OnRead( BufferT &buf, size_t bytes_transferred  )
	{
		std::copy( buf.begin() + headerOffset_, buf.begin() + bytes_transferred, back_inserter( request_ ) );

		bytesRead_ += bytes_transferred - headerOffset_;
	}

	int OnFirstRead( BufferT &buf, size_t bytes_transferred  )
	{
		headerOffset_ = ParseRequestHeader( buf, bytes_transferred );
		return CheckHeader();
	}

	bool IsReadCompleted() const
	{
		return bytesRead_ >= requestLength_;
	}

	const std::string &GetRequestString() const
	{
		return request_;
	}

	int GetRequestLength() const
	{
		return requestLength_;
	}

	void Reset()
	{
		request_.clear();
		requestLength_ = bytesRead_ = headerOffset_ = 0;
	}

private:
	int ParseRequestHeader( BufferT &buf, size_t bytes_transferred  )
	{
		int offset = 0;
		std::string length;

		typename BufferT::iterator it = std::find( buf.begin(), buf.begin() + bytes_transferred, '\n' );
		if ( it != buf.end() )
		{
			offset = (int)std::distance( buf.begin(), it );
			std::copy( buf.begin(), buf.begin() + offset, back_inserter( length ) );

			try
			{
				requestLength_ = boost::lexical_cast<unsigned int>( length );
			}
			catch( boost::bad_lexical_cast &e )
			{
				PS_LOG( "Reading request length failed: " << e.what() );
			}
		}
		else
		{
			PS_LOG( "Reading request length failed: new line not found" );
		}

		return offset;
	}

	int CheckHeader()
	{
		// TODO: Error codes
		if ( headerOffset_ > maxScriptSize )
			return -1;

		return 0;
	}

private:
	std::string request_;
	int	requestLength_;
	int bytesRead_;
	unsigned int headerOffset_;
};

class IActionStrategy
{
public:
	virtual void HandleRequest( const std::string &requestStr ) = 0;
	virtual const std::string &GetResponse() = 0;
};

class ExecutePython : public IActionStrategy
{
public:
	virtual void HandleRequest( const std::string &requestStr )
	{
		//using namespace boost::python;
		//exec( str( requestStr ) );

		std::stringstream ss;
		ss << requestStr;

		rParserMut->lock();
		boost::property_tree::read_json( ss, ptree_ );
		rParserMut->unlock();
		int id = ptree_.get<int>( "id" );

		pid_t pid = 0;
		if ( forkMode )
		{
			pid = DoFork();
			if ( pid > 0 )
				return;
		}

		size_t offset = id * shmemBlockSize;
		char *addr = (char*)mappedRegion->get_address() + offset;

		//PyCompilerFlags cf;
		//cf.cf_flags = 0; // TODO: ignore os._exit(), sys.exit(), thread.exit(), etc.
	    errCode_ = PyRun_SimpleStringFlags( addr, NULL );

		if ( forkMode && pid == 0 )
		{
			ThreadParams &threadParams = threadInfo[ boost::this_thread::get_id() ];
		    write( threadParams.pipefd[1], &errCode_, sizeof( errCode_ ) );
			exit( errCode_ );
		}
	}

	virtual pid_t DoFork()
	{
		pid_t pid = fork();

		if ( pid > 0 )
		{
			//PS_LOG( "wait child " << pid );
			ThreadParams &threadParams = threadInfo[ boost::this_thread::get_id() ];
		    threadParams.pid = pid;
		    read( threadParams.pipefd[0], &errCode_, sizeof( errCode_ ) );
			//PS_LOG( "wait child done " << pid );
		}
		else
		if ( pid == 0 )
		{
			isFork = true;
			prctl( PR_SET_PDEATHSIG, SIGHUP );
			PyOS_AfterFork();
		}
		else
		{
			PS_LOG( "DoFork: fork() failed " << strerror(errno) );
		}

		return pid;
	}

	virtual const std::string &GetResponse()
	{
		std::stringstream ss;

		// TODO: full error code description
		ptree_.put( "err", errCode_ );

		wParserMut->lock();
		boost::property_tree::write_json( ss, ptree_, false );
		wParserMut->unlock();
		response_ = ss.str();
		return response_;
	}

	virtual void OnError( int err )
	{
		errCode_ = err;
	}

private:
	boost::property_tree::ptree ptree_;
	std::string response_;
	int errCode_;
};

template< typename ActionPolicy >
class Action : private ActionPolicy
{
public:
	template< typename T >
	void HandleRequest( Request<T> &request )
	{
		const std::string &requestStr = request.GetRequestString();
		ActionPolicy::HandleRequest( requestStr );
	}

	virtual const std::string &GetResponse()
	{
		return ActionPolicy::GetResponse();
	}

	virtual void OnError( int err )
	{
		ActionPolicy::OnError( err );
	}
};

class Session : public boost::enable_shared_from_this< Session >
{
	typedef boost::array< char, 1024 > BufferType;

public:
	Session( boost::asio::io_service &io_service )
	: socket_( io_service )
	{
	}

	virtual ~Session()
	{
		cout << "E: ~Session()" << endl;
	}

	virtual void Start()
	{
	    memset( buffer_.c_array(), 0, buffer_.size() );
		socket_.async_read_some( boost::asio::buffer( buffer_ ),
								 boost::bind( &Session::FirstRead, shared_from_this(),
											boost::asio::placeholders::error,
											boost::asio::placeholders::bytes_transferred ) );
	}

	tcp::socket &GetSocket()
	{
		return socket_;
	}

protected:
	virtual void FirstRead( const boost::system::error_code& error, size_t bytes_transferred )
	{
		if ( !error )
		{
			int ret = request_.OnFirstRead( buffer_, bytes_transferred );
			if ( ret < 0 )
			{
				action_.OnError( ret );
				WriteResponse();
				return;
			}
		}
		else
		{
			PS_LOG( "Session::FirstRead error=" << error.value() );
		}

		HandleRead( error, bytes_transferred );
	}

	virtual void HandleRead( const boost::system::error_code& error, size_t bytes_transferred )
	{
		if ( !error )
		{
			request_.OnRead( buffer_, bytes_transferred );

			if ( !request_.IsReadCompleted() )
			{
				socket_.async_read_some( boost::asio::buffer( buffer_ ),
										 boost::bind( &Session::HandleRead, shared_from_this(),
													boost::asio::placeholders::error,
													boost::asio::placeholders::bytes_transferred ) );
			}
			else
			{
				HandleRequest();
			}
		}
		else
		{
			PS_LOG( "Session::HandleRead error=" << error.value() );
			//HandleError( error );
		}
	}

	virtual void HandleRequest()
	{
		action_.HandleRequest( request_ );

		request_.Reset();
		Start();

		WriteResponse();
	}

	virtual void WriteResponse()
	{
	    response_ = action_.GetResponse();

		boost::asio::async_write( socket_,
								boost::asio::buffer( response_ ),
	   							boost::bind( &Session::HandleWrite, shared_from_this(),
											 boost::asio::placeholders::error,
											 boost::asio::placeholders::bytes_transferred ) );
	}

	virtual void HandleWrite( const boost::system::error_code& error, size_t bytes_transferred )
	{
		if ( error )
		{
			PS_LOG( "Session::HandleWrite error=" << error.value() );
		}
	}

protected:
	tcp::socket socket_;
	BufferType buffer_;
	Request< BufferType > request_;
	Action< ExecutePython > action_;
	std::string response_;
};


class ConnectionAcceptor
{
	typedef boost::shared_ptr< Session > session_ptr;

public:
	ConnectionAcceptor( boost::asio::io_service &io_service, unsigned short port )
	: io_service_( io_service ),
	  acceptor_( io_service )
	{
		try
		{
		    tcp::endpoint endpoint( tcp::v4(), port );
			acceptor_.open( endpoint.protocol() );
			acceptor_.set_option( tcp::acceptor::reuse_address( true ) );
			acceptor_.set_option( tcp::no_delay( true ) );
			acceptor_.bind( tcp::endpoint( tcp::v4(), port ) );
			acceptor_.listen();
		}
		catch( std::exception &e )
		{
			PS_LOG( "ConnectionAcceptor: " << e.what() );
		}

		StartAccept();
	}

	void StartAccept()
	{
		session_ptr session( new Session( io_service_ ) );
		acceptor_.async_accept( session->GetSocket(),
								boost::bind( &ConnectionAcceptor::HandleAccept, this,
											session, boost::asio::placeholders::error ) );
	}

private:
	void HandleAccept( session_ptr session, const boost::system::error_code &error )
	{
		if ( !error )
		{
			cout << "connection accepted..." << endl;
			io_service_.post( boost::bind( &Session::Start, session ) );
			StartAccept();
		}
		else
		{
			PS_LOG( "HandleAccept: " << error.message() );
		}
	}

private:
	boost::asio::io_service &io_service_;
	tcp::acceptor acceptor_;
};

} // namespace python_server


namespace {

void SigHandler( int s )
{
	if ( s == SIGTERM )
	{
		exit( 0 );
	}

	if ( s == SIGCHLD && python_server::forkMode )
	{
		// On Linux, multiple children terminating will be compressed into a single SIGCHLD
		while( 1 )
		{
			int status;
			pid_t pid = waitpid( -1, &status, WNOHANG );
			if ( pid <= 0 )
				break;
		}
	}
}

void SetupSignalHandlers()
{
	struct sigaction sigHandler;
	memset( &sigHandler, 0, sizeof( sigHandler ) );
	sigHandler.sa_handler = SigHandler;
	sigemptyset(&sigHandler.sa_mask);
	sigHandler.sa_flags = 0;

	sigaction( SIGTERM, &sigHandler, 0 );
	sigaction( SIGCHLD, &sigHandler, 0 );
}

void SetupPyExecIPC()
{
	namespace ipc = boost::interprocess;

	try
	{
		python_server::sharedMemPool = new ipc::shared_memory_object( ipc::open_only, python_server::shmemName, ipc::read_only );
		python_server::mappedRegion = new ipc::mapped_region( *python_server::sharedMemPool, ipc::read_only );
	}
	catch( std::exception &e )
	{
		PS_LOG( "SetupPyExecIPC failed: " << e.what() );
		exit( 1 );
	}
}

void Impersonate()
{
	if ( python_server::uid )
	{
		int ret = setuid( python_server::uid );
		if ( ret < 0 )
		{
			PS_LOG( "impersonate uid=" << python_server::uid << " failed : " << strerror(errno) );
			exit( 1 );
		}
	}
}

void AtExit()
{
	if ( python_server::isFork )
		return;

	kill( getppid(), SIGTERM );

	// cleanup threads
	python_server::ThreadInfo::iterator it;
	for( it = python_server::threadInfo.begin();
		 it != python_server::threadInfo.end();
	   ++it )
	{
		python_server::ThreadParams &threadParams = it->second;

		close( threadParams.pipefd[0] );
		close( threadParams.pipefd[1] );
	}

	if ( python_server::rParserMut )
	{
		delete python_server::rParserMut;
		python_server::rParserMut = NULL;
	}

	if ( python_server::wParserMut )
	{
		delete python_server::wParserMut;
		python_server::wParserMut = NULL;
	}

	python_server::logger::ShutdownLogger();
}

void OnThreadCreate( const boost::thread *thread )
{
	static int threadCnt = 0;

	python_server::ThreadParams threadParams;
	memset( &threadParams, 0, sizeof( threadParams ) );
	if ( python_server::forkMode )
	{
		pipe( threadParams.pipefd );
	}
	++threadCnt;

	python_server::threadInfo[ thread->get_id() ] = threadParams;
}

void ThreadFun( boost::asio::io_service *io_service )
{
	try
	{
		io_service->run();
	}
	catch( std::exception &e )
	{
		PS_LOG( "ThreadFun: " << e.what() );
	}
}

} // anonymous namespace


int main( int argc, char* argv[], char **envp )
{
	SetupSignalHandlers();
	atexit( AtExit );

	PyEval_InitThreads();
	Py_Initialize();

	try
	{
		// initialization
		python_server::isDaemon = false;
		python_server::forkMode = true;
		python_server::isFork = false;
		python_server::uid = 0;

		// parse input command line options
		namespace po = boost::program_options;
		
		po::options_description descr;

		descr.add_options()
			("num_thread", po::value<unsigned int>(), "Thread pool size")
			("d", "Run as a daemon")
			("u", po::value<uid_t>(), "Start as a specific non-root user")
			("f", "Create process for each request");
		
		po::variables_map vm;
		po::store( po::parse_command_line( argc, argv, descr ), vm );
		po::notify( vm );

		if ( vm.count( "u" ) )
		{
			python_server::uid = vm[ "u" ].as<uid_t>();
		}

		if ( vm.count( "d" ) )
		{
			python_server::isDaemon = true;
		}

		if ( vm.count( "t" ) )
		{
			python_server::forkMode = false;
		}

		if ( vm.count( "num_thread" ) )
		{
			python_server::numThread = vm[ "num_thread" ].as<unsigned int>();
		}

		python_server::logger::InitLogger( python_server::isDaemon, "PyExec" );

		SetupPyExecIPC();
		
		// start accepting connections
		boost::asio::io_service io_service;

		python_server::ConnectionAcceptor acceptor( io_service, python_server::defaultPyExecPort );

		python_server::rParserMut = new boost::mutex();
		python_server::wParserMut = new boost::mutex();

		// create thread pool
		boost::thread_group worker_threads;
		for( unsigned int i = 0; i < python_server::numThread; ++i )
		{
			boost::thread *thread = worker_threads.create_thread(
				boost::bind( &ThreadFun, &io_service )
			);
			OnThreadCreate( thread );
		}

		// signal parent process to say that PyExec has been initialized
		kill( getppid(), SIGUSR1 );

		Impersonate();

		if ( !python_server::isDaemon )
		{
			sigset_t waitset;
			int sig;
			sigemptyset( &waitset );
			sigaddset( &waitset, SIGTERM );
			sigwait( &waitset, &sig );
		}
		else
		{
			PS_LOG( "started" );

			sigset_t waitset;
			int sig;
			sigemptyset( &waitset );
			sigaddset( &waitset, SIGTERM );
			sigwait( &waitset, &sig );
		}

		io_service.stop();
		worker_threads.join_all();
	}
	catch( std::exception &e )
	{
		cout << e.what() << endl;
		PS_LOG( e.what() );
	}

	Py_Finalize();

	PS_LOG( "stopped" );

	return 0;
}
