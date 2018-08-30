#include "archive.test.h"

#include <sys/stat.h>

namespace archive_test
{
// beacuse windows does not have S_ISREG et al.
#ifndef __STAT_MODE_CHECK
#define __STAT_MODE_CHECK( mode, mask ) (((mode) & _S_IFMT) == (mask))
#endif

class StatFileFromFileSystem : public AsyncTest
{
public:

  enum CheckMode
  {
    NoFile = 0,
    IsFile,
    IsDirectory
  };

private:
  std::string filepath_;
  CheckMode mode_  = NoFile;
  bool async_mode_ = false;
  bool passed = false;
  uv_fs_t request_;

  void CheckStat( int error_code )
  {
    uv_stat_t& to_test = request_.statbuf;

    AsyncTest::RunState has_passed = AsyncTest::RunState::Failed;

    if( mode_ == NoFile )
    {
      if( !( __STAT_MODE_CHECK( to_test.st_mode, S_IFREG ) || __STAT_MODE_CHECK( to_test.st_mode, S_IFDIR ) ) )
      {
        has_passed = AsyncTest::RunState::Passed;
      }
    }
    else if( mode_ == IsFile )
    {
      if( __STAT_MODE_CHECK( to_test.st_mode, S_IFREG ) )
      {
        has_passed = AsyncTest::RunState::Passed;
      }
    }
    else if( mode_ == IsDirectory )
    {
      if( __STAT_MODE_CHECK( to_test.st_mode, S_IFDIR ) )
      {
        has_passed = AsyncTest::RunState::Passed;
      }
    }
    else
    {
      has_passed = AsyncTest::RunState::Aborted;
    }

    archive::uv_fs_req_cleanup( &request_ );

    AsyncTest::Finished( has_passed );
  }

  void RunAsync()
  {
    request_.data = this;

    int ret = archive::uv_fs_stat( Loop(), &request_, filepath_.c_str(), []( uv_fs_t* req )
    {
      StatFileFromFileSystem* pThis = reinterpret_cast< StatFileFromFileSystem* >( req->data );
      pThis->CheckStat( ( int )req->result );
    } );

    if( ret < 0 )
    {
      AsyncTest::Finished( AsyncTest::RunState::Failed );
    }
  }

  void RunSync()
  {
    int ret = archive::uv_fs_stat( Loop(), &request_, filepath_.c_str(), nullptr );

    CheckStat( ret );
  }

public:
  StatFileFromFileSystem( const char* name, const std::string& base_path, const std::string& filepath, CheckMode mode, bool async_mode ) : AsyncTest( name )
  {
    filepath_ = base_path + filepath;
    mode_ = mode;
    async_mode_ = async_mode;
  }

  ~StatFileFromFileSystem()
  {
  }

  void Run()
  {
    if( async_mode_ == true )
    {
      RunAsync();
    }
    else
    {
      RunSync();
    }
  }
};



void stat_test_register( AppInfo* appInfo )
{
  #if 1
  // Stats checks
  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check file Off Disk /package.json" , appInfo->extracted_root_path_,  "/package.json",  StatFileFromFileSystem::IsFile, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check file Off Disk /package.json" , appInfo->extracted_root_path_,  "/package.json",  StatFileFromFileSystem::IsFile, false ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check file Off Disk /public/index.ejs" , appInfo->extracted_root_path_,  "/public/index.ejs",  StatFileFromFileSystem::IsFile, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check file Off Disk /public/index.ejs" , appInfo->extracted_root_path_,  "/public/index.ejs",  StatFileFromFileSystem::IsFile, false ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check dir Off Disk /public/" , appInfo->extracted_root_path_,  "/public/",  StatFileFromFileSystem::IsDirectory, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check dir Off Disk /public/" , appInfo->extracted_root_path_,  "/public/",  StatFileFromFileSystem::IsDirectory, false ) );

  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check file Off Archive /package.json" , appInfo->mount_root_path_,  "/package.json",  StatFileFromFileSystem::IsFile, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check file Off Archive /package.json" , appInfo->mount_root_path_,  "/package.json",  StatFileFromFileSystem::IsFile, false ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check file Off Archive /public/index.ejs" , appInfo->mount_root_path_,  "/public/index.ejs",  StatFileFromFileSystem::IsFile, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check file Off Archive /public/index.ejs" , appInfo->mount_root_path_,  "/public/index.ejs",  StatFileFromFileSystem::IsFile, false ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "ASync Stat check dir Off Archive /public/" , appInfo->mount_root_path_,  "/public/",  StatFileFromFileSystem::IsDirectory, true ) );
  appInfo->tests_.Add( new StatFileFromFileSystem( "Sync Stat check dir Off Archive /public/" , appInfo->mount_root_path_,  "/public/",  StatFileFromFileSystem::IsDirectory, false ) );

  #endif
}

}
