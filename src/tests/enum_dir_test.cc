#include "archive.test.h"

namespace archive_test
{

class EnumFileSystem : public AsyncTest
{
public:
  typedef struct
  {
    std::string name_;
    uv_dirent_type_t type_;
  } EnumEntry;

  using EnumEntries = std::vector< EnumEntry >;
  using EnumEntriesTest = bool (*)(int request_result, const EnumEntries& items);

private:
  std::string path_;
  bool is_async_ = true;
  EnumEntriesTest test_foreach_cb_ = nullptr;

  bool passed_ = false;
  uv_fs_t request_;

  void Report()
  {
    AsyncTest::RunState has_passed = AsyncTest::RunState::Failed;

    if( passed_ == true )
    {
      has_passed = AsyncTest::RunState::Passed;
    }
    AsyncTest::Finished( has_passed );
  }

  void Process( uv_fs_t* request )
  {
    // The enumerated items
    EnumEntries items_;

    if(request->result > 0)
    {
      uv_dirent_t item;
      int items_count = 0;

      while(UV_EOF != archive::uv_fs_scandir_next( request, &item ))
      {
        EnumEntry new_entry;
        new_entry.name_ = item.name;
        new_entry.type_ = item.type;

        items_.push_back( new_entry );

        ++items_count;
      }
    }
    
    if(test_foreach_cb_ == nullptr)
    {
      passed_ = true;
    }
    else
    {
      if((*test_foreach_cb_)( request->result, items_) == true)
      {
        passed_ = true;
      }
    }
  };

  void RunAsync()
  {
    request_.data = this;

    archive::uv_fs_scandir( Loop(), &request_, path_.c_str(), 0, [](uv_fs_t* request)
    {
      EnumFileSystem* pThis = reinterpret_cast<EnumFileSystem*>(request->data);

      pThis->Process(request);

      archive::uv_fs_req_cleanup(request);

      pThis->Report();
    } );
  }

  void RunSync()
  {
    int r = archive::uv_fs_scandir( Loop(), &request_, path_.c_str(), 0, nullptr );

    if(r == request_.result)
    {
      Process(&request_);
    }
    archive::uv_fs_req_cleanup(&request_);

    Report();
  }

public:
  EnumFileSystem( const char* name, const std::string& base_path, const std::string& sub_path, bool is_async, EnumEntriesTest test_foreach_cb) : AsyncTest( name )
  {
    path_ = base_path + sub_path;
    is_async_ = is_async;
    test_foreach_cb_ = test_foreach_cb;
  }

  ~EnumFileSystem()
  {
  }

  void Run()
  {
    if( is_async_ == true )
    {
      RunAsync();
    }
    else
    {
      RunSync();
    }
  }
};

static bool TestScanExistingFile(int request_result, const EnumFileSystem::EnumEntries& items)
{
  if(request_result == UV_ENOTDIR)
  {
    return true;
  }
  return false;
}

static bool TestScanNoneExistingFile(int request_result, const EnumFileSystem::EnumEntries& items)
{
  if(request_result == UV_ENOENT)
  {
    return true;
  }
  return false;
}

static bool TestPublicFolder(int request_result, const EnumFileSystem::EnumEntries& items)
{
  if(request_result == 2 && items.size() == 2)
  {
     return true;
  }
  return false;
}

/// The common enum for each tests.
static bool TestRootFolder(int request_result, const EnumFileSystem::EnumEntries& items)
{
  if(items.size() != 7)
  {
    return false;
  }

  int number_of_dirs = 0;
  int number_of_files = 0;
  int number_of_unknown = 0;

  for(EnumFileSystem::EnumEntries::const_iterator x=items.cbegin(); x!=items.cend(); ++x)
  {
    if(x->type_ == UV_DIRENT_DIR)
    {
      ++number_of_dirs;
    }
    else if(x->type_ == UV_DIRENT_FILE)
    {
      ++number_of_files;
    }
    else
    {
      ++number_of_unknown;
    }
  }

  if((number_of_dirs == 2) && (number_of_files == 5) && (number_of_unknown == 0))
  {
    return true;
  }

  return false;
}


void enum_dir_test_register(AppInfo* appInfo)
{
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of / off Disk" , appInfo->extracted_root_path_, "/", true, &TestRootFolder ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /package.json off Disk" , appInfo->extracted_root_path_, "/package.json", true, &TestScanExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /wibble off Disk" , appInfo->extracted_root_path_, "/wibble", true, &TestScanNoneExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /public off Disk" , appInfo->extracted_root_path_, "/public", true, &TestPublicFolder ) );

  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of / off Disk" , appInfo->extracted_root_path_, "/", false, &TestRootFolder ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /package.json off Disk" , appInfo->extracted_root_path_, "/package.json", false, &TestScanExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /wibble off Disk" , appInfo->extracted_root_path_, "/wibble", false, &TestScanNoneExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /public off Disk" , appInfo->extracted_root_path_, "/public", false, &TestPublicFolder ) );

  appInfo->tests_.Add( new EnumFileSystem( "Async enum of / from Archive" , appInfo->mount_root_path_, "/", true, &TestRootFolder ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /package.json from Archive" , appInfo->mount_root_path_, "/package.json", true, &TestScanExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /wibble from Archive" , appInfo->mount_root_path_, "/wibble", true, &TestScanNoneExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Async enum of /public from Archive" , appInfo->mount_root_path_, "/public", true, &TestPublicFolder ) );

  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /public  from Archive" , appInfo->mount_root_path_, "/public", false, &TestPublicFolder ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of / from Archive" , appInfo->mount_root_path_, "/", false, &TestRootFolder ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /package.json from Archive" , appInfo->mount_root_path_, "/package.json", false, &TestScanExistingFile ) );
  appInfo->tests_.Add( new EnumFileSystem( "Sync enum of /wibble from Archive" , appInfo->mount_root_path_, "/wibble", false, &TestScanNoneExistingFile ) );


}

}
