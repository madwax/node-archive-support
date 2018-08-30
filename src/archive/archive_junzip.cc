#include "archive/archive_junzip.h"
#include "archive/manager.h"

#include <uv.h>
#include <zlib.h>
#include <openssl/md5.h>

#include <ctime>
#include <cstring>

#include <locale>

#if defined( _WIN32 )
// pre gcc 5 this is not supported + we only need it for Windows 
#include <codecvt>

#include <Windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace archive
{

extern void SplitOnPathSeps( const std::string& text, std::vector< std::string >& result, bool& endsWithDirSep );

/// Zip files store datetime in DOS format so we need to convert to time_t
static void DOSToTimeT( time_t& resulting, uint16_t dosDate, uint16_t dosFile )
{
  struct tm timeIs;

  std::memset( &timeIs, 0, sizeof( struct tm ) );

  timeIs.tm_year = ((dosDate >> 9) & 127) + 1980 - 1900;
  timeIs.tm_mon = ((dosDate >> 5) & 15) - 1;
  timeIs.tm_mday = dosDate & 31;

  timeIs.tm_hour = (dosFile >> 11) & 31;
  timeIs.tm_min = (dosFile >> 5) & 63;
  timeIs.tm_sec = (dosFile << 1) & 62;

  resulting = std::mktime( &timeIs );
}

static void Convsert( uv_timespec_t &output, time_t input )
{
  output.tv_nsec = 0;
  
  if( input == 0 )
  {
    output.tv_sec = 0;
  }
  else
  {
    output.tv_sec = ( long )( input / 1000 );
  }
}

/*************************************************************************************************************/

void ArchiveFileJUnzip::Set( JZFileHeader* header )
{
  size_ = header->uncompressedSize;
  offset_ = header->offset;

  DOSToTimeT( lastModified_, header->lastModFileDate, header->lastModFileTime );
}

/*************************************************************************************************************/

void ArchiveDirJUnzip::Set( JZFileHeader* header )
{
  DOSToTimeT( lastModified_, header->lastModFileDate, header->lastModFileTime );
}


/*************************************************************************************************************/


ArchiveJUnzip::ArchiveJUnzip( Manager* manager, int archiveId, const std::string& mountPoint, const std::string& archiveFilePath )
  : Archive( manager, archiveId, mountPoint, archiveFilePath )
{
}

ArchiveJUnzip::~ArchiveJUnzip()
{
  // If we have mounted but not unmounted do it.
  if( zip_file_handle_ != nullptr )
  {
    Unmount();
  }
}

ArchiveDir* ArchiveJUnzip::Root()
{
  return static_cast< ArchiveDir* >( &root_ );
}

const std::string ArchiveJUnzip::CacheFilePath( const ArchiveFileJUnzip* file ) const
{
  return temp_path_ + std::string( "/" ) + std::to_string( file->archiveId_ ) + std::string( ".cache" );
}

void ArchiveJUnzip::Validate( ArchiveFileJUnzip* file )
{
  if( file->exstracted_ != ArchiveFileJUnzip::NotExtracted )
  {
    return;
  }

  file->exstracted_ = ArchiveFileJUnzip::Extracting;

	FILE* cache_file = nullptr;

  const std::string cacheFilePath = CacheFilePath( file );

#if defined(_WIN32)
	if( ::fopen_s( &cache_file, cacheFilePath.c_str(), "rb" ) )
	{
		cache_file = nullptr;
	}
#else
	cache_file = ::fopen( cacheFilePath.c_str(), "rb" );
#endif

  if( cache_file == nullptr )
  {
    std::printf( "Failed to validate extract cache filepath: %s\n", cacheFilePath.c_str() );
    file->exstracted_ = ArchiveFileJUnzip::NotExtracted;
    is_unsafe_ = true;
  }
  else
  {
    file->exstracted_ = ArchiveFileJUnzip::Extracted;
    fclose( cache_file );
  }
}

void ArchiveJUnzip::Extract( ArchiveFileJUnzip* file )
{
  // don't do work someone else has or is doing.
  if( file->exstracted_ != ArchiveFileJUnzip::NotExtracted )
  {
    return;
  }

  file->exstracted_ = ArchiveFileJUnzip::Extracting;

  size_t currentOffset = zip_file_handle_->tell( zip_file_handle_ );

  zip_file_handle_->seek( zip_file_handle_, file->offset_, SEEK_SET );

  JZFileHeader tmp;
  char fname[ 1024 ];

  jzReadLocalFileHeader( zip_file_handle_, &tmp, fname, 1023 );

  std::vector<char> buffer( tmp.uncompressedSize );

	if( jzReadData( zip_file_handle_, &tmp, buffer.data() ) == Z_OK )
	{
  	std::string cacheFilePath = CacheFilePath( file );

    //std::printf( " ---> Writing file: %s\n", cacheFilePath.c_str() );

		FILE* out = nullptr;

#if defined(_WIN32)
		if( ::fopen_s( &out, cacheFilePath.c_str(), "wb" ) )
		{
			out = nullptr;
		}
#else
		out = ::fopen( cacheFilePath.c_str(), "wb" );
#endif
		if( out == nullptr )
		{
      is_unsafe_ = true;
			std::printf( "Failed to extract cache filepath: %s\n", cacheFilePath.c_str() );

      file->exstracted_ = ArchiveFileJUnzip::NotExtracted;
		}
		else
		{
			std::fwrite( buffer.data(), tmp.uncompressedSize, 1, out );

			std::fclose( out );

      file->exstracted_ = ArchiveFileJUnzip::Extracted;
		}
	}
	else
	{
		std::printf( "Failed to Decompress file: %d\n", file->archiveId_ );
    file->exstracted_ = ArchiveFileJUnzip::NotExtracted;
	}

  zip_file_handle_->seek( zip_file_handle_, currentOffset, SEEK_SET );
}

int ArchiveJUnzip::AddEntry( JZFile* /*hZipFile*/, int archiveIndexNumber, JZFileHeader* fileHeader, const char* filename )
{
  //std::printf( "Index:%d Name:%s Offset:%d size:%d/%d\n", archiveIndexNumber, filename, fileHeader->offset, fileHeader->compressedSize, fileHeader->uncompressedSize );
  bool is_dir;

  std::vector< std::string > parts = Archive::SplitPath( filename, is_dir );

  ArchiveDir *node = Root();

  // don't use an iterator for this as know position is important.
  for( size_t i=0, sz=parts.size(); i<sz; ++i )
  {
    const std::string& name = parts[ i ];

    ArchiveDir* knownDir = node->FindDir( name );
    if( knownDir == nullptr )
    {
      // we need to add a dir or file.
      bool addAsFile = false;
      if( i == ( sz - 1 ) && is_dir == false )
      {
        addAsFile = true;
      }

      if( addAsFile == true )
      {
        ArchiveFileJUnzip* newFile = new ArchiveFileJUnzip();

        newFile->archiveId_ = archiveIndexNumber;
        newFile->Set( fileHeader );

        node->Add( name, newFile );

        // if we need to extract the file.
        if( extract_on_mount_ == true )
        {
          // do sync.
          Extract( newFile );
        }
        else
        {
          Validate( newFile );
        }

        return 1;
      }
      else
      {
        ArchiveDirJUnzip* newDir = new ArchiveDirJUnzip();

        node->Add( name, newDir );

        if( i != ( sz - 1 ) )
        {
          newDir->Set( fileHeader );
          node = newDir;
        }
      }
    }
    else
    {
      // next
      node = knownDir;
    }
  }

  return 1; // read next = 1 abort = 0
}

int ArchiveJUnzip::onMountEachFile( JZFile* hZipFile, int archivesFileIndex, JZFileHeader* header, char* filepath, void* pUser )
{
	ArchiveJUnzip* target = reinterpret_cast<ArchiveJUnzip*>( pUser );
	return target->AddEntry( hZipFile, archivesFileIndex, header, filepath );
}

ErrorCodes ArchiveJUnzip::Mount()
{
#if defined(_WIN32)
  if( ::fopen_s(&file_handle_, archive_filepath_.c_str(), "rb" ))
  {
    file_handle_ = nullptr;
  }
#else
  file_handle_ = ::fopen(archive_filepath_.c_str(), "rb");
#endif

  if(file_handle_ == nullptr)
  {
    return ErrorCodes::ArchiveNotFound;
  }

  // we use the archives md5 hash as id in the cache
  md5_hash_ = Archive::GetMD5(file_handle_);
  temp_path_ = manager_->CacheRoot() + std::string( "/" ) + md5_hash_;

  uv_fs_t test_dir;
  int error_code;
  int archiveExstracted = 0;

  error_code = ::uv_fs_stat(manager_->Loop(), &test_dir, temp_path_.c_str(), nullptr);
  if(error_code != 0)
  {
    archiveExstracted = 0;
  }
  else
  {
    archiveExstracted = 1;
  }

  ::uv_fs_req_cleanup( &test_dir );

  if(archiveExstracted == 0)
  {
		uv_fs_t mkdirRequest;

		// if we get to this point we need to create the holding folder for files we have to cache.
		error_code = ::uv_fs_mkdir(manager_->Loop(), &mkdirRequest, temp_path_.c_str(), 0777, nullptr);
		if(error_code < 0)
		{
			zip_file_handle_ = nullptr;
			file_handle_ = nullptr;

			return ErrorCodes::FailedToCreateCache;
		}

    extract_on_mount_ = true;
  }

	zip_file_handle_ = ::jzfile_from_stdio_file( file_handle_ );

  if( ::jzReadEndRecord( zip_file_handle_, &endRecord_ ) )
  {
    zip_file_handle_->close( zip_file_handle_ );
    zip_file_handle_ = nullptr;
    file_handle_ = nullptr;

    return ErrorCodes::ArchiveInvalid;
  }

	// we have the archive dir so time to create the cache.
	if( ::jzReadCentralDirectory( zip_file_handle_, &endRecord_, &ArchiveJUnzip::onMountEachFile, this ) )
	{
    zip_file_handle_->close( zip_file_handle_ );
    zip_file_handle_ = nullptr;
    file_handle_ = nullptr;

		return ErrorCodes::ArchiveInvalid;
	}

  return ErrorCodes::NoError;
}

bool ArchiveJUnzip::IsMounted()
{
  if( zip_file_handle_ == nullptr )
  {
    return false;
  }
  return true;
}

void ArchiveJUnzip::Unmount()
{
  if( zip_file_handle_ != nullptr )
  {
    zip_file_handle_->close( zip_file_handle_ );
    file_handle_ = nullptr;
    zip_file_handle_ = nullptr;
  }
}

struct ArchiveJUnzipExtractData
{
	const std::string& extract_to_root_;

	ArchiveJUnzipExtractData( const std::string& base_path ) : extract_to_root_( base_path )
	{
	};
};

static int ExtractToForEachEntry( JZFile* zip_file, int /*archive_index*/, JZFileHeader* header, char* filepath, void* pUser )
{
	int ret = 1;
	struct ArchiveJUnzipExtractData* info = reinterpret_cast< struct ArchiveJUnzipExtractData* >( pUser );

//	std::printf( "Index:%d Name:%s Offset:%d size:%d/%d\n", archive_index, filepath, header->offset, header->compressedSize, header->uncompressedSize );

#if defined(_WIN32)
	std::wstring_convert< std::codecvt_utf8< wchar_t >, wchar_t > convert;
	std::wstring true_filepath = convert.from_bytes( info->extract_to_root_ + std::string( "/" ) +  std::string( filepath ) );
#else
	std::string true_filepath = info->extract_to_root_ + std::string( "/" ) + std::string( filepath );
#endif

	if( true_filepath.at( true_filepath.length() - 1 ) == '/' )
	{
		// it's a dir
#if defined(_WIN32)
		::CreateDirectoryW( true_filepath.data(), NULL );
#else
		::mkdir( true_filepath.data() , 777 );
#endif
	}
	else
	{
		size_t bytes_left = header->uncompressedSize;

		std::vector< char > buffer( bytes_left );

		size_t currentFilePos = zip_file->tell( zip_file );

		zip_file->seek( zip_file, header->offset, SEEK_SET );

    JZFileHeader tmp;
    char fname[ 1024 ];

    jzReadLocalFileHeader( zip_file, &tmp, fname, 1023 );

		if( jzReadData( zip_file, header, buffer.data() ) == 0 )
		{
#if defined(_WIN32)
			FILE* hFile;
			::_wfopen_s( &hFile, true_filepath.data(), L"wb" );
#else
			FILE* hFile = std::fopen( true_filepath.data(), "wb" );
#endif

			if(hFile == nullptr)
			{
				// Error!
				return 0;
			}

			char* raw = buffer.data();

			while( bytes_left )
			{
				size_t written = std::fwrite( raw, 1, bytes_left, hFile );
				raw += written;
				bytes_left -= written;
			}

			std::fclose( hFile );
		}

		zip_file->seek( zip_file, currentFilePos, SEEK_SET );
	}

	return ret; // 1 = next
}

bool ArchiveJUnzip::ExtractTo( const std::string& archive_filepath, const std::string& extract_to_path )
{
	FILE* hFile = nullptr;

#if defined(_WIN32)
  if( ::fopen_s( &hFile, archive_filepath.c_str(), "rb" ) )
  {
    hFile = nullptr;
  }
#else
  hFile = ::fopen( archive_filepath.c_str(), "rb" );
#endif

	if( hFile == nullptr )
	{
		return false;
	}

  ArchiveJUnzipExtractData extra( extract_to_path );
	bool ret = false;

	JZFile* hZipFile = ::jzfile_from_stdio_file( hFile );
	if(hZipFile != nullptr)
	{
		JZEndRecord endRecord;

		if( !::jzReadEndRecord( hZipFile, &endRecord ) )
		{
			if( !::jzReadCentralDirectory( hZipFile, &endRecord, &archive::ExtractToForEachEntry, &extra ) )
			{
				ret = true;
			}
		}

		hZipFile->close( hZipFile );
	}
  return ret;
}

int ArchiveJUnzip::fs_stat( uv_loop_t* loop, uv_fs_t* req, const char* filePath )
{
  std::vector< std::string > parts = FilePathToParts( filePath );

  ArchiveItem* pTarget = Find( parts );
  int r = 0;
 
  if( pTarget == nullptr )
  {
    req->result = UV_ENOENT;
    req->ptr = nullptr;
  }
  else
  {
    req->result = 0;
    req->ptr = reinterpret_cast< void* >( &( req->statbuf ) );

    req->ptr = &req->statbuf;

    req->statbuf.st_dev = 0;
    req->statbuf.st_ino = 0;
    req->statbuf.st_gid = 0;
    req->statbuf.st_uid = 0;
    req->statbuf.st_mode = 0;

    if( pTarget->IsFile() )
    {
      ArchiveFile *pFile = static_cast< ArchiveFile* >( pTarget );

      Convsert( req->statbuf.st_atim, pFile->lastModified_ );
      Convsert( req->statbuf.st_ctim, pFile->lastModified_ );
      Convsert( req->statbuf.st_mtim, pFile->lastModified_ );
      Convsert( req->statbuf.st_birthtim, pFile->lastModified_ );

      req->statbuf.st_mode |= 0x8000;  // _S_IFREG or file

      req->statbuf.st_size = pFile->size_;
    }
    else
    {
      req->statbuf.st_mode |= 0x4000;  // _S_IFDIR
      req->statbuf.st_size = 0;
    }
  }

  if( req->cb == nullptr )
  {
    r = static_cast<int>(req->result);
  }
  else
  {
		// we have a request callback so it needs to be async.
    Schedule(loop, req);
  }

  return r;
}

std::string ArchiveJUnzip::CacheFilePath(const std::string& full_filepath)
{
  std::string ret;

  ArchiveItem* target_archive_item = Find(FilePathToParts(full_filepath.c_str()));

  if(target_archive_item != nullptr && target_archive_item->IsFile())
  {
    ArchiveFileJUnzip* juzip_file_item = static_cast<ArchiveFileJUnzip*>(target_archive_item);

    ret = CacheFilePath(juzip_file_item);
  }

  return ret;
}

int ArchiveJUnzip::fs_fstat(uv_loop_t* loop, uv_fs_t* req, uv_file real_fileId)
{
  int r = 0;

	req->flags = 0;

  OpenFiles::iterator found_entry = open_files_.find( real_fileId );

  if(found_entry == open_files_.end())
  {
    req->result = UV_ENOENT;
    req->ptr = nullptr;
  }
  else
  {
    req->result = 0;
    req->ptr = reinterpret_cast< void* >( &( req->statbuf ) );

    req->ptr = &req->statbuf;

    req->statbuf.st_dev = 0;
    req->statbuf.st_ino = 0;
    req->statbuf.st_gid = 0;
    req->statbuf.st_uid = 0;
    req->statbuf.st_mode = 0;

    if( found_entry->second.target_->IsFile() )
    {
      ArchiveFile *pFile = static_cast< ArchiveFile* >( found_entry->second.target_ );

      Convsert( req->statbuf.st_atim, pFile->lastModified_ );
      Convsert( req->statbuf.st_ctim, pFile->lastModified_ );
      Convsert( req->statbuf.st_mtim, pFile->lastModified_ );
      Convsert( req->statbuf.st_birthtim, pFile->lastModified_ );

      req->statbuf.st_mode |= 0x8000;  // _S_IFREG or file

      req->statbuf.st_size = pFile->size_;
    }
    else
    {
      req->statbuf.st_mode |= 0x4000;  // _S_IFDIR
      req->statbuf.st_size = 0;
    }
  }

  if(req->cb == nullptr)
  {
    r = static_cast<int>(req->result);
  }
  else
  {
    Schedule(loop, req);
  }

  return r;
}



void ArchiveJUnzip::fs_open_on( uv_fs_t* request )
{
  Shadow_uv_fs_t* true_request = static_cast< Shadow_uv_fs_t* >( request );

  ArchiveJUnzip* pThis = static_cast< ArchiveJUnzip* >( true_request->data );

  if( request->result > 0 )
  {
    OpenFileInfo info;

    // we are opened.
    info.target_ = static_cast< ArchiveFileJUnzip* >( true_request->target_ );
    info.real_fileId_ = ( uv_file )true_request->result;

    // insert into the open files table.
    pThis->open_files_.insert( std::pair<uv_file, OpenFileInfo>( ( uv_file )request->result, info ) );

#if defined(_WIN32)
		true_request->shadowing_request_->fs.info = request->fs.info;
#endif		

    // how to update the shadow.
    true_request->shadowing_request_->result = request->result;
  }
  else
  {
    // error!
    true_request->shadowing_request_->result =  request->result;
  }

	( *( true_request->shadowing_request_->cb ) )( true_request->shadowing_request_ );

  ::uv_fs_req_cleanup( request );

  delete true_request;
}

int ArchiveJUnzip::fs_open( uv_loop_t* loop, uv_fs_t* request, int flags, const char* filePath )
{
  int er = 0;
  int r = 0;

  std::vector< std::string > parts = FilePathToParts( filePath );

  request->result = 0;

#if defined( _WIN32 )
	std::memset( &request->fs.info, 0, sizeof( request->fs.info ) );
#endif

  // find the entry
  ArchiveItem* target_file_item = Find( parts );
	ArchiveFileJUnzip* zip_file_item = nullptr;

	std::string cache_filepath;

  // if pTarget is null or a dir then error out.
	if( target_file_item == nullptr )
	{
		request->result = UV_ENOENT;
	}
	else if( !target_file_item->IsFile() )
	{
		request->result = UV_ENOENT;
	}
	else 
	{
		// now the archive file.
		zip_file_item = static_cast< ArchiveFileJUnzip* >( target_file_item );
		if( zip_file_item->exstracted_ != ArchiveFileJUnzip::Extracted )
		{
		  request->result = UV_EIO;
		}
		else
		{
			cache_filepath = CacheFilePath( zip_file_item ); 
		}
	}

	// if an error
  if( request->result < 0 )
  {
    if( request->cb != nullptr )
    {
      // As fs_open_on needs a shadow request...
			Shadow_uv_fs_t *openRequest = new Shadow_uv_fs_t();
			
			openRequest->cb = &ArchiveJUnzip::fs_open_on;
			openRequest->data = this;
			openRequest->target_ = nullptr;
			openRequest->shadowing_request_ = request;
			openRequest->result = request->result;

      // post away
      Schedule( loop, openRequest );
    }
    else
    {
      r = ( uv_file )request->result;
    }
    return r;
  }

  // async or sync
  if( request->cb == nullptr )
  {
    // sync so do the open and log the real file id and map it to a new fake.
    uv_fs_t req;

    er = ::uv_fs_open( loop, &req, cache_filepath.c_str(), flags, 0x0777, nullptr );

		request->result = er;
    r = er;

    if( er > 0 )
    {
#if defined(_WIN32)			
			request->fs.info = req.fs.info;
#endif
      // we need to add the opened file.
      OpenFileInfo fileInfo;

      fileInfo.target_ = zip_file_item;
      fileInfo.real_fileId_ = er;

      // insert into the open files table.
      open_files_.insert( std::pair<uv_file, OpenFileInfo>( er, fileInfo ) );
    }
  }
  else
  {
    // we are async
    // This is were things get a bit 'odd'
    // We need a request to open the file but we can't use the passed request as that has a set path variable set which is the filepath not the real filepath.
    // to get over this we open using another request and then join the two together.
    Shadow_uv_fs_t *openRequest = new Shadow_uv_fs_t();

    openRequest->data = this;
    openRequest->target_ = zip_file_item;
    openRequest->shadowing_request_ = request;

    // it's sync call.
    er = ::uv_fs_open( loop, openRequest, cache_filepath.c_str(), flags, 0x0777, &ArchiveJUnzip::fs_open_on );
    if( er < 0 )
    {
      delete openRequest;
      r = er;
    }
  }

  return r;
}

void ArchiveJUnzip::fs_read_on( uv_fs_t* request )
{
  // just pass through to the manager
  Manager::fs_read_on(request);
}

int ArchiveJUnzip::fs_read(uv_loop_t* loop, uv_fs_t* req, uv_file real_fileId, const uv_buf_t bufs[], unsigned int nbufs, int64_t offset )
{
  int r = 0;

  req->result = 0;

  // Get the file object.
  OpenFiles::iterator found_file_info = open_files_.find( real_fileId );
  if( found_file_info == open_files_.end() )
  {
    req->result = UV_EBADF;
	}
 
  if( req->cb == nullptr )
  {
    if( req->result == 0 )
    {
      // we can do the read.
      r = ::uv_fs_read( loop, req, real_fileId, bufs, nbufs, offset, nullptr );
		}
    else
    {
      r = ( int )req->result;
    }
  }
  else
  {
    if( req->result == 0 )
    {
      r = ::uv_fs_read( loop, req, real_fileId, bufs, nbufs, offset, &ArchiveJUnzip::fs_read_on );
    }
    else
    {
      req->cb = ArchiveJUnzip::fs_read_on;
      Schedule( loop, req );
    }
  }

  return r;
}

void ArchiveJUnzip::fs_close_on( uv_fs_t* request )
{
  Manager::fs_close_on( request );
}

int ArchiveJUnzip::fs_close( uv_loop_t* loop, uv_fs_t* req, uv_file real_fileId )
{
  int r = 0;

  req->result = 0;

  // Get the file object.
  OpenFiles::iterator found_file_info = open_files_.find( real_fileId );
  if( found_file_info == open_files_.end() )
  {
    req->result = UV_EBADF;
  }
  else
  {
		// Remove the local mapping
    open_files_.erase( found_file_info );
  }

  if( req->cb == nullptr )
  {
    if( req->result == 0 )
    {
      // we can do the read.
      r = ::uv_fs_close( loop, req, real_fileId, nullptr );
    }
    else
    {
      r = ( int )req->result;
    }
  }
  else
  {
    if( req->result == 0 )
    {
      r = ::uv_fs_close( loop, req, real_fileId, &ArchiveJUnzip::fs_close_on );
    }
    else
    {
      req->cb = ArchiveJUnzip::fs_close_on;
      Schedule( loop, req );
    }
  }

  return r;
}

int ArchiveJUnzip::fs_scandir(uv_loop_t* loop, uv_fs_t* request, const char* path, int /*flags*/)
{
  int r = 0;
	std::vector< std::string > path_parts = FilePathToParts( path );

  ArchiveItem* target_item = Find( path_parts );
	if(target_item == nullptr)
	{
		request->result = UV_ENOENT;
	}
	else if(target_item->IsFile())
	{
		request->result = UV_ENOTDIR;
	}
	else
	{
		const size_t pointer_dirent_sz = sizeof(uv__dirent_t*);

		ArchiveDirJUnzip* zip_dir_item = static_cast< ArchiveDirJUnzip* >(target_item);

		const size_t total_items = zip_dir_item->dirs_.size() + zip_dir_item->files_.size();

		request->result = total_items;

		if( total_items > 0 )
		{
			size_t current_index = 0;

			// alloc the array
			uv__dirent_t** results_array = reinterpret_cast<uv__dirent_t**>(scan_dir_alloc(pointer_dirent_sz * total_items));

			// now the fun bit begins.
			// first do the dir's

			const size_t size_of_direct_t = sizeof( uv__dirent_t );

			for( std::map< std::string, struct _ArchiveDir* >::const_iterator sub_dir_item=zip_dir_item->dirs_.begin(); sub_dir_item!=zip_dir_item->dirs_.end(); ++sub_dir_item, ++current_index )
			{
				size_t dirent_true_alloc_size = size_of_direct_t + ( sub_dir_item->first.length() + 1 );

				char* raw_data = static_cast< char* >( scan_dir_alloc( dirent_true_alloc_size ) );
				std::memset( raw_data, 0, dirent_true_alloc_size );

				uv__dirent_t* item = reinterpret_cast< uv__dirent_t* >( raw_data );
				results_array[ current_index ] = item;
				
				item->d_type = UV_DIRENT_DIR;

				char* name_start = item->d_name;
				
				size_t str_size = sub_dir_item->first.length();
#if defined( _WIN32 )
				strncpy_s(name_start, str_size + 1, sub_dir_item->first.c_str(), str_size);
#else
				std::strncpy(name_start, sub_dir_item->first.c_str(), str_size);
#endif
			}

			// now do the files
			for( std::map< std::string, struct _ArchiveFile* >::const_iterator sub_file_item=zip_dir_item->files_.begin(); sub_file_item!=zip_dir_item->files_.end(); ++sub_file_item, ++current_index )
			{
				size_t dirent_true_alloc_size = size_of_direct_t + ( sub_file_item->first.length() + 1 );

				char* raw_data = static_cast< char* >( scan_dir_alloc( dirent_true_alloc_size ) );
				std::memset( raw_data, 0, dirent_true_alloc_size );

				uv__dirent_t* item = reinterpret_cast< uv__dirent_t* >( raw_data );

				results_array[ current_index ] = item;

				item->d_type = UV_DIRENT_FILE;

				char* name_start = item->d_name;

				size_t str_size = sub_file_item->first.length();
#if defined( _WIN32 )
				strncpy_s(name_start, str_size + 1, sub_file_item->first.c_str(), str_size);
#else
				std::strncpy( name_start, sub_file_item->first.c_str(), str_size);
#endif
			}

			// set the needed flags.
	#if defined(_WIN32)
			request->flags |= EXT_UV_FS_FREE_PTR;
			request->fs.info.nbufs = 0;
	#else
			request->nbufs = 0;
	#endif
			request->ptr = results_array;
		}
		else
		{
			request->ptr = nullptr;

#if defined(_WIN32)
			request->fs.info.nbufs = 0;
#else
			request->nbufs = 0;
#endif
		}
	}

	if(request->cb == nullptr)
	{
		r = static_cast<int>(request->result);
	}
	else
	{
		Schedule(loop, request);
	}

  return r;
}

}
