/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by lianyu on 2022/10/29.
//

#include "storage/buffer/frame.h"
#include "session/thread_data.h"

FrameId::FrameId(int file_desc, PageNum page_num) : file_desc_(file_desc), page_num_(page_num)
{}

bool FrameId::equal_to(const FrameId &other) const
{
  return file_desc_ == other.file_desc_ && page_num_ == other.page_num_;
}

bool FrameId::operator==(const FrameId &other) const
{
  return this->equal_to(other);
}

size_t FrameId::hash() const
{
  return (static_cast<size_t>(file_desc_) << 32L) | page_num_;
}

int FrameId::file_desc() const
{
  return file_desc_;
}
PageNum FrameId::page_num() const
{
  return page_num_;
}

std::string to_string(const FrameId &frame_id)
{
  std::stringstream ss;
  ss << "fd:" << frame_id.file_desc() << ",page_num:" << frame_id.page_num();
  return ss.str();
}

////////////////////////////////////////////////////////////////////////////////
intptr_t get_default_debug_xid()
{
  ThreadData *thd = ThreadData::current();
  intptr_t xid = (thd == nullptr) ? 
                 // pthread_self的返回值类型是pthread_t，pthread_t在linux和mac上不同
                 // 在Linux上是一个整数类型，而在mac上是一个指针。为了能在两个平台上都编译通过，
                 // 就将pthread_self返回值转换两次
                 reinterpret_cast<intptr_t>(reinterpret_cast<void*>(pthread_self())) : 
                 reinterpret_cast<intptr_t>(thd);
  return xid;
}

void Frame::write_latch()
{
  write_latch(get_default_debug_xid());
}

void Frame::write_latch(intptr_t xid)
{
  {
    std::scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_.load() > 0,
           "frame lock. write lock failed while pin count is invalid. "
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(write_locker_ != xid,
           "frame lock write twice."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(read_lockers_.find(xid) == read_lockers_.end(),
           "frame lock write while holding the read lock."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());
  }

  lock_.lock();
  write_locker_ = xid;

  LOG_DEBUG("frame write lock success."
            "this=%p, pin=%d, pageNum=%d, write locker=%lx, fd=%d, xid=%lx, lbt=%s",
            this, pin_count_.load(), page_.page_num, write_locker_, file_desc_, xid, lbt());
  //    pthread_rwlock_wrlock(&rwlock_);
}

void Frame::write_unlatch()
{
  write_unlatch(get_default_debug_xid());
}

void Frame::write_unlatch(intptr_t xid)
{
  // 因为当前已经加着写锁，而且写锁只有一个，所以不再加debug_lock来做校验

  ASSERT(pin_count_.load() > 0, 
        "frame lock. write unlock failed while pin count is invalid."
        "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
         this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

  ASSERT(write_locker_ == xid,
         "frame unlock write while not the owner."
         "write_locker=%lx, this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
         write_locker_, this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

  LOG_DEBUG("frame write unlock success. this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
            this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

  write_locker_ = 0;
  lock_.unlock();
//    pthread_rwlock_unlock(&rwlock_);
}

void Frame::read_latch()
{
  read_latch(get_default_debug_xid());
}

void Frame::read_latch(intptr_t xid) 
{
  {
    std::scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_ > 0, "frame lock. read lock failed while pin count is invalid."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(read_lockers_.find(xid) == read_lockers_.end(),
           "frame lock read double times."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(xid != write_locker_,
           "frame lock read while holding the write lock."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    read_lockers_.insert(xid);
  }

  lock_.lock();
  LOG_DEBUG("frame read lock success."
            "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
            this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());
//    pthread_rwlock_rdlock(&rwlock_);
}

bool Frame::try_read_latch()
{
  intptr_t xid = get_default_debug_xid();
  {
    std::scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_ > 0, "frame try lock. read lock failed while pin count is invalid."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(read_lockers_.find(xid) == read_lockers_.end(),
           "frame try to lock read double times."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(xid != write_locker_,
           "frame try to lock read while holding the write lock."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());
  }

  bool ret = lock_.try_lock();
  if (ret) {
    debug_lock_.lock();
    read_lockers_.insert(xid);
    LOG_DEBUG("frame read lock success."
              "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
              this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());
    debug_lock_.unlock();
  }

  return ret;
}

void Frame::read_unlatch()
{
  read_unlatch(get_default_debug_xid());
}

void Frame::read_unlatch(intptr_t xid)
{
  {
    std::scoped_lock debug_lock(debug_lock_);
    ASSERT(pin_count_.load() > 0,
            "frame lock. read unlock failed while pin count is invalid."
            "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    ASSERT(read_lockers_.find(xid) != read_lockers_.end(),
           "frame unlock while not holding read lock."
           "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
           this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

    read_lockers_.erase(xid);
  }

  LOG_DEBUG("frame read unlock success."
            "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
            this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());

  lock_.unlock();
//    pthread_rwlock_unlock(&rwlock_);
}

void Frame::pin()
{
  std::scoped_lock debug_lock(debug_lock_);

  intptr_t xid = get_default_debug_xid();
  int pin_count = ++pin_count_;

  LOG_DEBUG("after frame pin. this=%p, write locker=%lx, read locker has xid %d? pin=%d, fd=%d, pageNum=%d, xid=%lx, lbt=%s",
            this, write_locker_, read_lockers_.find(xid) != read_lockers_.end(), 
            pin_count, file_desc_, page_.page_num, xid, lbt());
}

int Frame::unpin()
{
  intptr_t xid = get_default_debug_xid();

  ASSERT(pin_count_.load() > 0,
         "try to unpin a frame that pin count <= 0."
         "this=%p, pin=%d, pageNum=%d, fd=%d, xid=%lx, lbt=%s",
         this, pin_count_.load(), page_.page_num, file_desc_, xid, lbt());
  
  std::scoped_lock debug_lock(debug_lock_);

  int pin_count = --pin_count_;

  LOG_DEBUG("after frame unpin. "
            "this=%p, write locker=%lx, read locker has xid? %d, pin=%d, fd=%d, pageNum=%d, xid=%lx, lbt=%s",
            this, write_locker_, read_lockers_.find(xid) != read_lockers_.end(), 
            pin_count, file_desc_, page_.page_num, xid, lbt());
  
  if (0 == pin_count) {
    ASSERT(write_locker_ == 0,
           "frame unpin to 0 failed while someone hold the write lock. write locker=%lx, pageNum=%d, fd=%d, xid=%lx",
           write_locker_, page_.page_num, file_desc_, xid);
    ASSERT(read_lockers_.empty(),
           "frame unpin to 0 failed while someone hold the read locks. reader num=%d, pageNum=%d, fd=%d, xid=%lx",
           read_lockers_.size(), page_.page_num, file_desc_, xid);
  }
  return pin_count;
}


unsigned long current_time()
{
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec * 1000 * 1000 * 1000UL + tp.tv_nsec;
}

void Frame::access()
{
  acc_time_ = current_time();
}

std::string to_string(const Frame &frame)
{
  std::stringstream ss;
  ss << "frame id:" << to_string(frame.frame_id()) 
     << ", dirty=" << frame.dirty()
     << ", pin=" << frame.pin_count()
     << ", fd=" << frame.file_desc()
     << ", page num=" << frame.page_num()
     << ", lsn=" << frame.lsn();
  return ss.str();
}
