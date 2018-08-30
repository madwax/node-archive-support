#ifndef SRC_TESTING_ARCHIVE_TEST_H__
#define SRC_TESTING_ARCHIVE_TEST_H__

#include "archive/manager.h"
#include "archive/archive_junzip.h"

namespace archive_test
{

class AsyncTests;

class AsyncTest
{
public:
  enum RunState 
  {
    NotRun = 0,
    Passed,
    Failed,
    Aborted,
  };

private:
  std::string name_;
  AsyncTests* tests_ = nullptr;
  RunState state_ = RunState::NotRun;
  uv_loop_t* loop_ = nullptr;

public: 
  AsyncTest( const char* name );
  virtual ~AsyncTest();

  uv_loop_t* Loop();

  const std::string& Name() const;
  const RunState State() const;

  virtual void Set( AsyncTests* tests, uv_loop_t* loop );

  virtual void Run() = 0;

  virtual void Finished( RunState state );
};

class AsyncTests
{
  std::vector< AsyncTest* > tests_;
  size_t current_test_ = 0;

  uv_loop_t* loop_ = nullptr;

  uv_async_t async_next_;

  bool failed_ = false;

  static void OnNext( uv_async_t* async );

  // does the clean up
  void Finish();

public:
  AsyncTests();
  virtual ~AsyncTests();

  void Add( AsyncTest* test );


  void Run( uv_loop_t* loop );

  void Done( AsyncTest* finished_test, bool has_failed );

};



typedef struct
{
  /// The common paths
  std::string dir_root_path_;
  std::string cache_root_path_;
  std::string extracted_root_path_;
  std::string mount_root_path_;

  AsyncTests tests_;

} AppInfo;

extern AppInfo* the_application_info;

int Start(int argc, char** argv );

}

#endif /* SRC_TESTING_ARCHIVE_TEST_H__ */

