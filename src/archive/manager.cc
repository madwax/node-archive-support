#include "archive/manager.h"
#include "archive/archive_junzip.h"

#include <cstring>
#include <cstdarg>

namespace archive
{

#if defined(_WIN32)
static const char* flatten_path(const char* in)
{
  if( in[0] == '\\' && in[2] == '\\' && in[2] == '?' && in[3] == '\\')
  {
    return (in + 4);
  }
  return in;
}

#define FLATTEN_PATH(unflattened_path) flatten_path(unflattened_path)
#else
#define FLATTEN_PATH(unflattened_path) unflattened_path 
#endif


/// The global manager object.
static Manager* gManager_ = nullptr;

Manager::Manager()
{
  gManager_ = this;
}

Manager::~Manager()
{

  if(report_wrappered_calls_!=nullptr && report_wrappered_calls_!=stdout)
  {
    std::fclose(report_wrappered_calls_);
    report_wrappered_calls_=nullptr;
  }

  gManager_ = nullptr;
}

bool Manager::Init(uv_loop_t* loop, int argc, char** argv )
{
  bool use_archive = false;
  std::string archive_path;
  std::string archive_mount;

  for(int i=0; i<argc; ++i)
  {
    char* item = argv[ i ];

    if(std::strcmp(item, "--archive.path") == 0)
    {
      use_archive = true;
      archive_path = argv[i+1];
    }
    else if(std::strcmp(item, "--archive.mount") == 0)
    {
      use_archive = true;
      archive_mount = argv[i+1];
    }
    else if(std::strcmp(item, "--archive.trace") == 0)
    {
      report_wrappered_calls_ = stdout;
    }
    else if(std::strcmp(item, "--archive.traceto") == 0)
    {
      report_wrappered_calls_ = std::fopen(argv[i+1], "w+");
      if(report_wrappered_calls_==nullptr)
      {
        std::fprintf(stderr, "Failed --archive.traceto as log file %s failed to be opened\n", argv[i+1]);
      }
    }
  }
  
  Bind(loop);

  if(use_archive)
  {
    if(archive_path.length() == 0)
    {
      std::fprintf(stderr, "You need to pass an archive using --archive.path\n");
      return false;
    }

    if(archive_mount.length() == 0)
    {
      std::fprintf(stderr, "You need to pass a mount point using --archive.mount\n");
      return false;
    }

    Report("Mounting archive:%s to mount:%s\n", archive_path.c_str(), archive_mount.c_str());

    if(Mount(archive_path, archive_mount)==false)
    {
      std::fprintf(stderr, "Failed to mount archive:%s to mount:%s\n", archive_path.c_str(), archive_mount.c_str());
      return false;
    }
  }
  return true;
}

bool Manager::BuildCacheDir( const std::string& path )
{
  if( cachesRoot_.length() == 0 && path.length() == 0 )
  {
    char tmp_holding[ 1024 ];

    // warning on unix this might point to /tmp
    size_t sz = 1024;
    uv_os_tmpdir( tmp_holding, &sz );

    cachesRoot_ = tmp_holding + std::string( "/archive_cache" );
  }

	if( path.length() != 0 )
	{
  	cachesRoot_ = path;
	}

  uv_fs_t re;
  int error_code = ::uv_fs_stat( loop_, &re, cachesRoot_.c_str(), nullptr );
  ::uv_fs_req_cleanup( &re );

  if( error_code != 0 )
  {
    error_code = ::uv_fs_mkdir( loop_, &re, cachesRoot_.c_str(), 0777, nullptr );
    ::uv_fs_req_cleanup( &re );

    if( error_code != 0 )
    {
      return false;
    }
  }

  return true;
}

bool Manager::SetCacheRoot( const std::string& cache_location_path )
{
  return BuildCacheDir( cache_location_path );
}

Mappings& Manager::KnownFiles()
{
  return knownFiles_;
}

Manager* Manager::Get()
{
  return gManager_;
}

void Manager::Release()
{
  for( Archives::iterator currentArchive=archives_.begin(); currentArchive!=archives_.end(); ++currentArchive )
  { 
    ( *currentArchive )->Unmount();
    delete ( *currentArchive );
  }

  archives_.clear();
}

/// Bind to the loop we want to host the manager and archives.
bool Manager::Bind( uv_loop_t* loop )
{
  loop_ = loop;

  // build the cache dir at this point.
  if( BuildCacheDir() == false )
  {
    return false;
  }

  // do we have any archives?
  for( Archives::iterator currentArchive=archives_.begin(); currentArchive!=archives_.end(); ++currentArchive )
  {
    if( ( *currentArchive )->IsMounted() == false )
    {
      ErrorCodes er = ( *currentArchive )->Mount();
      if( er != ErrorCodes::NoError )
      {
        // TODO report to stderr like the rest of nodes.
      }
    }
  }

  return true;
}

uv_loop_t* Manager::Loop()
{
  return loop_;
}

void Manager::Report(const char*msg, ...)
{
  if(report_wrappered_calls_==nullptr)
  {
    return;
  }

  va_list args;

  va_start(args, msg);

  std::vfprintf(report_wrappered_calls_, msg, args);

  va_end(args);
}


const std::string& Manager::CacheRoot() const
{
  return cachesRoot_; 
}

bool Manager::Mount( const std::string& archive_filepath, const std::string& mount_point )
{
  static int archive_id_counter = 1;

  // we call BuildCacheDir() just in case it was not called before.
  if( BuildCacheDir() == false )
  {
    return false;
  }

  ArchiveJUnzip* created_archive = new ArchiveJUnzip( this, archive_id_counter, mount_point, archive_filepath );

  ErrorCodes er = created_archive->Mount();
  if( er != ErrorCodes::NoError )
  {
    delete created_archive;
    return false;
  }

  this->archives_.push_back( created_archive );

  return true;
}

Archive* Manager::Find(const std::string& filepath)
{
  Archive* pTarget = nullptr;
  size_t mount_point_size = 0;
  size_t mount_start_point = 0;

#if defined(_WIN32)
  // node sometimes passes windows file paths as NT and not DOS paths.  e.g. \\?\C:\ vs c:
  if(filepath[0] == '\\' && filepath[1] == '\\' && filepath[2] == '?' && filepath[3] == '\\')
  {
    mount_start_point = 4;
  }
#endif

  for(Archives::iterator i=archives_.begin(); i!=archives_.end(); ++i)
  {
    const std::string& mount_point = (*i)->MountPoint();

    size_t x = filepath.find(mount_point);

    if(x == mount_start_point && mount_point.length() > mount_point_size)
    {
      pTarget = (*i);
      mount_point_size = mount_point.length();
    }
  }

  if(pTarget == nullptr)
  {
    return nullptr;
  }

  return pTarget;
}

std::string Manager::GetTrueFileName(const std::string& full_filepath)
{
  Archive* found_archive=Find(full_filepath);
  if(found_archive==nullptr)
  {
    return full_filepath;
  }

  return found_archive->CacheFilePath(full_filepath);
}

void Manager::Sheath( uv_fs_t* request, uv_fs_cb cb, uv_file fake, Archive* pArchive )
{
  RequestSheath* new_sheath = new RequestSheath();

  new_sheath->owner_ = this;
  new_sheath->fake_ = fake;
  new_sheath->pUserData_ = request->data;
  new_sheath->cb_ = cb;
  new_sheath->pArchive_ = pArchive;

  request->data = new_sheath;
}

void Manager::Unsheath( uv_fs_t* request )
{
  RequestSheath* sheath = reinterpret_cast< RequestSheath* >( request->data );

  request->data = sheath->pUserData_;

  delete sheath;
}

Manager* Manager::Unsheath( uv_fs_t* request, uv_fs_cb* pCb, uv_file& fake, Archive** ppArchive )
{
  RequestSheath* sheath = reinterpret_cast< RequestSheath* >( request->data );
  Manager* pManager = sheath->owner_;
  fake = sheath->fake_;

  *pCb = sheath->cb_;

  request->data = sheath->pUserData_;

  if( ppArchive != nullptr )
  {
    *ppArchive = sheath->pArchive_;
  }
 
  delete sheath;

  return pManager;
}

void Manager::fs_req_init( uv_loop_t* loop, uv_fs_t* request, uv_fs_type subType, const uv_fs_cb cb )
{
	// This is lifted from the platform version of libuv's fc.c file.  
	// under unix fs.c look for #define INIT() and then PATH() micros.

	#if defined(_WIN32)
  request->u.io.overlapped.Internal = 0;
  #endif
  
	request->fs_type =  subType;
  request->result = 0;
  request->ptr = NULL;
  request->loop = loop;
  request->path = NULL;
  request->flags = 0;

  #if defined(_WIN32)

	request->fs.info.bufs = request->fs.info.bufsml;

  #else
  request->new_path = NULL;
  request->bufs = request->bufsml;
  #endif

  request->cb = cb; 
}

int Manager::fs_capture_path( uv_fs_t* req, const char* path, const char* new_path, bool copy_path )
{
#if defined( _WIN32 )
  // Lifted from  fs__capture_path() in the windows code
  char* buf;
  char* pos;
  ssize_t buf_sz = 0, path_len = 0, pathw_len = 0, new_pathw_len = 0;

  /* new_path can only be set if path is also set. */
  if(new_path == NULL || path != NULL)
  {
  }

  if (path != NULL) {
    pathw_len = MultiByteToWideChar(CP_UTF8,
                                    0,
                                    path,
                                    -1,
                                    NULL,
                                    0);
    if (pathw_len == 0) {
      return GetLastError();
    }

    buf_sz += pathw_len * sizeof(WCHAR);
  }

  if (path != NULL && copy_path) {
    path_len = 1 + strlen(path);
    buf_sz += path_len;
  }

  if (new_path != NULL) {
    new_pathw_len = MultiByteToWideChar(CP_UTF8,
                                        0,
                                        new_path,
                                        -1,
                                        NULL,
                                        0);
    if (new_pathw_len == 0) {
      return GetLastError();
    }

    buf_sz += new_pathw_len * sizeof(WCHAR);
  }


  if (buf_sz == 0) {
    req->file.pathw = NULL;
    req->fs.info.new_pathw = NULL;
    req->path = NULL;
    return 0;
  }

  buf = (char*) uv__malloc(buf_sz);
  if (buf == NULL) {
    return ERROR_OUTOFMEMORY;
  }

  pos = buf;

  if (path != NULL) {
    DWORD r = MultiByteToWideChar(CP_UTF8,
                                  0,
                                  path,
                                  -1,
                                  (WCHAR*) pos,
                                  static_cast< int >( pathw_len ));

    req->file.pathw = (WCHAR*) pos;
    pos += r * sizeof(WCHAR);
  } else {
    req->file.pathw = NULL;
  }

  if (new_path != NULL) {
    DWORD r = MultiByteToWideChar(CP_UTF8,
                                  0,
                                  new_path,
                                  -1,
                                  (WCHAR*) pos,
                                  static_cast< int >( new_pathw_len ));

    req->fs.info.new_pathw = (WCHAR*) pos;
    pos += r * sizeof(WCHAR);
  } else {
    req->fs.info.new_pathw = NULL;
  }

  req->path = path;
  if (path != NULL && copy_path) {
    memcpy(pos, path, path_len);

    req->path = pos;
  }

  req->flags = EXT_UV_FS_FREE_PATHS;

#else

  if( path != NULL )
  {
    if( new_path != NULL )
    {
      // need to copy new_path and path see PATH2 micro in unix fs.c
      if (req->cb == NULL)
      {
        req->path = path;
        req->new_path = new_path;
      }
      else
      { 
        size_t path_len;
        size_t new_path_len;
        path_len = strlen(path) + 1;
        new_path_len = strlen(new_path) + 1;
        req->path = static_cast<const char*>(uv__malloc(path_len + new_path_len));
        if (req->path == NULL)
        {
          return UV_ENOMEM;
        }

        req->new_path = req->path + path_len;
        memcpy((void*) req->path, path, path_len);
        memcpy((void*) req->new_path, new_path, new_path_len);
      }
    }
    else
    {
      // just copy path see PATH micro in unix fs.c
      if (req->cb == NULL)
      {
        req->path = path;
        req->new_path = new_path;
      }
      else
      {
        req->path = uv__strdup( path );
        if (req->path == NULL )
        {
          return UV_ENOMEM;
        }
      }
    }
  }

#endif

  return 0;
}

void Manager::fs_req_cleanup( uv_fs_t* request )
{
  ::uv_fs_req_cleanup( request );
}

void Manager::fs_fstat_on(uv_fs_t* req)
{
  uv_fs_cb cb;
  uv_file fake_fileId;

  if(Get()->report_wrappered_calls_!=nullptr)
  {
    std::fprintf(Get()->report_wrappered_calls_, "@@ fs_fstat_on req:%p\n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath( req, &cb, fake_fileId, nullptr );

  cb( req );
}

int Manager::fs_fstat(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, uv_fs_cb cb)
{
  int r = 0;
  Mappings::RealSource source;

  if(Get()->report_wrappered_calls_!=nullptr)
  {
    std::fprintf(Get()->report_wrappered_calls_, "@@ fs_fstat loop:%p req:%p fakeId:%d\n", loop, req, fake_fileId );
  }

  req->result = 0;

  if( knownFiles_.Get( fake_fileId, source ) == false)
  {
    if(Get()->report_wrappered_calls_)
    {
      std::fprintf(stdout, "  @@ Failed to find internal entry\n");
    }

    req->result = UV_ENOENT;
  
    if( cb == nullptr )
    {
       r = ( int )req->result;
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      Schedule( loop, req );
    }
  }

  if( source.second != nullptr )
  {
    Archive* target = source.second;

    fs_req_init( loop, req, UV_FS_FSTAT, cb );

    if( cb != nullptr )
    {
      // if onStatCb is null we don't sheath the request.
      Sheath( req, cb, fake_fileId, target );
      req->cb = &Manager::fs_fstat_on;
    }

    req->result = 0;
    r = target->fs_fstat( loop, req, source.first );
  }
  else
  {
    // pass through
    if(cb != nullptr)
    {
      Sheath(req, cb, fake_fileId, nullptr);
      r = ::uv_fs_fstat( loop, req, source.first, &Manager::fs_fstat_on);
    }
    else
    {
      r = ::uv_fs_fstat( loop, req, source.first, nullptr );
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
  }

  return r;
}

void Manager::fs_stat_on( uv_fs_t* req )
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_stat_on req:%p\n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath(req, &cb, fake, nullptr );

  cb(req);
}

int Manager::fs_stat( uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb )
{
  int r = 0;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_stat loop:%p req:%p path:%s callback:%p\n", loop, req, path, cb);
  }

  Archive* pTarget = Find( path );
  if( pTarget == nullptr )
  {
    // it's a normal file.
    if( cb == nullptr )
    {
      r = ::uv_fs_stat( loop, req, path, nullptr );
    }
    else
    {
      Sheath( req, cb, 0, nullptr );
      r = ::uv_fs_stat( loop, req, path, &Manager::fs_stat_on );
    }
  }
  else
  {
    fs_req_init( loop, req, UV_FS_STAT, cb );
    fs_capture_path( req, path, nullptr, cb == nullptr );
    if( cb != nullptr )
    {
      // if onStatCb is null we don't sheath the request.
      Sheath( req, cb, 0, pTarget );
      req->cb = &Manager::fs_stat_on;
    }

    req->result = 0;
    r = pTarget->fs_stat( loop, req, path );
  }
  return r;
}


void Manager::fs_lstat_on( uv_fs_t* req )
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_stat_on req:%p\n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath(req, &cb, fake, nullptr );

  cb(req);
}

int Manager::fs_lstat( uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb )
{
  int r = 0;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_lstat loop:%p req:%p path:%s\n", loop, req, path);
  }

  Archive* pTarget = Find( path );
  if( pTarget == nullptr )
  {
    // it's a normal file.
    if( cb == nullptr )
    {
      r = ::uv_fs_lstat( loop, req, path, nullptr );
    }
    else
    {
      Sheath( req, cb, 0, nullptr );
      r = ::uv_fs_lstat( loop, req, path, &Manager::fs_stat_on );
    }
  }
  else
  {
    fs_req_init( loop, req, UV_FS_LSTAT, cb );
    fs_capture_path( req, path, nullptr, cb == nullptr );
    if( cb != nullptr )
    {
      // if onStatCb is null we don't sheath the request.
      Sheath( req, cb, 0, pTarget );
      req->cb = &Manager::fs_stat_on;
    }

    req->result = 0;
    r = pTarget->fs_stat( loop, req, path );
  }
  return r;
}


void Manager::fs_realpath_on( uv_fs_t* req )
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_realpath_on req:%p \n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath(req, &cb, fake, nullptr);

  cb(req);
}

int Manager::fs_realpath( uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb )
{
  int r = 0;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_realpath loop:%p req:%p path:%s\n", loop, req, path);
  }

  Archive* pTarget = Find( path );
  if( pTarget == nullptr )
  {
    // it's a normal file.
    if( cb == nullptr )
    {
      r = ::uv_fs_realpath( loop, req, path, nullptr );
    }
    else
    {
      Sheath( req, cb, 0, nullptr );
      r = ::uv_fs_realpath( loop, req, path, &Manager::fs_realpath_on );
    }
  }
  else
  {
    fs_req_init( loop, req, UV_FS_REALPATH, cb );
    fs_capture_path( req, path, nullptr, cb == nullptr );

    // req->path - holds the source
    // req->ptr - holds the result.
    // we cheat!
    char* dup = uv__strdup(path);
    req->ptr = dup;
    req->result = 0;

    if( cb != nullptr )
    {
      // if onStatCb is null we don't sheath the request.
      Sheath( req, cb, 0, pTarget );
      req->cb = &Manager::fs_realpath_on;

      Schedule(loop, req);
    }
    else
    {
      r = static_cast<int>(req->result);
    }
  }
  return r;
}

void Manager::fs_open_on(uv_fs_t* req)
{
  uv_fs_cb cb;
  uv_file fake;
  Archive* target_archive = nullptr;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_open_on req:%p\n", req);
  }

  Manager* manager = Manager::Unsheath( req, &cb, fake, &target_archive );

  // we have a real file Id
  if( req->result >= 0 )
  {
    // first create the mapping between real and fake file id.
    uv_file real_fileId = static_cast< uv_file >(req->result);
    uv_file fake_fileId = manager->knownFiles_.NextFakeId();

    manager->knownFiles_.Insert(fake_fileId, real_fileId, target_archive);

		// under windows don't set this...
    //request->file.fd = fake_fileId;
    req->result = fake_fileId;
  }

  cb(req);
}

int Manager::fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode, uv_fs_cb cb )
{
  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_open loop:%p req:%p path:%s\n", loop, req, path);
  }

  int r = 0;
  Archive* target_archive = Find( path );

  if( target_archive == nullptr )
  {
    // it's a normal file.
    if( cb == nullptr )
    {
      // we are sync.
      r = ::uv_fs_open( loop, req, path, flags, mode, nullptr );   
      if( r > 0 )
      {
        uv_file fake_fileId = knownFiles_.NextFakeId();

        knownFiles_.Insert( fake_fileId, static_cast< uv_file >( req->result ), nullptr );

        req->result = fake_fileId;
				r = fake_fileId;
      }
    }
    else
    {
      Sheath( req, cb, 0, nullptr );
      r = ::uv_fs_open( loop, req, path, flags, mode, &Manager::fs_open_on );
    }
  }
  else
  {
    fs_req_init( loop, req, UV_FS_OPEN, &Manager::fs_open_on );
    fs_capture_path( req, path, nullptr, cb == nullptr );

    if( cb == nullptr )
    {
			req->cb = nullptr; // null the callback.

      r = target_archive->fs_open( loop, req, flags, path );
			if( r > 0 )
			{
				uv_file fake_fileId = knownFiles_.NextFakeId();
				uv_file real_fileId = static_cast< uv_file >( req->result );

				knownFiles_.Insert( fake_fileId, real_fileId, target_archive );

				req->result = fake_fileId;
				r = fake_fileId;
			}
    }
    else
    {
      Sheath( req, cb, 0, target_archive );
      r = target_archive->fs_open( loop, req, flags, FLATTEN_PATH(path) );
    }
  }

  return r;
}

void Manager::fs_read_on( uv_fs_t* req )
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_read_on req:%p\n", req);
  }

  Manager::Unsheath(req, &cb, fake, nullptr);

  cb(req);
}

int Manager::fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb on_read_cb )
{
  int r = 0;
  Mappings::RealSource source;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_read loop:%p req:%p fakeId:%d\n", loop, req, fake_fileId);
  }

  if( knownFiles_.Get( fake_fileId, source ) == false )
  {
    if( on_read_cb != nullptr )
    {
      req->result = UV_ENOENT;
      req->cb = on_read_cb;

      Schedule( loop, req );

      return 0;
    }

    return UV_ENOENT;
  }

  if( source.second != nullptr )
  {
    Archive* target_archive = source.second;

    if( on_read_cb == nullptr )
    {
      fs_req_init( loop, req, UV_FS_READ, nullptr );

      r = target_archive->fs_read( loop, req, source.first, bufs, nbufs, offset );
      
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
    else
    {
      fs_req_init( loop, req, UV_FS_READ, &Manager::fs_read_on );

      Sheath( req, on_read_cb, fake_fileId, nullptr );
      r = target_archive->fs_read( loop, req, source.first, bufs, nbufs, offset );
    }
  }
  else
  {
    if( on_read_cb == nullptr )
    {
      r = ::uv_fs_read( loop, req, source.first, bufs, nbufs, offset, nullptr );
      
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
    else
    {
      Sheath( req, on_read_cb, fake_fileId, nullptr );
      r = ::uv_fs_read( loop, req, source.first, bufs, nbufs, offset, &Manager::fs_read_on );
    }
  }

  return r;
}

void Manager::fs_close_on(uv_fs_t* req)
{
  uv_file fake;
  uv_fs_cb cb;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_close_on req:%p \n", req);
  }

  Manager* pM = Manager::Unsheath(req, &cb, fake, nullptr);

  pM->knownFiles_.Remove(fake);

  cb(req);
}

int Manager::fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, uv_fs_cb on_close_cb )
{
  int r = 0;
  Mappings::RealSource source;
  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_close loop:%p req:%p fake_fileId:%d\n", loop, req, fake_fileId);
  }

  if( knownFiles_.Get( fake_fileId, source ) == false )
  {
    if( on_close_cb != nullptr )
    {
      req->result = UV_ENOENT;
      req->cb = on_close_cb;

      Schedule( loop, req );

      return 0;
    }

    return static_cast< int >( req->result );
  }

  if( source.second != nullptr )
  {
    Archive* target_archive = source.second;

    if( on_close_cb == nullptr )
    {
      fs_req_init(loop, req, UV_FS_CLOSE, nullptr);

      r = target_archive->fs_close( loop, req, source.first );
      //req->file.fd = fake_fileId;
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
      
			knownFiles_.Remove( fake_fileId );
    }
    else
    {
      fs_req_init(loop, req, UV_FS_CLOSE, &Manager::fs_close_on);

      Sheath(req, on_close_cb, fake_fileId, target_archive);
      r = target_archive->fs_close( loop, req, source.first );
    }
  }
  else
  {
    if( on_close_cb == nullptr )
    {
      r = ::uv_fs_close( loop, req, source.first, nullptr );
      //req->file.fd = fake_fileId;
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
			knownFiles_.Remove( fake_fileId );
    }
    else
    {
      Sheath( req, on_close_cb, fake_fileId, nullptr );
      r = ::uv_fs_close( loop, req, source.first, &Manager::fs_close_on );
    }
  }
  return r;
}

void Manager::fs_scandir_on( uv_fs_t* req )
{
  uv_file fake_fileId = 0;
  uv_fs_cb cb;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_scandir_on req:%p\n", req);
  }

  Manager::Unsheath( req, &cb, fake_fileId, nullptr );

  cb( req );
}

int Manager::fs_scandir(uv_loop_t* loop, uv_fs_t* req, const char* path,  int flags, uv_fs_cb cb)
{
  int r = 0;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_scandir loop:%p req:%p path:%s\n", loop, req, path);
  }

  Archive* target_archive = Find( path );

  if( target_archive == nullptr )
  {
    if( cb == nullptr )
    {
      r = ::uv_fs_scandir(loop, req, path, flags, nullptr);
    }
    else
    {
      Sheath(req, cb, 0, nullptr);
      r = ::uv_fs_scandir(loop, req, path, flags, &Manager::fs_scandir_on);
    }
  }
  else
  {
    fs_req_init( loop, req, UV_FS_SCANDIR, nullptr );
    fs_capture_path( req, path, nullptr, cb == nullptr );

    if( cb != nullptr )
    {
      Sheath(req, cb, 0, target_archive );
    
      // we want to run through the managers fs_scandir_on
      req->cb = &Manager::fs_scandir_on;
    }

    r = target_archive->fs_scandir(loop, req, FLATTEN_PATH(path), flags);
  }
  return r;
}

int Manager::fs_scandir_next(uv_fs_t* req, uv_dirent_t* ent)
{
  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_scandir_next req:%p dir:%p\n", req, ent);
  }

  return ::uv_fs_scandir_next(req, ent);
}

void Manager::fs_write_on(uv_fs_t* req)
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_write_on req:%p\n", req);
  }

  Manager::Unsheath(req, &cb, fake, nullptr);

  cb(req);
}

int Manager::fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb)
{
  int r = 0;
  Mappings::RealSource source;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_write loop:%p req:%p fakeId:%d\n", loop, req, fake_fileId);
  }

  if( knownFiles_.Get( fake_fileId, source ) == false )
  {
    if( cb != nullptr )
    {
      req->result = UV_ENOENT;
      req->cb = cb;

      Schedule( loop, req );

      return 0;
    }

    return UV_ENOENT;
  }

  if( source.second != nullptr )
  {
    Archive* target_archive = source.second;

    req->result = UV_ECANCELED;

    if( cb == nullptr )
    {
      r = static_cast<int>(req->result);
    }
    else
    {
      fs_req_init( loop, req, UV_FS_WRITE, &Manager::fs_write_on );
      Sheath( req, cb, fake_fileId, nullptr );
      Schedule(loop, req);
    }
  }
  else
  {
    if( cb == nullptr )
    {
      r = ::uv_fs_write( loop, req, source.first, bufs, nbufs, offset, nullptr );
      
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      r = ::uv_fs_write( loop, req, source.first, bufs, nbufs, offset, &Manager::fs_write_on );
    }
  }

  return r;
}

void Manager::fs_fsync_on(uv_fs_t* req)
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_fsync_on req:%p\n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath(req, &cb, fake, nullptr );

  cb(req);
}

int Manager::fs_fsync(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, uv_fs_cb cb)
{
  int r = 0;

  Mappings::RealSource source;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_fsync loop:%p req:%p fakeId:%d\n", loop, req, fake_fileId);
  }

  if( knownFiles_.Get( fake_fileId, source ) == false )
  {
    if( cb != nullptr )
    {
      req->result = UV_ENOENT;
      req->cb = cb;

      Schedule( loop, req );

      return 0;
    }

    return UV_ENOENT;
  }

  if( source.second != nullptr )
  {
    Archive* target_archive = source.second;

    req->result = 0;

    if( cb == nullptr )
    {
      r = static_cast<int>(req->result);
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      req->cb = &Manager::fs_fsync_on;
      Schedule(loop, req);
    }
  }
  else
  {
    if( cb == nullptr )
    {
      r = ::uv_fs_fsync( loop, req, source.first, nullptr );
      
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      r = ::uv_fs_fsync( loop, req, source.first, &Manager::fs_fsync_on );
    }
  }

  return r;
}

void Manager::fs_fdatasync_on(uv_fs_t* req)
{
  uv_fs_cb cb;
  uv_file fake;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_fdatasync_on req:%p\n", req);
  }

  // we don't need to know if this is a request from an archive or real file system.
  Manager::Unsheath(req, &cb, fake, nullptr );

  cb(req);
}

int Manager::fs_fdatasync(uv_loop_t* loop, uv_fs_t* req, uv_file fake_fileId, uv_fs_cb cb)
{
  int r = 0;

  Mappings::RealSource source;

  if(Get()->report_wrappered_calls_)
  {
    std::fprintf(stdout, "@@ fs_fsdataync loop:%p req:%p fakeId:%d\n", loop, req, fake_fileId);
  }

  if( knownFiles_.Get( fake_fileId, source ) == false )
  {
    if( cb != nullptr )
    {
      req->result = UV_ENOENT;
      req->cb = cb;

      Schedule( loop, req );

      return 0;
    }

    return UV_ENOENT;
  }

  if( source.second != nullptr )
  {
    Archive* target_archive = source.second;

    req->result = 0;

    if( cb == nullptr )
    {
      r = static_cast<int>(req->result);
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      req->cb = &Manager::fs_fdatasync_on;
      Schedule(loop, req);
    }
  }
  else
  {
    if( cb == nullptr )
    {
      r = ::uv_fs_fdatasync( loop, req, source.first, nullptr );
      
      SET_REQUEST_FILE_HANDLE(req, fake_fileId);
    }
    else
    {
      Sheath( req, cb, fake_fileId, nullptr );
      r = ::uv_fs_fdatasync( loop, req, source.first, &Manager::fs_fdatasync_on );
    }
  }

  return r;
}

int Manager::fs_ftruncate(uv_loop_t* loop, uv_fs_t* req, uv_file file, int64_t offset, uv_fs_cb cb)
{
  int r = 0;

  return r;
}

int Manager::fs_sendfile(uv_loop_t* loop, uv_fs_t* req, uv_file out_fd, uv_file in_fd, int64_t in_offset, size_t length, uv_fs_cb cb)
{
  int r = 0;

  return r;
}

int Manager::fs_futime(uv_loop_t* loop, uv_fs_t* req, uv_file file, double atime, double mtime, uv_fs_cb cb)
{
    int r = 0;

  return r;
}

int Manager::fs_fchmod(uv_loop_t* loop,uv_fs_t* req,uv_file file,int mode,uv_fs_cb cb)
{
    int r = 0;

  return r;
}

int Manager::fs_fchown(uv_loop_t* loop,  uv_fs_t* req,  uv_file file,  uv_uid_t uid,  uv_gid_t gid,  uv_fs_cb cb)
{
  int r = 0;

  return r;
}

/*************************************************************************************************************/

uv_fs_type uv_fs_get_type(const uv_fs_t* f)
{
  return ::uv_fs_get_type(f);
}

ssize_t uv_fs_get_result(const uv_fs_t* f)
{
  return ::uv_fs_get_result(f);
}

void* uv_fs_get_ptr(const uv_fs_t* f)
{
  return ::uv_fs_get_ptr(f);
}

const char* uv_fs_get_path(const uv_fs_t* f )
{
  return ::uv_fs_get_path(f);
}

uv_stat_t* uv_fs_get_statbuf(uv_fs_t* f)
{
  return ::uv_fs_get_statbuf(f);
} 

void uv_fs_req_cleanup(uv_fs_t* req)
{
  Manager::Get()->fs_req_cleanup(req);
}

int uv_fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file,  uv_fs_cb cb)
{
  return Manager::Get()->fs_close(loop, req, file, cb);
}

int uv_fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode, uv_fs_cb cb)
{
  return Manager::Get()->fs_open(loop, req, path, flags, mode, cb);
}

int uv_fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb)
{
  return Manager::Get()->fs_read(loop, req, file, bufs, nbufs, offset, cb);
}

int uv_fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb)
{
  return Manager::Get()->fs_write(loop, req, file, bufs, nbufs, offset, cb);
}

int uv_fs_fsync(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb)
{
  return Manager::Get()->fs_fsync(loop, req, file, cb);
}

int uv_fs_fdatasync(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb)
{
  return Manager::Get()->fs_fdatasync(loop, req, file, cb);
}

int uv_fs_ftruncate(uv_loop_t* loop, uv_fs_t* req, uv_file file, int64_t offset, uv_fs_cb cb)
{
  return Manager::Get()->fs_ftruncate( loop, req, file, offset, cb);
}

int uv_fs_sendfile(uv_loop_t* loop, uv_fs_t* req, uv_file out_fd, uv_file in_fd, int64_t in_offset, size_t length, uv_fs_cb cb)
{
  return Manager::Get()->fs_sendfile(loop, req, out_fd, in_fd, in_offset, length, cb);
}

int uv_fs_futime(uv_loop_t* loop, uv_fs_t* req, uv_file file, double atime, double mtime, uv_fs_cb cb)
{
  return Manager::Get()->fs_futime( loop, req, file, atime, mtime,cb);
}

int uv_fs_fchmod(uv_loop_t* loop,uv_fs_t* req,uv_file file,int mode,uv_fs_cb cb)
{
  return Manager::Get()->fs_fchmod(loop, req,file,mode, cb);
}

int uv_fs_fchown(uv_loop_t* loop,  uv_fs_t* req,  uv_file file,  uv_uid_t uid,  uv_gid_t gid,  uv_fs_cb cb)
{
  return Manager::Get()->fs_fchown( loop, req, file, uid, gid, cb);
}

int uv_fs_unlink(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_scandir path:%s\n", path );
  return ::uv_fs_unlink(loop, req, path, cb);
}

int uv_fs_copyfile(uv_loop_t* loop, uv_fs_t* req, const char* path, const char* new_path, int flags, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_scandir path:%s\n", path );
  return ::uv_fs_copyfile( loop, req, path, new_path, flags, cb);
}

int uv_fs_mkdir(uv_loop_t* loop, uv_fs_t* req, const char* path, int mode, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_mkdir path:%s\n", path );
  return ::uv_fs_mkdir(loop, req, path, mode, cb);
}

int uv_fs_mkdtemp(uv_loop_t* loop, uv_fs_t* req, const char* tpl, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_mkdtemp\n");
  return ::uv_fs_mkdtemp(loop, req, tpl, cb);
}

int uv_fs_rmdir(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_rmdir path:%s\n", path );
  return ::uv_fs_rmdir(loop, req, path, cb);
}

int uv_fs_scandir(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, uv_fs_cb cb)
{
  return Manager::Get()->fs_scandir(loop, req, path, flags, cb);
}

int uv_fs_scandir_next(uv_fs_t* req, uv_dirent_t* ent)
{
  return Manager::Get()->fs_scandir_next(req, ent);
}

int uv_fs_stat(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb)
{
  return Manager::Get()->fs_stat(loop, req, path, cb);
}

int uv_fs_lstat(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_fs_cb cb)
{
  return Manager::Get()->fs_lstat(loop, req, path, cb);
}

int uv_fs_fstat(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb)
{
  return Manager::Get()->fs_fstat(loop, req, file, cb);
}

int uv_fs_rename(uv_loop_t* loop, uv_fs_t* req, const char* path, const char* new_path, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_rename path:%s\n", path );
  return ::uv_fs_rename(loop, req, path, new_path, cb);
}

int uv_fs_access(uv_loop_t* loop, uv_fs_t* req, const char* path, int mode, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_access path:%s\n", path );
  return ::uv_fs_access(loop, req, path, mode, cb);
}

int uv_fs_chmod(uv_loop_t* loop, uv_fs_t* req, const char* path, int mode, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_chmod path:%s\n", path );
  return ::uv_fs_chmod(loop, req, path, mode, cb);
}

int uv_fs_utime(uv_loop_t* loop, uv_fs_t* req, const char* path, double atime, double mtime, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_utime path:%s\n", path );
  return ::uv_fs_utime(loop, req, path, atime, mtime, cb);
}

int uv_fs_link(uv_loop_t* loop,uv_fs_t* req,const char* path,const char* new_path,uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_link path:%s\n", path );
  return ::uv_fs_link(loop,req,path,new_path, cb);
}

int uv_fs_symlink(uv_loop_t* loop,uv_fs_t* req,const char* path,const char* new_path,int flags,uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_symlink path:%s\n", path );
  return ::uv_fs_symlink(loop,req,path,new_path,flags,cb);
}

int uv_fs_readlink(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_reallink path:%s\n", path );
  return ::uv_fs_readlink( loop, req, path, cb);
}

int uv_fs_realpath(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_realpath path:%s\n", path );
  return Manager::Get()->fs_realpath( loop, req, path, cb);
}

int uv_fs_chown(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_uid_t uid,uv_gid_t gid,uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_chown path:%s\n", path );
  return ::uv_fs_chown( loop,req,path, uid, gid, cb);
}

int uv_fs_lchown(uv_loop_t* loop,uv_fs_t* req, const char* path, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb)
{
std::fprintf( stdout, "## uv_fs_lchown path:%s\n", path );
  return ::uv_fs_lchown(loop, req, path, uid, gid, cb);
}

}
