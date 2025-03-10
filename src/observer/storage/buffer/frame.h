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

#pragma once

#include <pthread.h>
#include <string.h>
#include <string>
#include <mutex>
#include <set>
#include <atomic>

#include "storage/buffer/page.h"
#include "common/log/log.h"
#include "common/lang/mutex.h"

class FrameId 
{
public:
  FrameId(int file_desc, PageNum page_num);
  bool    equal_to(const FrameId &other) const;
  bool    operator==(const FrameId &other) const;
  size_t  hash() const;
  int     file_desc() const;
  PageNum page_num() const;

  friend std::string to_string(const FrameId &frame_id);
private:
  int     file_desc_;
  PageNum page_num_;
};

class Frame
{
public:
  void clear_page()
  {
    memset(&page_, 0, sizeof(page_));
  }

  int     file_desc() const { return file_desc_; }
  void    set_file_desc(int fd) { file_desc_ = fd; }
  Page &  page() { return page_; }
  PageNum page_num() const { return page_.page_num; }
  void    set_page_num(PageNum page_num) { page_.page_num = page_num; }
  FrameId frame_id() const { return FrameId(file_desc_, page_.page_num); }
  LSN     lsn() const { return page_.lsn; }
  void    set_lsn(LSN lsn) { page_.lsn = lsn; }

  /// 刷新访问时间 TODO touch is better?
  void access();

  /**
   * 标记指定页面为“脏”页。如果修改了页面的内容，则应调用此函数，
   * 以便该页面被淘汰出缓冲区时系统将新的页面数据写入磁盘文件
   */
  void mark_dirty() { dirty_ = true; }
  void clear_dirty() { dirty_ = false; }
  bool dirty() const { return dirty_; }

  char *data() { return page_.data; }

  bool can_purge() { return pin_count_.load() == 0; }

  /**
   * 给当前页帧增加引用计数
   * pin通常都会加着frame manager锁来访问
   */
  void pin();

  /**
   * 释放一个当前页帧的引用计数
   * 与pin对应，但是通常不会加着frame manager的锁来访问
   */
  int  unpin();
  int  pin_count() const { return pin_count_.load(); }

  void write_latch();
  void write_latch(intptr_t xid);

  void write_unlatch();
  void write_unlatch(intptr_t xid);
  
  void read_latch();
  void read_latch(intptr_t xid);
  bool try_read_latch();

  void read_unlatch();
  void read_unlatch(intptr_t xid);

  friend std::string to_string(const Frame &frame);

private:
  friend class  BufferPool;

  bool              dirty_     = false;
  std::atomic<int>  pin_count_{0};
  unsigned long     acc_time_  = 0;
  int               file_desc_ = -1;
  Page              page_;

  //读写锁
  pthread_rwlock_t  rwlock_ = PTHREAD_RWLOCK_INITIALIZER;
  /// 在非并发编译时，加锁解锁动作将什么都不做
  common::Mutex     lock_;

  /// 使用一些手段来做测试，提前检测出头疼的死锁问题
  /// 如果编译时没有增加调试选项，这些代码什么都不做
  common::DebugMutex  debug_lock_;
  intptr_t            write_locker_ = 0;
  std::set<intptr_t>  read_lockers_;
};

