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

#define BOOST_SPIRIT_THREADSAFE

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
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include "common.h"
#include "common/request.h"
#include "common/log.h"
#include "common/config.h"
#include "common/error_code.h"
#include <common/configure.h>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

using namespace std;
using boost::asio::ip::tcp;

namespace python_server {

bool isDaemon;
bool isFork;
uid_t uid;
unsigned int numThread;
string exeDir;

boost::interprocess::shared_memory_object *sharedMemPool;
boost::interprocess::mapped_region *mappedRegion; 

struct ThreadParams
{
    int writeFifoFD, readFifoFD;
    string writeFifo, readFifo;
    pid_t pid;
};

typedef std::map< boost::thread::id, ThreadParams > ThreadInfo;
ThreadInfo threadInfo;


class Job
{
public:
    template< typename T >
    void ParseRequest( Request<T> &request )
    {
        const std::string &requestStr = request.GetString();

        std::istringstream ss( requestStr );

        boost::property_tree::ptree ptree;
        boost::property_tree::read_json( ss, ptree );

        jobId_ = ptree.get<int>( "id" );
        scriptLength_ = ptree.get<unsigned int>( "len" );
        language_ = ptree.get<std::string>( "lang" );
        taskId_ = ptree.get<int>( "task_id" );
        numTasks_ = ptree.get<int>( "num_tasks" );
        timeout_ = ptree.get<int>( "timeout" );
    }

    void GetResponse( std::string &response )
    {
        std::ostringstream ss;
        boost::property_tree::ptree ptree;

        ptree.put( "err", errCode_ );

        boost::property_tree::write_json( ss, ptree, false );
        size_t responseLength = ss.str().size();
        response = boost::lexical_cast< std::string >( responseLength );
        response += '\n';
        response += ss.str();
    }

    void OnError( int err )
    {
        errCode_ = err;
    }

    int GetJobId() const { return jobId_; }
    unsigned int GetScriptLength() const { return scriptLength_; }
    const std::string &GetScriptLanguage() const { return language_; }
    int GetTaskId() const { return taskId_; }
    int GetNumTasks() const { return numTasks_; }
    int GetTimeout() const { return timeout_; }

private:
    int jobId_;
    unsigned int scriptLength_;
    int errCode_;
    std::string language_;
    int taskId_;
    int numTasks_;
    int timeout_;
};

class ScriptExec
{
public:
    virtual ~ScriptExec() {}

    virtual void Execute( Job *job )
    {
        if ( !InitLanguageEnv() )
        {
            job->OnError( NODE_FATAL );
            return;
        }

        job_ = job;

        pid_t pid = DoFork();
        if ( pid > 0 )
            return;

        string scriptLength = boost::lexical_cast<std::string>( job->GetScriptLength() );

        size_t offset = job->GetJobId() * SHMEM_BLOCK_SIZE;
        string shmemOffset = boost::lexical_cast<std::string>( offset );

        string taskId = boost::lexical_cast<std::string>( job->GetTaskId() );
        string numTasks = boost::lexical_cast<std::string>( job->GetNumTasks() );

        ThreadParams &threadParams = threadInfo[ boost::this_thread::get_id() ];

        int ret = execl( exePath_.c_str(), job->GetScriptLanguage().c_str(),
                         nodePath_.c_str(),
                         threadParams.readFifo.c_str(), threadParams.writeFifo.c_str(),
                         scriptLength.c_str(),
                         taskId.c_str(), numTasks.c_str(), NULL );
        if ( ret < 0 )
        {
            PS_LOG( "ScriptExec::Execute: execl failed: " << strerror(errno) );
        }
        ::exit( 1 );
    }

    virtual void KillExec( pid_t pid )
    {
        PS_LOG( "poll timed out, trying to kill process: " << pid );
        int ret = kill( pid, SIGTERM );
        if ( ret == -1 )
        {
            PS_LOG( "process killing failed: pid=" << pid << ", err=" << strerror(errno) );
        }
    }

protected:
    virtual bool InitLanguageEnv() = 0;

    virtual pid_t DoFork()
    {
        pid_t pid = fork();

        if ( pid > 0 )
        {
            //PS_LOG( "wait child " << pid );
            ThreadParams &threadParams = threadInfo[ boost::this_thread::get_id() ];
            threadParams.pid = pid;

            sigset_t sigset, oldset;
            sigemptyset( &sigset );
            sigaddset( &sigset, SIGCHLD );
            sigprocmask( SIG_BLOCK, &sigset, &oldset );

            if ( DoFifoIO( threadParams.writeFifoFD, false, pid ) )
            {
                DoFifoIO( threadParams.readFifoFD, true, pid );
            }

            sigprocmask( SIG_BLOCK, &oldset, NULL );
            //PS_LOG( "wait child done " << pid );
        }
        else
        if ( pid == 0 )
        {
            isFork = true;
            // linux-only. kill child process, if parent exits
#ifdef HAVE_SYS_PRCTL_H
            prctl( PR_SET_PDEATHSIG, SIGHUP );
#endif
        }
        else
        {
            PS_LOG( "ScriptExec::DoFork: fork() failed " << strerror(errno) );
        }

        return pid;
    }

    bool DoFifoIO( int fifo, bool doRead, pid_t pid )
    {
        if ( fifo != -1 )
        {
            pollfd pfd[1];
            pfd[0].fd = fifo;
            pfd[0].events = doRead ? POLLIN : POLLOUT;

            int errCode = NODE_FATAL;
            int ret = poll( pfd, 1, job_->GetTimeout() * 1000 );
            if ( ret > 0 )
            {
                if ( doRead )
                {
                    ret = read( fifo, &errCode, sizeof( errCode ) );
                    if ( ret > 0 )
                    {
                        return true;
                    }
                    else
                    {
                        PS_LOG( "ScriptExec::DoFifoIO: read fifo failed: " << strerror(errno) );
                    }
                }
                else
                {
                    size_t offset = job_->GetJobId() * SHMEM_BLOCK_SIZE;
                    char *shmemAddr = (char*)python_server::mappedRegion->get_address() + offset;
                    
                    ret = write( fifo, shmemAddr, job_->GetScriptLength() );
                    if ( ret > 0 )
                    {
                        return true;
                    }
                    else
                    {
                        PS_LOG( "ScriptExec::DoFifoIO: write fifo failed: " << strerror(errno) );
                    }
                }
            }
            else
            if ( ret == 0 )
            {
                errCode = NODE_JOB_TIMEOUT;
                KillExec( pid );
            }
            else
            {
                PS_LOG( "ScriptExec::DoFifoIO: poll failed: " << strerror(errno) );

            }
            job_->OnError( errCode );
        }
        else
        {
            PS_LOG( "ScriptExec::DoFifoIO: pipe not opened" );
            job_->OnError( NODE_FATAL );
        }
        return false;
    }

protected:
    Job *job_;
    std::string exePath_;
    std::string nodePath_;
};

class PythonExec : public ScriptExec
{
protected:
    virtual bool InitLanguageEnv()
    {
        try
        {
            exePath_ = Config::Instance().Get<string>( "python" );
            nodePath_ = exeDir + '/' + NODE_SCRIPT_NAME_PY;
        }
        catch( std::exception &e )
        {
            PS_LOG( "PythonExec::Init: " << e.what() );
            return false;
        }
        return true;
    }
};

class JavaExec : public ScriptExec
{
public:
    virtual void Execute( Job *job )
    {
        if ( !InitLanguageEnv() )
        {
            job->OnError( NODE_FATAL );
            return;
        }

        job_ = job;

        pid_t pid = DoFork();
        if ( pid > 0 )
            return;

        string scriptLength = boost::lexical_cast<std::string>( job->GetScriptLength() );

        size_t offset = job->GetJobId() * SHMEM_BLOCK_SIZE;
        string shmemOffset = boost::lexical_cast<std::string>( offset );

        string taskId = boost::lexical_cast<std::string>( job->GetTaskId() );
        string numTasks = boost::lexical_cast<std::string>( job->GetNumTasks() );

        ThreadParams &threadParams = threadInfo[ boost::this_thread::get_id() ];

        int ret = execl( exePath_.c_str(), job->GetScriptLanguage().c_str(),
                         "-cp", nodePath_.c_str(),
                         "node",
                         threadParams.readFifo.c_str(), threadParams.writeFifo.c_str(),
                         scriptLength.c_str(),
                         taskId.c_str(), numTasks.c_str(), NULL );
        if ( ret < 0 )
        {
            PS_LOG( "JavaExec::Execute: execl failed: " << strerror(errno) );
        }
        ::exit( 1 );
    }

protected:
    virtual bool InitLanguageEnv()
    {
        try
        {
            exePath_ = Config::Instance().Get<string>( "java" );
            nodePath_ = exeDir + "/node";
        }
        catch( std::exception &e )
        {
            PS_LOG( "JavaExec::Init: " << e.what() );
            return false;
        }
        return true;
    }
};

class ShellExec : public ScriptExec
{
protected:
    virtual bool InitLanguageEnv()
    {
        try
        {
            exePath_ = Config::Instance().Get<string>( "shell" );
            nodePath_ = exeDir + '/' + NODE_SCRIPT_NAME_SHELL;
        }
        catch( std::exception &e )
        {
            PS_LOG( "ShellExec::Init: " << e.what() );
            return false;
        }
        return true;
    }
};

class RubyExec : public ScriptExec
{
protected:
    virtual bool InitLanguageEnv()
    {
        try
        {
            exePath_ = Config::Instance().Get<string>( "ruby" );
            nodePath_ = exeDir + '/' + NODE_SCRIPT_NAME_RUBY;
        }
        catch( std::exception &e )
        {
            PS_LOG( "RubyExec::Init: " << e.what() );
            return false;
        }
        return true;
    }
};

class JavaScriptExec : public ScriptExec
{
protected:
    virtual bool InitLanguageEnv()
    {
        try
        {
            exePath_ = Config::Instance().Get<string>( "js" );
            nodePath_ = exeDir + '/' + NODE_SCRIPT_NAME_JS;
        }
        catch( std::exception &e )
        {
            PS_LOG( "JavaScriptExec::Init: " << e.what() );
            return false;
        }
        return true;
    }
};

class ExecCreator
{
public:
    virtual ScriptExec *Create( const std::string &language )
    {
        if ( language == "python" )
            return new PythonExec();
        if ( language == "java" )
            return new JavaExec();
        if ( language == "shell" )
            return new ShellExec();
        if ( language == "ruby" )
            return new RubyExec();
        if ( language == "js" )
            return new JavaScriptExec();
        return NULL;
    }
};

class Session : public boost::enable_shared_from_this< Session >
{
    typedef boost::array< char, 1024 > BufferType;

public:
    Session( boost::asio::io_service &io_service )
    : socket_( io_service ), request_( true )
    {
    }

    virtual ~Session()
    {
        cout << "E: ~Session()" << endl;
    }

    void Start()
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
    void FirstRead( const boost::system::error_code& error, size_t bytes_transferred )
    {
        if ( !error )
        {
            int ret = request_.OnFirstRead( buffer_, bytes_transferred );
            if ( ret == 0 )
            {
                socket_.async_read_some( boost::asio::buffer( buffer_ ),
                                         boost::bind( &Session::FirstRead, shared_from_this(),
                                                      boost::asio::placeholders::error,
                                                      boost::asio::placeholders::bytes_transferred ) );
                return;
            }
            if ( ret < 0 )
            {
                job_.OnError( NODE_FATAL );
                WriteResponse();
                return;
            }
        }
        else
        {
            PS_LOG( "Session::FirstRead error=" << error.message() );
        }

        HandleRead( error, bytes_transferred );
    }

    void HandleRead( const boost::system::error_code& error, size_t bytes_transferred )
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
            PS_LOG( "Session::HandleRead error=" << error.message() );
            //HandleError( error );
        }
    }

    void HandleRequest()
    {
        job_.ParseRequest( request_ );

        boost::scoped_ptr< ScriptExec > scriptExec(
            execCreator_.Create( job_.GetScriptLanguage() )
        );
        if ( scriptExec )
        {
            scriptExec->Execute( &job_ );
        }
        else
        {
            PS_LOG( "Session::HandleRequest: appropriate executor not found for language: "
                    << job_.GetScriptLanguage() );
            job_.OnError( NODE_LANG_NOT_SUPPORTED );
        }

        request_.Reset();
        Start();

        WriteResponse();
    }

    void WriteResponse()
    {
        job_.GetResponse( response_ );

        boost::asio::async_write( socket_,
                                boost::asio::buffer( response_ ),
                                boost::bind( &Session::HandleWrite, shared_from_this(),
                                             boost::asio::placeholders::error,
                                             boost::asio::placeholders::bytes_transferred ) );
    }

    void HandleWrite( const boost::system::error_code& error, size_t bytes_transferred )
    {
        if ( error )
        {
            PS_LOG( "Session::HandleWrite error=" << error.message() );
        }
    }

protected:
    tcp::socket socket_;
    BufferType buffer_;
    Request< BufferType > request_;
    Job job_;
    ExecCreator execCreator_;
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
            acceptor_.bind( endpoint );
            acceptor_.listen();
        }
        catch( std::exception &e )
        {
            PS_LOG( "ConnectionAcceptor: " << e.what() );
        }

        StartAccept();
    }

private:
    void StartAccept()
    {
        session_ptr session( new Session( io_service_ ) );
        acceptor_.async_accept( session->GetSocket(),
                                boost::bind( &ConnectionAcceptor::HandleAccept, this,
                                            session, boost::asio::placeholders::error ) );
    }

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
        PS_LOG( "Caught SIGTERM. Exiting..." );
        exit( 0 );
    }

    if ( s == SIGCHLD )
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
    sigaction( SIGHUP, &sigHandler, 0 );
}

void SetupPyExecIPC()
{
    namespace ipc = boost::interprocess; 

    try
    {
        python_server::sharedMemPool = new ipc::shared_memory_object( ipc::open_only, python_server::SHMEM_NAME, ipc::read_only );
        python_server::mappedRegion = new ipc::mapped_region( *python_server::sharedMemPool, ipc::read_only );
    }
    catch( std::exception &e )
    {
        PS_LOG( "SetupPyExecIPC failed: " << e.what() );
        exit( 1 );
    }
}

void SetupLanguageRuntime()
{
    pid_t pid = fork();
    if ( pid == 0 )
    {
        python_server::isFork = true;
        std::string javacPath;
        try
        {
            javacPath = python_server::Config::Instance().Get<std::string>( "javac" );
        }
        catch( std::exception &e )
        {
            PS_LOG( "SetupLanguageRuntime: get javac path failed: " << e.what() );
        }
        std::string nodePath = python_server::exeDir + '/' + python_server::NODE_SCRIPT_NAME_JAVA;
        if ( access( javacPath.c_str(), F_OK ) != -1 )
        {
            int ret = execl( javacPath.c_str(), "javac", nodePath.c_str(), NULL );
            if ( ret < 0 )
            {
                PS_LOG( "SetupLanguageRuntime: execl(javac) failed: " << strerror(errno) );
            }
        }
        else
        {
            PS_LOG( "SetupLanguageRuntime: file not found: " << javacPath );
        }
        ::exit( 1 );
    }
    else
    if ( pid > 0 )
    {
        int status;
        waitpid( pid, &status, 0 );
    }
    else
    if ( pid < 0 )
    {
        PS_LOG( "SetupLanguageRuntime: fork() failed " << strerror(errno) );
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

        PS_LOG( "successfully impersonated, uid=" << python_server::uid );
    }
}

void AtExit()
{
    if ( python_server::isFork )
        return;

    // cleanup threads
    python_server::ThreadInfo::iterator it;
    for( it = python_server::threadInfo.begin();
         it != python_server::threadInfo.end();
       ++it )
    {
        python_server::ThreadParams &threadParams = it->second;

        if ( threadParams.readFifoFD != -1 )
            close( threadParams.readFifoFD );

        if ( !threadParams.readFifo.empty() )
            unlink( threadParams.readFifo.c_str() );

        if ( threadParams.writeFifoFD != -1 )
            close( threadParams.writeFifoFD );

        if ( !threadParams.writeFifo.empty() )
            unlink( threadParams.writeFifo.c_str() );
    }

    delete python_server::mappedRegion;
    python_server::mappedRegion = NULL;

    delete python_server::sharedMemPool;
    python_server::sharedMemPool = NULL;

    python_server::logger::ShutdownLogger();

    kill( getppid(), SIGTERM );
}

int CreateFifo( const std::string &fifoName )
{
    unlink( fifoName.c_str() );

    int ret = mkfifo( fifoName.c_str(), S_IRUSR | S_IWUSR );
    if ( !ret )
    {
        if ( python_server::uid )
        {
            ret = chown( fifoName.c_str(), python_server::uid, -1 );
            if ( ret == -1 )
                PS_LOG( "CreateFifo: chown failed " << strerror(errno) );
        }

        int fifofd = open( fifoName.c_str(), O_RDWR | O_NONBLOCK );
        if ( fifofd == -1 )
        {
            PS_LOG( "open fifo " << fifoName << " failed: " << strerror(errno) );
        }
        return fifofd;
    }
    else
    {
        PS_LOG( "CreateFifo: mkfifo failed " << strerror(errno) );
    }
    return -1;
}

void OnThreadCreate( const boost::thread *thread )
{
    static int threadCnt = 0;

    python_server::ThreadParams threadParams;
    threadParams.writeFifoFD = -1;
    threadParams.readFifoFD = -1;

    std::ostringstream ss;
    ss << python_server::FIFO_NAME << 'w' << threadCnt;
    threadParams.writeFifo = ss.str();

    threadParams.writeFifoFD = CreateFifo( threadParams.writeFifo );
    if ( threadParams.writeFifoFD == -1 )
    {
        threadParams.writeFifo.clear();
    }

    std::ostringstream ss2;
    ss2 << python_server::FIFO_NAME << 'r' << threadCnt;
    threadParams.readFifo = ss2.str();

    threadParams.readFifoFD = CreateFifo( threadParams.readFifo );
    if ( threadParams.readFifoFD == -1 )
    {
        threadParams.readFifo.clear();
    }

    python_server::threadInfo[ thread->get_id() ] = threadParams;
    ++threadCnt;
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

    try
    {
        // initialization
        python_server::isDaemon = false;
        python_server::isFork = false;
        python_server::uid = 0;

        // parse input command line options
        namespace po = boost::program_options;
        
        po::options_description descr;

        descr.add_options()
            ("num_thread", po::value<unsigned int>(), "Thread pool size")
            ("exe_dir", po::value<std::string>(), "Executable working directory")
            ("d", "Run as a daemon")
            ("u", po::value<uid_t>(), "Start as a specific non-root user")
            ("f", "Create process for each request");
        
        po::variables_map vm;
        po::store( po::parse_command_line( argc, argv, descr ), vm );
        po::notify( vm );

        if ( vm.count( "d" ) )
        {
            python_server::isDaemon = true;
        }

        python_server::logger::InitLogger( python_server::isDaemon, "PyExec" );

        if ( vm.count( "u" ) )
        {
            python_server::uid = vm[ "u" ].as<uid_t>();
        }

        if ( vm.count( "num_thread" ) )
        {
            python_server::numThread = vm[ "num_thread" ].as<unsigned int>();
        }

        if ( vm.count( "exe_dir" ) )
        {
            python_server::exeDir = vm[ "exe_dir" ].as<std::string>();
        }

        python_server::Config::Instance().ParseConfig( python_server::exeDir.c_str() );

        SetupLanguageRuntime();

        SetupPyExecIPC();
        
        // start accepting connections
        boost::asio::io_service io_service;

        python_server::ConnectionAcceptor acceptor( io_service, python_server::DEFAULT_PYEXEC_PORT );

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
        cout << "Exception: " << e.what() << endl;
        PS_LOG( "Exception: " << e.what() );
    }

    PS_LOG( "stopped" );

    return 0;
}
