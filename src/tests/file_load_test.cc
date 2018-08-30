#include "archive.test.h"

#include <sys/stat.h>

namespace archive_test
{

using OnUnboundCB = std::function<void()>;
using OnFileLoadedCB = std::function<void(int errorCode, std::vector<char>& data )>;

class FileLoader
{
  #if defined( _WIN32 )
  static const ULONG MaxReadSize = ( 1024 * 4 );
  #else
  static const uint64_t MaxReadSize = ( 1024 * 4 );
  #endif

  uv_loop_t* m_loop;
  int m_hFile;
  std::string m_filePath;
  uint64_t m_fileLoaded;

  OnUnboundCB m_onUnboundCb;
  OnFileLoadedCB m_onLoadedCb;

  std::vector<char> m_data;

  uv_fs_t m_request;

  static void onClosed( uv_fs_t* request )
  {
    FileLoader* loader = static_cast<FileLoader*>( request->data );

    archive::uv_fs_req_cleanup( request );

    loader->m_fileLoaded = 0;

    (loader->m_onLoadedCb)( 0, loader->m_data );
  };

  static void onStat( uv_fs_t* request )
  {
    FileLoader* loader = static_cast<FileLoader*>( request->data );

    if( request->result < 0 )
    {
      int er = static_cast< int >(request->result);
      // clean up before we make the call just in case the object is deleted. 
      archive::uv_fs_req_cleanup( request );

      (loader->m_onLoadedCb)( er, loader->m_data );

      return;
    }

    uv_stat_t* statInfo = static_cast<uv_stat_t*>( request->ptr );

    // is it a file or not?
    if( ( statInfo->st_mode & S_IFMT ) != S_IFREG )
    {
      // clean up before we make the call just in case the object is deleted. 
      archive::uv_fs_req_cleanup( request );
      (loader->m_onLoadedCb)( -1, loader->m_data );
      return;
    }

    uint64_t fileSize = statInfo->st_size;

    archive::uv_fs_req_cleanup( request );

    if( fileSize == 0 )
    {
       (loader->m_onLoadedCb)( 0, loader->m_data );
       return;
    }

    // now open the file.
    loader->m_fileLoaded = 0;
    loader->m_data.resize( fileSize );

    loader->m_request.data = loader;

    int r = archive::uv_fs_open( loader->m_loop, &loader->m_request, loader->m_filePath.c_str(), O_RDONLY, 777, &FileLoader::onOpened );
    if( r < 0 )
    {
      (loader->m_onLoadedCb)( r, loader->m_data );
    }
  }

  static void onOpened( uv_fs_t* request )
  {
    FileLoader* loader = static_cast<FileLoader*>( request->data );

    loader->m_hFile = static_cast<int>(request->result);

    archive::uv_fs_req_cleanup( request );

    if( loader->m_hFile < 0 )
    {
      (loader->m_onLoadedCb)( loader->m_hFile, loader->m_data );
      loader->m_hFile = 0;
      return;
    }

    // Do the first read
    loader->read();
  }

  static void onRead( uv_fs_t* request )
  {
    FileLoader* loader = static_cast<FileLoader*>( request->data );

    ssize_t read = request->result;

    archive::uv_fs_req_cleanup( request );

    if( read < 0 )
    {
      // error!
      archive::uv_fs_close( loader->m_loop, &loader->m_request, loader->m_hFile, nullptr );

      (loader->m_onLoadedCb)( static_cast<int>(read), loader->m_data );
      return;
    }

    // move on.
    loader->m_fileLoaded += static_cast<uint64_t>( read );

    // If we have finished loading all the data the call to read() will handle it for us.
    loader->read();
  };

  void read()
  {
    m_request.data = this;

    if( m_data.size() == m_fileLoaded )
    {
      // time to close the file.
      m_request.data = this;

      int hFile = m_hFile;
      m_hFile = -1;

      archive::uv_fs_close( m_loop, &m_request, hFile, &FileLoader::onClosed );

      return;
    }

    uv_buf_t nextRead;

    nextRead.base = m_data.data() + m_fileLoaded;

    // libuv uses ULONG for len under windows!
    #if defined( _WIN32 )
    nextRead.len = static_cast<ULONG>( m_data.size() - m_fileLoaded );
    #else
    nextRead.len = static_cast<uint64_t>( m_data.size() - m_fileLoaded );
    #endif

    // clamp the read.
    if( nextRead.len > FileLoader::MaxReadSize )
    {
      nextRead.len = FileLoader::MaxReadSize;
    } 

    int r = archive::uv_fs_read( m_loop, &m_request, m_hFile, &nextRead, 1, m_fileLoaded, &FileLoader::onRead );
    if( r < 0 )
    {
      // error
      archive::uv_fs_close( m_loop, &m_request, m_hFile, nullptr );
      m_hFile = -1;

      m_onLoadedCb( r, m_data );
      return;
    }
  };

public:
  FileLoader() : m_loop(nullptr), m_hFile(-1), m_fileLoaded(0)
  {
    m_request.data = this;
  };

  ~FileLoader()
  {
  };

  void bind( uv_loop_t* loop, OnUnboundCB onUnboundCb )
  {
    m_loop = loop;
    m_onUnboundCb = onUnboundCb;
  };

  void unbind()
  { 
    if( m_loop != nullptr)
    {
      if( m_hFile >= 0 )
      {
        // close sync
        archive::uv_fs_close( m_loop, &m_request, m_hFile, nullptr );
        uv_fs_req_cleanup( &m_request );
        m_hFile = -1;
      }
      m_loop = nullptr;
    }

    m_onUnboundCb();
  };

  void load( const std::string& filePath, OnFileLoadedCB onLoaded )
  {
    if( m_loop == nullptr )
    {
      /// not bound
      onLoaded( UV_EINVAL, m_data );
      return;
    }

    m_filePath = filePath;
    m_onLoadedCb = onLoaded;

    m_request.data = this;

    int er = archive::uv_fs_stat( m_loop, &m_request, m_filePath.c_str(), &FileLoader::onStat );
    if( er < 0 )
    {
      onLoaded( er, m_data );
    }
  };

  static void load( uv_loop_t* loop, const std::string& filePath, OnFileLoadedCB onLoadedCb )
  {
    FileLoader* theFileLoader = new FileLoader();

    theFileLoader->bind(loop, [theFileLoader]()
    {
      // delete on unbind
      delete theFileLoader;
    } );

    theFileLoader->load( filePath, [onLoadedCb, theFileLoader](int er, std::vector<char>& data )
    {
      onLoadedCb( er, data );
      theFileLoader->unbind();
    } );
  };
};




class FileLoadTestFromDisk : public AsyncTest
{
  FileLoader loader_;
  bool passed_ = false;

  std::string base_path_;
  std::string filepath_;
  bool to_fail_ = false;

public:
  FileLoadTestFromDisk( const char* name, const std::string& base_path, const std::string& filepath, bool to_fail ) : AsyncTest( name )
  {
    base_path_ = base_path;
    filepath_ = filepath;
    to_fail_ = to_fail;
  }

  void OnUnbound()
  {
    AsyncTest::RunState state = AsyncTest::RunState::Failed;
    if( passed_ == true )
    {
      state = AsyncTest::RunState::Passed;
    }

    AsyncTest::Finished( state );
  }

  void OnLoaded( int inError, std::vector< char >& /*data*/ )
  {
    if( inError )
    {
      passed_ = false;
    }
    else 
    {
        passed_ = true;
    } 

    // we want it to fail.
    if( to_fail_ == true )
    {
      if( passed_ == true )
      {
        passed_ = false; 
      }
      else
      {
        passed_ = true;
      }
    }

    loader_.unbind();
  }

  void Run()
  {
    FileLoadTestFromDisk* pThis = this;

    loader_.bind( Loop(), [pThis]
    {
      pThis->OnUnbound();
    } );

    loader_.load( base_path_ + filepath_, [pThis]( int inError, std::vector< char >& data )
    {
      pThis->OnLoaded(inError, data );
    } );

  }
};

class SyncFileLoadTestFromDisk : public AsyncTest
{
  bool passed_ = false;

  std::string base_path_;
  std::string filepath_;
  bool to_fail_ = false;


  void DoLoad()
  {
    int r;
    uv_fs_t request;

    std::string target_filepath = base_path_ + filepath_;

    r = archive::uv_fs_open( Loop(), &request, target_filepath.c_str(), O_RDONLY, 0777, nullptr );
    archive::uv_fs_req_cleanup( &request );

    if( r < 0 )
    {
      return;
    }

    uv_file hfile = ( uv_file )r;

    std::vector< char > data;

    char holding_buffer[ 1024 ];
    uv_buf_t buf;

    buf.base = holding_buffer;
    buf.len = 1024;

    bool read_in = true;
    uint64_t offset = 0;

    while( read_in )
    {
      std::memset( holding_buffer, 0, 1024 );

      r = archive::uv_fs_read( Loop(), &request, hfile, &buf, 1, offset, nullptr );
      archive::uv_fs_req_cleanup( &request );

      if( r == 0 )
      {
        // its over.
        read_in = false;
      }
      else if( r > 0 )
      {
        // move on.
        offset += static_cast< uint64_t >( r );

        // append the loaded data
        data.insert( data.end(), ( holding_buffer + 0 ), ( holding_buffer + 0 ) + r );
      }
      else
      {
        read_in = false;
        if( r == UV_EOF )
        {
          // end of file which is ok
        }
        else
        {
          // something else happened.
          return;
        }
      }
    }

    archive::uv_fs_close( Loop(), &request, hfile, nullptr );
    archive::uv_fs_req_cleanup( &request );

    passed_ = true;
  };

public:
  SyncFileLoadTestFromDisk( const char* name, const std::string& base_path, const std::string& filepath, bool to_fail ) : AsyncTest( name )
  {
    base_path_ = base_path;
    filepath_ = filepath;
    to_fail_ = to_fail;
  }

  ~SyncFileLoadTestFromDisk()
  {
  }

  void Run()
  {
    DoLoad();

    if( to_fail_ == true )
    {
      if( passed_ )
      {
        passed_ = false;
      }
      else
      {
        passed_ = true;
      }
    }

    AsyncTest::RunState state = AsyncTest::RunState::Failed;
    if( passed_ == true )
    {
      state = AsyncTest::RunState::Passed;
    }
    AsyncTest::Finished( state );
  }
};

void file_load_test_register( AppInfo* appInfo )
{
  #if 1
  // Async loads
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load Off Disk /project.json - not a file", the_application_info->extracted_root_path_, "/project.json", true ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load Off Disk /package.json", the_application_info->extracted_root_path_, "/package.json", false ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load Off Disk /public/index.ejs", the_application_info->extracted_root_path_, "/public/index.ejs", false ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load Off Disk /public/unknown.ejs - not a file", the_application_info->extracted_root_path_, "/public/unknown.ejs", true ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load Off Disk /public/ - not a file", the_application_info->extracted_root_path_, "/public/", true ) );

  // Archive file load checks
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load From Archive /package.json", the_application_info->mount_root_path_, "/package.json", false ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load From Archive /project.json - not a file", the_application_info->mount_root_path_, "/project.json", true ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load From Archive /public/index.ejs", the_application_info->mount_root_path_, "/public/index.ejs", false ) );
  appInfo->tests_.Add( new FileLoadTestFromDisk( "Async File Load From Archive /public/unknown.ejs - not a file", the_application_info->mount_root_path_, "/public/unknown.ejs", true ) );
  #endif

  #if 1
  // Sync loads
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load Off Disk /package.json", the_application_info->extracted_root_path_, "/package.json", false ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load Off Disk /project.json", the_application_info->extracted_root_path_, "/project.json", true ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load Off Disk /public/index.ejs", the_application_info->extracted_root_path_, "/public/index.ejs", false ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load Off Disk /public/unknown.ejs", the_application_info->extracted_root_path_, "/public/unknown.ejs", true ) );

  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load From Archive /package.json", the_application_info->mount_root_path_, "/package.json", false ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load From Archive /project.json", the_application_info->mount_root_path_, "/project.json", true ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load From Archive /public/index.ejs", the_application_info->mount_root_path_, "/public/index.ejs", false ) );
  appInfo->tests_.Add( new SyncFileLoadTestFromDisk( "Sync File Load From Archive /public/unknown.ejs", the_application_info->mount_root_path_, "/public/unknown.ejs", true ) );
  #endif

}


}
