#include "archive/archive.h"
#include "archive/manager.h"

#include <cstdio>
#include <cstring>

#include <openssl/md5.h>

namespace archive
{

ArchiveDir* ArchiveDir::FindDir( const std::string& name )
{
  std::map< std::string, ArchiveDir* >::iterator foundItem = dirs_.find( name );
  if( foundItem == dirs_.end() )
  {
    return nullptr;
  } 
  return foundItem->second;
}

ArchiveFile* ArchiveDir::FindFile( const std::string& name )
{
  std::map< std::string, ArchiveFile* >::iterator foundItem = files_.find( name );
  if( foundItem == files_.end() )
  {
    return nullptr;
  } 
  return foundItem->second;
}

int ArchiveDir::Add( const std::string& name, ArchiveFile* file_node )
{
	files_.insert( std::pair< std::string, ArchiveFile* >( name, file_node ) );
	return 0;
}

int ArchiveDir::Add( const std::string& name, struct _ArchiveDir* dir_node )
{
	dirs_.insert( std::pair< std::string, ArchiveDir* >( name, dir_node ) );		
	return 0;
}

Archive::Archive( Manager* manager, int archiveId, const std::string& mount_point, const std::string& archive_filepath )
  : manager_(manager)
  , id_(archiveId)
  , mount_point_(mount_point)
  , archive_filepath_( archive_filepath )
{
}

Archive::~Archive()
{
}

std::string Archive::GetMD5( const std::string& filepath ) 
{
  FILE* file_handle = nullptr;

#if defined(_WIN32)
  if( ::fopen_s( &file_handle, filepath.c_str(), "rb" ) )
  {
    file_handle = nullptr;
  }
#else
  file_handle = std::fopen( filepath.c_str(), "rb" );
#endif

  if( file_handle == nullptr )
  {
    return std::string( "" );
  }

  std::string ret = Archive::GetMD5(file_handle);

  ::fclose( file_handle );

  return ret;
}

std::string Archive::GetMD5(FILE* file_handle)
{
  // we can only do this in C++11 
  unsigned char digest_buff[ MD5_DIGEST_LENGTH ];
  unsigned char read_buff[ 4096 ];
  size_t len;

  ::MD5_CTX contx;
  ::MD5_Init(&contx);

  while( ( len = fread( read_buff, 1, 4096, file_handle ) ) != 0 )
  {
    ::MD5_Update( &contx, read_buff, len );
  }

  ::MD5_Final( digest_buff, &contx );

  // reset the file pointer
  ::fseek( file_handle, 0, SEEK_SET );

  char string_buffer[ MD5_DIGEST_LENGTH * 3 ];
  std::memset( string_buffer, 0, MD5_DIGEST_LENGTH * 3 );

#if defined(_WIN32)
// sprintf!
#pragma warning(push)
#pragma warning(disable:4996)
#endif

  for( int i=0; i<MD5_DIGEST_LENGTH; ++i )
  {
    std::sprintf( ( string_buffer + ( i * 2 ) ), "%02x", digest_buff[ i ] );
  }

#if defined(_WIN32)
#pragma warning(pop)
#endif

  return std::string( string_buffer );
}

static std::size_t FindNextPathMarker( const std::string& str, const std::size_t offset )
{
#if defined(_WIN32)
  for( std::string::const_iterator current = ( str.begin() + offset ); current != str.end(); ++current )
  {
    if( *current == '/' || *current == '\\' )
    {
      return ( std::size_t )( current - str.begin() );
    }
  }

  return std::string::npos; 
#else
  return str.find( '/', offset );
#endif
}

std::vector< std::string > Archive::SplitPath( const std::string& text, bool& does_it_ends_with_dir_seperator )
{
  std::vector< std::string > ret;
  std::size_t start=0;
  std::size_t end=0;

  if( text.length() != 0 )
  {
#if defined(_WIN32)
    if( ( text.at( text.length() - 1 ) == '/' ) || ( text.at( text.length() - 1 ) == '\\' ) )
#else
    if( text.at( text.length() - 1 ) == '/' )
#endif
    {
      does_it_ends_with_dir_seperator = true;
    }
    else
    {
      does_it_ends_with_dir_seperator = false;
    }

    while( ( end = FindNextPathMarker( text, start ) ) != std::string::npos )
    {
      if( end != start )
      {
        ret.push_back( text.substr( start, end - start ) );
      }

      start = end + 1;
    }

		std::string tmp1 = text.substr( start );
		if(tmp1.size())
		{
			ret.push_back(tmp1);
		}
  }

  return ret;
}

/// returns the mount position
const std::string& Archive::MountPoint() const
{
  return mount_point_;
}

static size_t FindNextDirSeperator(const std::string& path, size_t starting_position)
{
	char* start = const_cast<char*>(path.c_str()) + starting_position;
	char* end = const_cast<char*>(path.c_str()) + path.length();

	while(start < end)
	{
		if(*start == '/')
		{
			return static_cast<size_t>(start - path.c_str());
		}
		else if(*start == '\\')
		{
			return static_cast<size_t>(start - path.c_str());
		}
		else
		{
			++start;
		}
	}

	return std::string::npos;
}

std::vector< std::string > Archive::FilePathToParts( const char* filepath )
{
#if defined(_WIN32)
	// passed in an NT path
	if( filepath[0] == '\\' && filepath[1] == '\\' && filepath[2] == '?' && filepath[3] == '\\')
	{
		filepath = filepath + 4;
	}
#endif

  std::vector< std::string > ret;
  std::string relativePath( filepath + mount_point_.length() );

  std::size_t start=0;
  std::size_t end=0;

  if( relativePath.length() != 0 )
  {
    while((end=FindNextDirSeperator(relativePath, start)) != std::string::npos)
    {
      if(end!=start)
      {
				const std::string tmp = relativePath.substr( start, end - start );
				if(tmp.length())
				{
					ret.push_back( tmp );
				}
      }

      start = end + 1;
    }

		const std::string tmp1 = relativePath.substr( start );
		if( tmp1.length() )
		{
			ret.push_back( tmp1 );
		}
  }

  return ret;
}

ArchiveItem* Archive::Find( const std::vector< std::string >& path_parts )
{
  ArchiveDir* dirNode = Root();
  ArchiveItem* ret = nullptr;

	// we need the root node.
	if( path_parts.size() == 0 )
	{
		return dirNode;
	}

  for( std::vector<std::string>::const_iterator i=path_parts.begin(); i!=path_parts.end(); ++i )
  {
    // we failed.
    if( dirNode == nullptr )
    {
      return nullptr;
    }

    ArchiveDir* sDir = dirNode->FindDir( (*i) );
    if( sDir == nullptr )
    {
      // file?
      ret = static_cast< ArchiveItem* >( dirNode->FindFile( (*i) ) );
      dirNode = nullptr;
    }
    else
    {
      dirNode = sDir;
      ret = static_cast< ArchiveItem* >( dirNode );
    }
  }
  
  return ret;
}

}
