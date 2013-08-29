#ifndef __JOB_MANAGER_H
#define __JOB_MANAGER_H

#include <boost/property_tree/ptree.hpp>
#include "job.h"


namespace master {

class JobDescriptionLanguage
{
public:
    virtual ~JobDescriptionLanguage() {}
    virtual bool ParseJob( const std::string &job_description, boost::property_tree::ptree &ptree ) = 0;
};

class JDLJason : public JobDescriptionLanguage
{
public:
    bool ParseJob( const std::string &job_description, boost::property_tree::ptree &ptree );
};

class JobManager
{
public:
    Job *CreateJob( const std::string &job_description ) const;
    void PushJob( Job *job );

    void SetExeDir( const std::string &dir ) { exeDir_ = dir; }

    Job *GetJobById( int64_t jobId );

    Job *PopJob();
    Job *GetTopJob();

    static JobManager &Instance()
    {
        static JobManager instance_;
        return instance_;
    }

    void Shutdown();

private:
    bool ReadScript( const std::string &fileName, std::string &script ) const;
    Job *CreateJob( boost::property_tree::ptree &ptree ) const;

private:
    JobQueue jobs_;
    std::string exeDir_;
};

} // namespace master

#endif
