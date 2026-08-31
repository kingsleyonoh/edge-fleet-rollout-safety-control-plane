#pragma once

#include <cstddef>
#include <mutex>

namespace edgefleet::shared {

class DatabasePool {
 public:
  explicit DatabasePool(std::size_t capacity = 1) : capacity_(capacity == 0 ? 1 : capacity) {}
  void close() { std::lock_guard lock(mutex_); closed_ = true; }
  bool closed() const { std::lock_guard lock(mutex_); return closed_; }
  std::size_t capacity() const { return capacity_; }

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  bool closed_ = false;
};

}  // namespace edgefleet::shared
