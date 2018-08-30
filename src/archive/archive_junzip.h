#ifndef SRC_ARCHIVE_ARCHIVE_JUNZIP_H_
#define SRC_ARCHIVE_ARCHIVE_JUNZIP_H_

#include "archive/archive.h"
#include "archive/junzip.h"
#include <map>
#include <vector>

namespace archive
{

// fwd define the zip archive file class
class ArchiveJUnzip;

// The JUnzip version of a archive file class
typedef struct _ArchiveFileJUnzip : public ArchiveFile
{
  // The current state of the file on disk
  enum ExtractStates
  {
    NotExtracted = 0,
    Extracting,
    Extracted
  };

  // The id of the file in the zip file
  int archiveId_ = 0;
  /// The offset in the zip file were this file belongs
  int offset_ = -1;
	/// If the file has been decompressed.
	ExtractStates exstracted_ = NotExtracted;

  // setter using the zip file header info
  void Set(JZFileHeader* header);
} ArchiveFileJUnzip;

/// The JUnzip version of an archive directory class
typedef struct _ArchiveDirJUnzip : public ArchiveDir
{
  // setter using the zip file header info
  void Set(JZFileHeader* header);  
} ArchiveDirJUnzip;

/// The JUnzip based Archive class
class ArchiveJUnzip : public Archive
{
  // struct used to keep track of open files in the archive
  typedef struct
  {
    // The archive item, this will always get a file but you never know in the future
    ArchiveItem* target_ = nullptr;
    // The real file id
    uv_file real_fileId_ = 0;
  } OpenFileInfo;

  // Some operations like open the passed uv_fs_t request does not in fact do the opening but one of these will and
  // we pass back all needed data of said request
  typedef struct : public uv_fs_t
  {
		ArchiveItem* target_ = nullptr;
		uv_file real_fileId = -1;
		uv_file fake_fileId = -1;
    uv_fs_t* shadowing_request_ = nullptr;
  } Shadow_uv_fs_t;

  using OpenFiles = std::map< uv_file, OpenFileInfo >;

  /// JUnzip uses fopen! fread et al.
  FILE* file_handle_ = nullptr;
  /// The JUnzip archive interface object
  JZFile* zip_file_handle_;
  /// The end record
  JZEndRecord endRecord_;
  /// The root dir
  ArchiveDirJUnzip root_;
  /// real file Id to OpenFileInfo.
  OpenFiles open_files_;
	/// Should this instance extract the archive on mount.
	bool extract_on_mount_ = false;
  /// the md5 hash of the archive file.
  std::string md5_hash_;
	/// Flag used to indecate there was a problem extracting the archive.
	bool is_unsafe_ = false;

  /// Returns the root dir object of the archive
  ArchiveDir* Root() override;

	/// Returns the filepath to the cache file for this file
	const std::string CacheFilePath(const ArchiveFileJUnzip* file) const;

  // Add a new zip file to the archive
	int AddEntry(JZFile* zip_file, int index, JZFileHeader* file_header, const char* filename );

  // Used to extract a file form the zip file and add it to the cache dir
  void Extract(ArchiveFileJUnzip* file);

	// Used to test the cache file for this file object is valid.
  // This is called during mounting if the cache is populated
	void Validate(ArchiveFileJUnzip* file);

  //Called when mounting a file 
	static int onMountEachFile(JZFile* zip_file, int archives_file_index, JZFileHeader* header, char* filepath, void* pUser);

  static void fs_open_on( uv_fs_t* request );
	static void fs_read_on( uv_fs_t* request );
	static void fs_close_on( uv_fs_t* request );

public:
  ArchiveJUnzip( Manager* manager, int archiveId, const std::string& mountPoint, const std::string& archiveFilePath );
  ~ArchiveJUnzip();

  bool IsMounted() override;

  /// Does the mount.
  /// Mounting is an synchronous process so blocks the thread.
  /// \return See the error codes
  ErrorCodes Mount() override;

  /// Does the unmount of the archive.
  void Unmount() override;

  /// Used to get a cache filepath from a true filepath
  std::string CacheFilePath(const std::string& full_filepath) override;

  /// Use to extract a zip archive file to extract_to_path
  /// \param extract_to_path  The root dir used to extract files to.
  /// \return true if the files were all extracted.
  static bool ExtractTo( const std::string& archive_filepath, const std::string& extract_to_path );

  /// libuv stuff
  //@{
  int fs_stat( uv_loop_t* loop, uv_fs_t* request, const char* filepath ) override;
  int fs_fstat(uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId) override;
  int fs_open( uv_loop_t* loop, uv_fs_t* request, int flags, const char* filepath ) override;
  int fs_read( uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset ) override;
  int fs_close( uv_loop_t* loop, uv_fs_t* request, uv_file real_fileId ) override;
  int fs_scandir(uv_loop_t* loop, uv_fs_t* request, const char* path, int flags) override;
  //@}
};


}

#endif /* SRC_ARCHIVE_ARCHIVE_JUNZIP_H_ */
