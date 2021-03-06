/*
===========================================================================

This software is licensed under the Apache 2 license, quoted below.

Copyright (C) 2015 Andrey Budnik <budnik27@gmail.com>

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

#include "cron_manager.h"
#include "job_history.h"
#include "job_manager.h"
#include "common/service_locator.h"
#include "statistics.h"

namespace master {

CronManager::TimeoutHandler::TimeoutHandler()
: removed_( false )
{}

void CronManager::JobTimeoutHandler::HandleTimeout()
{
    auto jobManager = common::GetService< IJobManager >();
    jobManager->BuildAndPushJob( -1, jobDescription_, true );
}

void CronManager::MetaJobTimeoutHandler::HandleTimeout()
{
    auto jobManager = common::GetService< IJobManager >();
    jobManager->BuildAndPushJob( -1, jobDescription_, true );
}


void CronManager::Start()
{
    io_service_.post( boost::bind( &CronManager::Run, this ) );
}

void CronManager::Stop()
{
    stopped_ = true;
    timer_.StopWaiting();
}

void CronManager::Run()
{
    while( !stopped_ )
    {
        timer_.Wait( 1000 );
        CheckTimeouts();
    }
}

void CronManager::Shutdown()
{
    StopAllJobs();
}

void CronManager::CheckTimeouts()
{
    std::vector< CallbackPtr > ready;

    {
        std::unique_lock< std::mutex > lock( jobsMut_ );
        auto it = jobs_.cbegin();
        const auto now = std::chrono::system_clock::now();
        for( ; it != jobs_.cend(); )
        {
            const ptime &jobPlannedTime = it->first;
            if ( now < jobPlannedTime ) // skip earlier planned jobs
                break;

            const CallbackPtr &handler = it->second;
            if ( !handler->removed_ )
            {
                ready.push_back( handler );
                names_.erase( handler->jobName_ );
            }
            jobs_.erase( it++ );
        }
    }

    for( const CallbackPtr &handler : ready )
    {
        handler->HandleTimeout();
    }
}

void CronManager::PushJob( const JobPtr &job, bool afterExecution )
{
    const auto now = std::chrono::system_clock::now();
    auto deadline = job->GetCron().Next( now );
    if ( afterExecution )
    {
        if ( deadline <= now )
        {
            deadline += std::chrono::minutes( 1 );
        }
    }
    else
    {
        auto jobManager = common::GetService< IJobManager >();
        jobManager->RegisterJobName( job->GetName() );
    }

    auto jobEventReceiver = common::GetService< IJobEventReceiver >();
    jobEventReceiver->OnJobAdd( job->GetName(), job->GetDescription() );

    auto handler = std::make_shared< JobTimeoutHandler >();
    handler->jobDescription_ = job->GetDescription();
    handler->jobName_ = job->GetName();
    handler->deadline_ = deadline;

    std::unique_lock< std::mutex > lock( jobsMut_ );
    jobs_.emplace( deadline, handler );

    names_[ job->GetName() ] = handler;
}

void CronManager::PushMetaJob( const JobGroupPtr &metaJob )
{
    const auto now = std::chrono::system_clock::now();
    auto deadline = metaJob->GetCron().Next( now );
    if ( deadline <= now )
    {
        deadline += std::chrono::minutes( 1 );
    }

    auto jobEventReceiver = common::GetService< IJobEventReceiver >();
    jobEventReceiver->OnJobAdd( metaJob->GetName(), metaJob->GetDescription() );

    auto handler = std::make_shared< MetaJobTimeoutHandler >();
    handler->jobDescription_ = metaJob->GetDescription();
    handler->jobName_ = metaJob->GetName();
    handler->deadline_ = deadline;

    std::unique_lock< std::mutex > lock( jobsMut_ );
    jobs_.emplace( deadline, handler );

    names_[ metaJob->GetName() ] = handler;
}

void CronManager::PushMetaJob( std::list< JobPtr > &jobs )
{
    auto jobManager = common::GetService< IJobManager >();
    auto metaJob = jobs.front()->GetJobGroup();

    const auto now = std::chrono::system_clock::now();
    auto deadline = metaJob->GetCron().Next( now );

    jobManager->RegisterJobName( metaJob->GetName() );

    auto jobEventReceiver = common::GetService< IJobEventReceiver >();
    jobEventReceiver->OnJobAdd( metaJob->GetName(), metaJob->GetDescription() );

    auto handler = std::make_shared< MetaJobTimeoutHandler >();
    handler->jobDescription_ = metaJob->GetDescription();
    handler->jobName_ = metaJob->GetName();
    handler->deadline_ = deadline;

    for( const auto &job : jobs )
    {
        if ( !job->GetName().empty() )
        {
            handler->jobNames.insert( job->GetName() );
            jobManager->RegisterJobName( job->GetName() );
        }
    }

    std::unique_lock< std::mutex > lock( jobsMut_ );
    jobs_.emplace( deadline, handler );

    names_[ metaJob->GetName() ] = handler;
}

void CronManager::StopJob( const std::string &jobName )
{
    std::unique_lock< std::mutex > lock( jobsMut_ );
    auto it = names_.find( jobName );
    if ( it != names_.end() )
    {
        const CallbackPtr &handler = it->second;
        ReleaseJob( handler );
        names_.erase( it );
    }
}

void CronManager::StopAllJobs()
{
    std::unique_lock< std::mutex > lock( jobsMut_ );

    for( auto it = jobs_.cbegin(); it != jobs_.cend(); ++it )
    {
        const CallbackPtr &handler = it->second;
        ReleaseJob( handler );
    }

    jobs_.clear();
    names_.clear();
}

void CronManager::ReleaseJob( const CallbackPtr &handler ) const
{
    auto jobManager = common::GetService< IJobManager >();
    handler->removed_ = true;

    auto handler_meta = std::dynamic_pointer_cast< MetaJobTimeoutHandler >( handler );
    if ( handler_meta )
    {
        for( const std::string &jobName : handler_meta->jobNames )
        {
            jobManager->ReleaseJobName( jobName );
        }
    }

    jobManager->ReleaseJobName( handler->jobName_ );

    auto jobEventReceiver = common::GetService< IJobEventReceiver >();
    jobEventReceiver->OnJobDelete( handler->jobName_ );
}

void CronManager::Accept( ICronVisitor *visitor )
{
    std::unique_lock< std::mutex > lock( jobsMut_ );

    visitor->Visit( *this );
}

void CronManager::GetJobsInfo( std::vector< CronJobInfo > &names )
{
    for( auto it = names_.cbegin(); it != names_.cend(); ++it )
    {
        const CallbackPtr &handler = it->second;
        if ( handler->removed_ )
            continue;

        CronJobInfo info;
        info.jobName_ = handler->jobName_;
        info.deadline_ = std::chrono::system_clock::to_time_t( handler->deadline_ );
        names.push_back( info );
    }
}

} // namespace master
