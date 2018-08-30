#ifndef SRC_ARCHIVE_UV_SCHEDULE_DELAY_H_
#define SRC_ARCHIVE_UV_SCHEDULE_DELAY_H_

#include <uv.h>

#include <vector>

namespace archive
{

/// Used to handle pending uv_fs_t
/// TODO - Think about atomics! node.js is currently mainly single threaded but worker threads are experimental at time of writing.
class UvScheduleDelay
{
  typedef struct : public uv_async_t
  {
    uv_fs_t* request_ = nullptr;
  } ScheduleRequest;

  static void OnProcessScheduleRequest(uv_async_t* check);
  static void OnCloseScheduleRequest(uv_handle_t* handle);

public:
  UvScheduleDelay();
  virtual ~UvScheduleDelay();

  void Schedule( uv_loop_t* owning_loop, uv_fs_t* request );
};

}

#endif /* SRC_ARCHIVE_UV_SCHEDULE_DELAY_H_ */
