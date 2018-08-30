
#include "archive.test.h"

#include <functional>
#include <cstdio>
#include <locale>
#include <codecvt>

#if defined( _MSC_VER )
#include <Windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace archive_test
{

AppInfo* the_application_info = nullptr;

// The different tests
extern void enum_dir_test_register( AppInfo* appInfo );
extern void file_load_test_register( AppInfo* appInfo );
extern void stat_test_register( AppInfo* appInfo );

static std::string GetOSTemp()
{
  // I know not the safest way but show me a temp
  char tmpPath[ 1024 ];
  size_t tmpPathSize = 1024;

  std::memset( tmpPath, 0, 1024 );

  uv_os_tmpdir( tmpPath, &tmpPathSize );

  return std::string( tmpPath );
}

static void MakePath( uv_loop_t* loop, const std::string& path )
{
	uv_fs_t re;
	int rt;

	// does it exist
	rt = ::uv_fs_stat( loop, &re, path.c_str(), nullptr );
	uv_fs_req_cleanup( &re );

	if( rt == 0 )
	{
		return;
	}

	// make the path
	uv_fs_mkdir( loop, &re, path.c_str(), 777, nullptr );
	uv_fs_req_cleanup( &re );
}

AsyncTest::AsyncTest( const char* name ) : name_( name )
{
}

AsyncTest::~AsyncTest()
{
}

const std::string& AsyncTest::Name() const
{
	return name_;
}

const AsyncTest::RunState AsyncTest::State() const
{
	return state_;
}

uv_loop_t* AsyncTest::Loop()
{
	return loop_;
}

void AsyncTest::Set( AsyncTests* tests, uv_loop_t* loop )
{	
	tests_ = tests;
	loop_ = loop;
}

void AsyncTest::Finished( AsyncTest::RunState state )
{
	state_ = state;

	bool failed = true;
	if( state == AsyncTest::RunState::Passed )
	{
		failed = false;
	}

	tests_->Done( this, failed );
}

AsyncTests::AsyncTests()
{
}

AsyncTests::~AsyncTests()
{
}

void AsyncTests::Add( AsyncTest* test_to_add )
{
	tests_.push_back( test_to_add );
}

void AsyncTests::OnNext( uv_async_t* async )
{	
	AsyncTests* pThis = reinterpret_cast< AsyncTests* >( async->data );

	if( pThis->current_test_ >= pThis->tests_.size() )
	{
		pThis->Finish();
		return;
	}

	AsyncTest* next_test = pThis->tests_.at( pThis->current_test_ );

	next_test->Set( pThis, async->loop );

	next_test->Run();
}

void AsyncTests::Run( uv_loop_t* loop )
{
	loop_ = loop;
	current_test_ = 0;

	uv_async_init( loop_, &async_next_, &AsyncTests::OnNext );
	async_next_.data = this;

	uv_async_send( &async_next_ );
}

void AsyncTests::Done( AsyncTest* finished_test, bool has_failed )
{
	if( has_failed )
	{
		failed_ = true;
		std::fprintf( stdout, "[ FAILED ] - '%s'\n", finished_test->Name().c_str() );
	}
	else
	{
		std::fprintf( stdout, "[ PASSED ] - '%s'\n", finished_test->Name().c_str() );

		++current_test_;
		if( current_test_ <= tests_.size() )
		{
			uv_async_send( &async_next_ );
			return;
		}
	}

	Finish();
}

void AsyncTests::Finish()
{
	uv_close( reinterpret_cast< uv_handle_t* >( &async_next_ ), nullptr );	

	if( failed_ )
	{
		std::fprintf( stdout, "Tests [ FAILED ]\n" );
	}
	else
	{
		std::fprintf( stdout, "Tests [ PASSED ]\n" );
	}
}

int Start(int argc, char** argv )
{
	AppInfo app_info;
	
	the_application_info = &app_info;

  uv_loop_t the_main_loop;
  std::string archive_filepath;

  for( int i=0; i<argc; ++i )
  {
    if( std::strcmp( "--archive", argv[ i ] ) == 0 )
    {
      archive_filepath = std::string( argv[ i + 1 ] );
    }
    else if( std::strcmp( "--help", argv[ i ] ) == 0 )
    {
      std::string( "Command Line options\n" );
      std::string( "  --archive %FILEPATH% - The location of the test archive\n" );
      std::string( "  --help - The help\n" );
      return 0;
    }
  }

  if( archive_filepath.length() == 0 )
  {
    std::printf( "You need to pass --archive FILEPATH\n" );
    return 1;
  }

  // create the temp location for the archive
  std::string archive_md5 = archive::Archive::GetMD5( archive_filepath );
  if( archive_md5.length() == 0 )
  {
    std::printf( "You need to pass a valid archive using --archive FILEPATH\n" );
    return 1;
  }

  uv_loop_init( &the_main_loop );

  archive::Manager the_archive_manager;

	// setup the different paths needed
	app_info.dir_root_path_ = GetOSTemp() + "/nat";
	app_info.cache_root_path_ = app_info.dir_root_path_ + "/cache";
	app_info.extracted_root_path_ = app_info.dir_root_path_ + "/ext";
	app_info.mount_root_path_ = app_info.dir_root_path_ + "/mnt";

	MakePath( &the_main_loop, app_info.dir_root_path_ );
	MakePath( &the_main_loop, app_info.cache_root_path_ );
	MakePath( &the_main_loop, app_info.extracted_root_path_ );

	// override the normal cache root location
	the_archive_manager.SetCacheRoot( app_info.cache_root_path_ );

	// bind the archive manager to the main loop.
	the_archive_manager.Bind( &the_main_loop );

	// mount an archive. This will take sometime as it extracts the archive to disk.
  the_archive_manager.Mount( archive_filepath, app_info.mount_root_path_ );

	// extract the archive file into extracted_root_path so we have a on file system copy to compare against.
	// archive::ArchiveJUnzip::ExtractTo( archive_filepath, app_info.extracted_root_path_ );

	// register the tests in order.
  //stat_test_register(&app_info);
	enum_dir_test_register(&app_info);
	//file_load_test_register(&app_info);

	app_info.tests_.Run( &the_main_loop );

  uv_run( &the_main_loop, UV_RUN_DEFAULT );

  the_archive_manager.Release();

  uv_loop_close( &the_main_loop );

	return 0;
}

}

#ifdef _WIN32
#include <windows.h>
#include <VersionHelpers.h>
#include <WinError.h>

int wmain(int argc, wchar_t* wargv[]) {
  if (!IsWindows7OrGreater()) {
    fprintf(stderr, "This application is only supported on Windows 7, "
                    "Windows Server 2008 R2, or higher.");
    exit(ERROR_EXE_MACHINE_TYPE_MISMATCH);
  }

  // Convert argv to UTF8
  char** argv = new char*[argc + 1];
  for (int i = 0; i < argc; i++) {
    // Compute the size of the required buffer
    DWORD size = WideCharToMultiByte(CP_UTF8,
                                     0,
                                     wargv[i],
                                     -1,
                                     nullptr,
                                     0,
                                     nullptr,
                                     nullptr);
    if (size == 0) {
      // This should never happen.
      fprintf(stderr, "Could not convert arguments to utf8.");
      exit(1);
    }
    // Do the actual conversion
    argv[i] = new char[size];
    DWORD result = WideCharToMultiByte(CP_UTF8,
                                       0,
                                       wargv[i],
                                       -1,
                                       argv[i],
                                       size,
                                       nullptr,
                                       nullptr);
    if (result == 0) {
      // This should never happen.
      fprintf(stderr, "Could not convert arguments to utf8.");
      exit(1);
    }
  }
  argv[argc] = nullptr;
  // Now that conversion is done, we can finally start.
  return archive_test::Start(argc, argv);
}
#else
// UNIX
#ifdef __linux__
#include <elf.h>
#ifdef __LP64__
#define Elf_auxv_t Elf64_auxv_t
#else
#define Elf_auxv_t Elf32_auxv_t
#endif  // __LP64__
extern char** environ;
#endif  // __linux__
#if defined(__POSIX__) && defined(NODE_SHARED_MODE)
#include <string.h>
#include <signal.h>
#endif

namespace node {
  extern bool linux_at_secure;
}  // namespace node

int main(int argc, char* argv[]) {
#if defined(__POSIX__) && defined(NODE_SHARED_MODE)
  // In node::PlatformInit(), we squash all signal handlers for non-shared lib
  // build. In order to run test cases against shared lib build, we also need
  // to do the same thing for shared lib build here, but only for SIGPIPE for
  // now. If node::PlatformInit() is moved to here, then this section could be
  // removed.
  {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, nullptr);
  }
#endif

#if defined(__linux__)
  char** envp = environ;
  while (*envp++ != nullptr) {}
  Elf_auxv_t* auxv = reinterpret_cast<Elf_auxv_t*>(envp);
  for (; auxv->a_type != AT_NULL; auxv++) {
    if (auxv->a_type == AT_SECURE) {
      node::linux_at_secure = auxv->a_un.a_val;
      break;
    }
  }
#endif
  // Disable stdio buffering, it interacts poorly with printf()
  // calls elsewhere in the program (e.g., any logging from V8.)
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  return Archive::Start(argc, argv);
}
#endif
