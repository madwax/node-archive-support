#ifndef SRC_ARCHIVE_ARCHIVE_H_
#define SRC_ARCHIVE_ARCHIVE_H_

#include "archive/uv_schedule_delay.h"

#include <string>
#include <map>
#include <functional>

// Can't think of a better place for this currently.
// The uv_fs_t structure is different on Windows and UNIX with the main pain being the file handle 
// Under Windows is uv_fs_t.file.fd but on UNIX it's uv_fs_t.file. The following micros get around this
#if defined(_WIN32)

#define SET_REQUEST_FILE_HANDLE(req, handle) req->file.fd = handle
#define GET_REQUEST_FILE_HANDLE(req) req->file.fd

#else

#define SET_REQUEST_FILE_HANDLE(req, handle) req->file = handle;
#define GET_REQUEST_FILE_HANDLE(req) req->file

#endif


namespace archive
{

/// Common error codes used through archive
enum class ErrorCodes
{
  NoError = 0,
  ArchiveNotFound,
  ArchiveUnsupportedType,
  ArchiveInvalid,
  /// Failed to create the on disk cache folder.
  FailedToCreateCache,
};

/// The base for file/dir objects
typedef struct _ArchiveItem
{
  time_t lastModified_ = 0;
  int id_ = 0;

  virtual bool IsFile() const = 0;
} ArchiveItem;

/// The archive file object
typedef struct _ArchiveFile : public ArchiveItem
{
  // The size of the file
  uint32_t size_ = 0;

  bool IsFile() const { return true; }
} ArchiveFile;

/// The archive dir object
typedef struct _ArchiveDir : public ArchiveItem
{
  // map of all the child dirs known to this object
  std::map< std::string, struct _ArchiveDir* > dirs_;
  // map of all the child files known to this object
  std::map< std::string, ArchiveFile* > files_;

  bool IsFile() const { return false; }

	/// Used to add a dir to this dir object.
	int Add( const std::string& name, ArchiveFile* fileNode );
	int Add( const std::string& name, struct _ArchiveDir* dirNode );

  /// Helper used to look up a dir.
  /// If a dir with name is not found nullptr is returned.
  struct _ArchiveDir* FindDir( const std::string& name );

  /// Helper used to look up a file
  /// If a file with name is not found nullptr is returned
  ArchiveFile* FindFile( const std::string& name ); 
} ArchiveDir;

/// Forward for the Manager
class Manager;

/// The base archive object.
/// The Archive Manager uses these objects.
class Archive :public UvScheduleDelay
{
protected:
  /// Back pointer to the manager
  Manager* manager_ = nullptr;
  /// The id of the archive passed from the manager
  int id_ = 0;
  /// The mount point of the archive.
  std::string mount_point_;
  /// Were the archive is on the local file system.
  std::string archive_filepath_;
  /// The temp location were files are extracted to.
  std::string temp_path_;

  /// The root dir object.
  /// Derived classes must imp this and return the root dir object.  This is called when doing finds
  virtual ArchiveDir* Root() = 0;

  /// Use to find a given file/dir.
  /// If an item can not be found nullptr is returned.
  ArchiveItem* Find( const std::vector< std::string >& pathParts );

public:
  Archive( Manager* manager, int archiveId, const std::string& mountPoint, const std::string& archiveFilePath );
  virtual ~Archive();

  /// Returns the MD5 (as a string) of a file
  static std::string GetMD5( const std::string& filePath );
  static std::string GetMD5( FILE* hFile );

  // Splits a path up into the different parts
  static std::vector< std::string > SplitPath( const std::string& path, bool& does_ends_with_dir_seperator );

  /// returns the mount position
  const std::string& MountPoint() const;

  /// takes filepath - mount point and tokenises the result into a string vector.
  std::vector< std::string > FilePathToParts( const char* filePath );

  /// Test if the archive is mounted or not
  virtual bool IsMounted() = 0;

  /// Call this to load the archive.
  virtual ErrorCodes Mount() = 0;
  /// Call this to unmount the archive and release memory/files etc
  virtual void Unmount() = 0;

  /// Returns the cache filepath for a given filepath
  /// \param filepath This is the full filepath
  /// \return the filepath to the cache file or empty if a file entry can not be found.
  virtual std::string CacheFilePath(const std::string& full_filepath) = 0;


  /// Libuv stuff
  //@{
  virtual int fs_stat( uv_loop_t* loop, uv_fs_t* request, const char* filePath) = 0;
  virtual int fs_fstat(uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId) = 0;

  /// hFake = The file id to be used by the archive.
  virtual int fs_open( uv_loop_t* loop, uv_fs_t* request, int flags, const char* filePath) = 0;
  virtual int fs_read(uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset) = 0;
  virtual int fs_close( uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId) = 0;
  //@}

  virtual int fs_scandir(uv_loop_t* loop, uv_fs_t* request, const char* path, int flags) = 0;

};

}

#endif /* SRC_ARCHIVE_ARCHIVE_H_ */
