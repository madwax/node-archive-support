#ifndef SRC_ARCHIVE_MANAGER_H_
#define SRC_ARCHIVE_MANAGER_H_

#include <uv.h>

#include "archive/archive.h"

#include <map>

/// Note
/// We can't get at libuv's allocator, it's private and you can only set it.  So we use the same functions as the default version as only embedder (so people say) override it.
#define uv__malloc std::malloc

#if defined(_WIN32)
#define uv__strdup ::_strdup
#else
#define uv__strdup ::strdup
#endif

// Copies of internal libuv defines which we don't have access to.
// We add the prefix EXT_ so remove it and search the libuv codebase
#if defined( _WIN32 )

// from fs.c
#define EXT_UV_FS_FREE_PATHS         0x0002
#define EXT_UV_FS_FREE_PTR           0x0008

// the windows code uses the libuv allocator
#define scan_dir_alloc( size ) uv__malloc( size )
#else
// the unix version will always use malloc/free
#define scan_dir_alloc( size ) std::malloc( size )
#endif

/// Used to tell the outside world when something is not implemented
#define ARCHIVE_NOT_SUPPORTED( func, path ) std::cerr << "ARCHIVE: NOT SUPPORTED " << func << " for file:" << path << std::endl;
#define ARCHIVE_NOT_SUPPORTED_MAPPED( func, fakeId, realId ) std::cerr << "ARCHIVE: NOT SUPPORTED " << func << " for fike_fileId:" << fakeId << " real_fileId:" << realId << std::endl;

namespace archive
{

typedef struct
{
  Manager* owner_ = nullptr;
  uv_file fake_ = 0;
  void* pUserData_ = nullptr;
  uv_fs_cb cb_ = nullptr;
  Archive* pArchive_ = nullptr;
} RequestSheath;

class Mappings
{
public:
  using RealSource = std::pair< uv_file, Archive* >;
private:
  uv_file counter_ = 10;

  /// fake fileId => { real fileId or Archive* }
  std::map< uv_file, RealSource > known_;

public:
  Mappings()
  {
  };

  ~Mappings()
  {
  }

  uv_file NextFakeId()
  {
    uv_file r = counter_;
    ++counter_;
    if( counter_ < 10 )
    {
      counter_ = 10;
    }
    return r;
  }

  bool Get( uv_file fake_fileId, RealSource& gotten )
  {
    std::map< uv_file, RealSource >::iterator found = known_.find( fake_fileId );
    if( found != known_.end() )
    {
      gotten = found->second;

      return true;
    }
    return false;
  }

  uv_file GetRealFile( uv_file fake_fileId )
  {
    std::map< uv_file, RealSource >::iterator found = known_.find( fake_fileId );
    if( found != known_.end() )
    {
      return found->second.first;
    }
    return 0;
  }

  Archive* GetArchive( uv_file fake_fileId )
  {
    std::map< uv_file, RealSource >::iterator found = known_.find( fake_fileId );
    if( found != known_.end() )
    {
      return found->second.second;
    }
    return nullptr;
  }

  /// Inserts a real file Id(e.g. to be used by fopen et al) and returns a fake fileId
  uv_file Insert( uv_file fake_fileId, uv_file real_fileId, Archive* owning_archive )
  {
    known_.insert( std::pair< uv_file, RealSource >( fake_fileId, RealSource( real_fileId, owning_archive ) ) );
    return fake_fileId;
  }

  /// Inserts an Archive and returns a fake fileId
  uv_file Insert( uv_file fake_fileId, Archive* pArchive )
  {
    known_.insert( std::pair< uv_file, RealSource >( fake_fileId, RealSource( 0, pArchive ) ) );
    return fake_fileId;
  }

  /// Remove a fake file mapping
  void Remove( uv_file fake_fileId )
  {
    std::map< uv_file, RealSource >::iterator found = known_.find( fake_fileId );
    if( found != known_.end() )
    {
      known_.erase( found );
    }
  }
};

class Manager : protected UvScheduleDelay
{
  using Archives = std::vector< Archive* >;

  /// Use to report all wrapered uv_fs_* calls
  FILE* report_wrappered_calls_ = nullptr;

  /// The loop we are using
  uv_loop_t* loop_ = nullptr;

  /// The list of archives this Manager is controlling
  Archives archives_;

  /// Base of archives caches
  std::string cachesRoot_;

  /// The mapping table.
  Mappings knownFiles_;

  /// Used to find the archive that services passed path.
  /// If no mounted archive is found nullptr is returned and the file should exists on the local file system.
  /// \param filePath - The filepath the caller is looking for
  Archive* Find( const std::string& filePath );

  // used to build the cache dir.
  bool BuildCacheDir( const std::string& path = std::string() );

public:
  Manager();
  ~Manager();

  // called by node
  bool Init(uv_loop_t* loop, int argc, char** argv );

  /// Used to handle the sheathing of file requests
  /// Sheathing is used to translate fake file id's to real ones when dealing with real files (e.g. not in an archive)
  //@{
  /// Use this to sheath a request
  void Sheath( uv_fs_t* request, uv_fs_cb cb, uv_file fakeFileId, Archive* pArchive );

  void Unsheath( uv_fs_t* request );

  static Manager* Unsheath( uv_fs_t* request, uv_fs_cb* pCb, uv_file& fakeFileId, Archive** ppArchive );
  //@}

  Mappings& KnownFiles();

	void Report(const char*msg, ...);

  /// Get the global copy.
  static Manager* Get();

  /// Call this to shutdown the Manager
  /// This will unmount any archives and release any resources used.
  static void Shutdown();

	/// returns the cache root dir.
	const std::string& CacheRoot() const;

  // Set the cache directory if you want to.
  bool SetCacheRoot( const std::string& cache_location_path );

	bool Mount( const std::string& archiveFilePath, const std::string& mountPoint );
  
  /// Bind to the loop we want to host the manager and archives.
  bool Bind(uv_loop_t* loop);

  /// Returns the loop being used
  uv_loop_t* Loop();

  /// Shutdown call this to do all the dirty work
  void Release();

  /// Returns the true filepath of a file.
  /// If the file is in a mounted archive the cache file filepath is returned.  This is used for loading SO/Dylib/DLL
  std::string GetTrueFileName(const std::string& filepath);

  /// libuv file system proxy layer
  //@{

  /// Private part of the libuv file system proxy layer
  //@{

  // See fs__capture_path() in the windows version of fs.c or PATH/PATH2 in the Unix version
  int fs_capture_path(uv_fs_t* request, const char* path, const char* new_path, bool copyPath);

  static void fs_stat_on(uv_fs_t* request);
  static void fs_lstat_on(uv_fs_t* request);
  static void fs_fstat_on(uv_fs_t* request);

  static void fs_open_on(uv_fs_t* request);
  static void fs_read_on(uv_fs_t* request);
  static void fs_close_on(uv_fs_t* request);

  static void fs_scandir_on(uv_fs_t* request);
  static void fs_realpath_on(uv_fs_t* request);

  static void fs_write_on(uv_fs_t* request);
  static void fs_fsync_on(uv_fs_t* request);
  static void fs_fdatasync_on(uv_fs_t* request);

  //@}

  void fs_req_init(uv_loop_t* loop, uv_fs_t* request, uv_fs_type subType, const uv_fs_cb cb);
  void fs_req_cleanup(uv_fs_t* request);

  int fs_scandir(uv_loop_t* loop, uv_fs_t* req, const char* path,  int flags, uv_fs_cb cb);
  int fs_scandir_next(uv_fs_t* req, uv_dirent_t* ent);

  int fs_stat(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_fs_cb cb);
  int fs_lstat(uv_loop_t* loop,uv_fs_t* req,const char* path,uv_fs_cb cb);
  int fs_open(uv_loop_t* loop, uv_fs_t* req, const char* path, int flags, int mode, uv_fs_cb cb);
  int fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file hFake, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb);
  int fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file hFake,  uv_fs_cb cb);
  int fs_fstat(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb);
  int fs_realpath(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb);

  int fs_write(uv_loop_t* loop, uv_fs_t* req, uv_file file, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset, uv_fs_cb cb);
  int fs_fsync(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb);
  int fs_fdatasync(uv_loop_t* loop, uv_fs_t* req, uv_file file, uv_fs_cb cb);
  int fs_ftruncate(uv_loop_t* loop, uv_fs_t* req, uv_file file, int64_t offset, uv_fs_cb cb);
  int fs_sendfile(uv_loop_t* loop, uv_fs_t* req, uv_file out_fd, uv_file in_fd, int64_t in_offset, size_t length, uv_fs_cb cb);
  int fs_futime(uv_loop_t* loop, uv_fs_t* req, uv_file file, double atime, double mtime, uv_fs_cb cb);
  int fs_fchmod(uv_loop_t* loop,uv_fs_t* req,uv_file file,int mode,uv_fs_cb cb);
  int fs_fchown(uv_loop_t* loop,  uv_fs_t* req,  uv_file file,  uv_uid_t uid,  uv_gid_t gid,  uv_fs_cb cb);
  //@}
};


uv_fs_type uv_fs_get_type(const uv_fs_t*);
ssize_t uv_fs_get_result(const uv_fs_t*);
void* uv_fs_get_ptr(const uv_fs_t*);
const char* uv_fs_get_path(const uv_fs_t*);
uv_stat_t* uv_fs_get_statbuf(uv_fs_t*);
void uv_fs_req_cleanup(uv_fs_t* req);
int uv_fs_close(uv_loop_t* loop, uv_fs_t* req, uv_file file,  uv_fs_cb cb);
int uv_fs_open(uv_loop_t* loop,  uv_fs_t* req, const char* path, int flags, int mode,  uv_fs_cb cb);
int uv_fs_read(uv_loop_t* loop, uv_fs_t* req,  uv_file file, const uv_buf_t bufs[], unsigned int nbufs,   int64_t offset,  uv_fs_cb cb);
int uv_fs_unlink(uv_loop_t* loop, uv_fs_t* req,  const char* path,  uv_fs_cb cb);
int uv_fs_write(uv_loop_t* loop,  uv_fs_t* req,  uv_file file, const uv_buf_t bufs[], unsigned int nbufs,  int64_t offset, uv_fs_cb cb);
int uv_fs_copyfile(uv_loop_t* loop,  uv_fs_t* req, const char* path,  const char* new_path,  int flags,  uv_fs_cb cb);
int uv_fs_mkdir(uv_loop_t* loop, uv_fs_t* req, const char* path, int mode, uv_fs_cb cb);
int uv_fs_mkdtemp(uv_loop_t* loop, uv_fs_t* req, const char* tpl, uv_fs_cb cb);
int uv_fs_rmdir(uv_loop_t* loop, uv_fs_t* req, const char* path,  uv_fs_cb cb);
int uv_fs_scandir(uv_loop_t* loop, uv_fs_t* req, const char* path,  int flags, uv_fs_cb cb);
int uv_fs_scandir_next(uv_fs_t* req, uv_dirent_t* ent);
int uv_fs_stat(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb);
int uv_fs_fstat(uv_loop_t* loop, uv_fs_t* req,  uv_file file, uv_fs_cb cb);
int uv_fs_rename(uv_loop_t* loop, uv_fs_t* req, const char* path, const char* new_path,  uv_fs_cb cb);
int uv_fs_fsync(uv_loop_t* loop, uv_fs_t* req, uv_file file,  uv_fs_cb cb);
int uv_fs_fdatasync(uv_loop_t* loop, uv_fs_t* req,  uv_file file, uv_fs_cb cb);
int uv_fs_ftruncate(uv_loop_t* loop, uv_fs_t* req,  uv_file file, int64_t offset,  uv_fs_cb cb);
int uv_fs_sendfile(uv_loop_t* loop, uv_fs_t* req, uv_file out_fd, uv_file in_fd, int64_t in_offset,  size_t length,  uv_fs_cb cb);
int uv_fs_access(uv_loop_t* loop, uv_fs_t* req, const char* path, int mode, uv_fs_cb cb);
int uv_fs_chmod(uv_loop_t* loop, uv_fs_t* req,  const char* path, int mode, uv_fs_cb cb);
int uv_fs_utime(uv_loop_t* loop, uv_fs_t* req, const char* path,  double atime, double mtime, uv_fs_cb cb);
int uv_fs_futime(uv_loop_t* loop, uv_fs_t* req,  uv_file file, double atime, double mtime, uv_fs_cb cb);
int uv_fs_lstat(uv_loop_t* loop, uv_fs_t* req, const char* path, uv_fs_cb cb);
int uv_fs_link(uv_loop_t* loop, uv_fs_t* req, const char* path, const char* new_path, uv_fs_cb cb);
int uv_fs_symlink(uv_loop_t* loop, uv_fs_t* req, const char* path, const char* new_path, int flags, uv_fs_cb cb);
int uv_fs_readlink(uv_loop_t* loop, uv_fs_t* req,  const char* path, uv_fs_cb cb);
int uv_fs_realpath(uv_loop_t* loop, uv_fs_t* req,  const char* path, uv_fs_cb cb);
int uv_fs_fchmod(uv_loop_t* loop, uv_fs_t* req, uv_file file,  int mode,  uv_fs_cb cb);
int uv_fs_chown(uv_loop_t* loop, uv_fs_t* req,  const char* path, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb);
int uv_fs_fchown(uv_loop_t* loop, uv_fs_t* req,  uv_file file, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb);
int uv_fs_lchown(uv_loop_t* loop,uv_fs_t* req, const char* path, uv_uid_t uid, uv_gid_t gid, uv_fs_cb cb);
}

#endif /* SRC_ARCHIVE_MANAGER_H_ */
