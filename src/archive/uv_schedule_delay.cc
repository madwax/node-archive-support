#include "archive/uv_schedule_delay.h"

namespace archive
{

UvScheduleDelay::UvScheduleDelay()
{
}

UvScheduleDelay::~UvScheduleDelay()
{
}

void UvScheduleDelay::OnProcessScheduleRequest(uv_async_t* async)
{
  UvScheduleDelay::ScheduleRequest* schedule_item = static_cast< UvScheduleDelay::ScheduleRequest* >( async );

	uv_fs_cb cb_ = schedule_item->request_->cb;

	( *cb_ )( schedule_item->request_ );

  uv_close( reinterpret_cast< uv_handle_t* >( async ), &UvScheduleDelay::OnCloseScheduleRequest );
}

void UvScheduleDelay::OnCloseScheduleRequest( uv_handle_t* handle )
{
  UvScheduleDelay::ScheduleRequest* schedule_request = reinterpret_cast< UvScheduleDelay::ScheduleRequest* >( handle );
  delete schedule_request;
}

void UvScheduleDelay::Schedule(uv_loop_t* owning_loop, uv_fs_t* reqeust)
{
  if(reqeust == nullptr)
  {
    return;
  }

  UvScheduleDelay::ScheduleRequest* new_schedule_item = new UvScheduleDelay::ScheduleRequest();

  new_schedule_item->data = this;
  new_schedule_item->request_ = reqeust;

  uv_async_init( owning_loop, new_schedule_item, &UvScheduleDelay::OnProcessScheduleRequest );

  uv_async_send( new_schedule_item ); 
}

}
