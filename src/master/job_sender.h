#ifndef __JOB_SENDER_H
#define __JOB_SENDER_H

#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include "common/observer.h"
#include "common/helper.h"
#include "job.h"
#include "worker.h"

using boost::asio::ip::tcp;

namespace master {

class JobSender : python_server::Observer
{
public:
    JobSender()
	: stopped_( false ), newJobAvailable_( false )
	{}

    virtual void Start() = 0;

    void Stop();

    void Run();

	virtual void OnJobSendCompletion( bool success, const Worker *worker, const Job *job );

private:
    virtual void NotifyObserver( int event );

	virtual void SendJob( const Worker *worker, const Job *job ) = 0;

private:
    bool stopped_;

    boost::mutex awakeMut_;
    boost::condition_variable awakeCond_;
    bool newJobAvailable_;
};

class SenderBoost : public boost::enable_shared_from_this< SenderBoost >
{
public:
	typedef boost::shared_ptr< SenderBoost > sender_ptr;

public:
	SenderBoost( boost::asio::io_service &io_service, int sendBufferSize,
				 JobSender *sender, const Worker *worker, const Job *job )
	: io_service_( io_service ), socket_( io_service ),
	 sender_( sender ), worker_( worker ), job_( job )
	{}

	void Send();

private:
	void HandleConnect( const boost::system::error_code &error );

    void MakeRequest();

    void HandleWrite( const boost::system::error_code &error, size_t bytes_transferred );

    void HandleRead( const boost::system::error_code &error, size_t bytes_transferred );

private:
    boost::asio::io_service &io_service_;
	tcp::socket socket_;
    std::string request_;
    char response_;
	JobSender *sender_;
	const Worker *worker_;
	const Job *job_;
};

class JobSenderBoost : public JobSender
{
public:
    JobSenderBoost( boost::asio::io_service &io_service,
					int sendBufferSize, int maxSimultSendingJobs )
    : io_service_( io_service ), sendBufferSize_( sendBufferSize ),
	 sendJobsSem_( maxSimultSendingJobs )
    {}

    virtual void Start();

private:
	virtual void SendJob( const Worker *worker, const Job *job );

    virtual void OnJobSendCompletion( bool success, const Worker *worker, const Job *job );

private:
    boost::asio::io_service &io_service_;
	int sendBufferSize_;
	python_server::Semaphore sendJobsSem_;
};

} // namespace master

#endif