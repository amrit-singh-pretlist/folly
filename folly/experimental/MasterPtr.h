/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <mutex>

#include <folly/Function.h>
#include <folly/experimental/Cleanup.h>
#include <folly/futures/Future.h>

#include <glog/logging.h>

namespace folly {

template <typename T>
class EnableMasterFromThis;

template <typename T>
class MasterPtr;

template <typename T>
class MasterPtrRef;

namespace detail {
struct publicallyDerivedFromEnableMasterFromThis_fn {
  template <class T>
  void operator()(const EnableMasterFromThis<T>&) const {}
};
} // namespace detail

template <class T>
constexpr bool is_enable_master_from_this_v = folly::
    is_invocable_v<detail::publicallyDerivedFromEnableMasterFromThis_fn, T>;

template <typename T>
using is_enable_master_from_this =
    std::bool_constant<is_enable_master_from_this_v<T>>;

/**
 * EnableMasterFromThis provides an object with appropriate access to the
 * functionality of the MasterPtr holding this.
 */
template <typename T>
class EnableMasterFromThis {
  // initializes members when the MasterPtr for this is constructed
  //
  // used by the MasterPtr for this, to invoke the EnableMasterFromThis base of
  // T, if it exists.
  template <
      class O,
      class Master,
      std::enable_if_t<is_enable_master_from_this_v<O>, int> = 0>
  static void set(EnableMasterFromThis<O>* that, Master& m) {
    that->outerPtrWeak_ = m.outerPtrWeak_;
  }

  template <
      class O,
      class Master,
      std::enable_if_t<!is_enable_master_from_this_v<O>, int> = 0>
  static void set(O*, Master&) {}

 public:
  // Gets a non-owning reference to the pointer. MasterPtr::join() and the
  // MasterPtr::cleanup() work do *NOT* wait for outstanding MasterPtrRef
  // objects to be released.
  MasterPtrRef<T> masterRefFromThis() {
    return MasterPtrRef<T>(outerPtrWeak_);
  }

  // Gets a non-owning const reference to the pointer. MasterPtr::join() and the
  // MasterPtr::cleanup() work do *NOT* wait for outstanding MasterPtrRef
  // objects to be released.
  MasterPtrRef<const T> masterRefFromThis() const {
    return MasterPtrRef<const T>(outerPtrWeak_);
  }

  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // MasterPtr::join() was called or the MasterPtr::cleanup() task was started
  // (even if the call to MasterPtr::join() hasn't returned yet and the
  // MasterPtr::cleanup() task has not completed yet).
  std::shared_ptr<T> masterLockFromThis() {
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // MasterPtr::join() was called or the MasterPtr::cleanup() task was started
  // (even if the call to MasterPtr::join() hasn't returned yet and the
  // MasterPtr::cleanup() task has not completed yet).
  std::shared_ptr<T const> masterLockFromThis() const {
    if (!*this) {
      return nullptr;
    }
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

 private:
  template <class>
  friend class MasterPtr;

  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
};

/**
 * MasterPtr should be used to achieve deterministic destruction of objects with
 * shared ownership.
 * Once an object is managed by a MasterPtr, shared_ptrs can be obtained
 * pointing to that object. However destroying those shared_ptrs will never call
 * the object destructor inline. To destroy the object, join() method must be
 * called on MasterPtr or the task returned from cleanup() must be completed,
 * which will wait for all shared_ptrs to be released and then call the object
 * destructor on the caller supplied execution context.
 */
template <typename T>
class MasterPtr {
  // retrieves nested cleanup() work from innerPtr_. Called when the MasterPtr
  // cleanup() task has finished waiting for outstanding references
  //
  template <class Cleanup, std::enable_if_t<is_cleanup_v<Cleanup>, int> = 0>
  static folly::SemiFuture<folly::Unit> getCleanup(Cleanup* cleanup) {
    return std::move(*cleanup).cleanup();
  }

  template <class O, std::enable_if_t<!is_cleanup_v<O>, int> = 0>
  static folly::SemiFuture<folly::Unit> getCleanup(O*) {
    return folly::makeSemiFuture();
  }

 public:
  MasterPtr() = delete;
  template <class T2, class Deleter>
  MasterPtr(std::unique_ptr<T2, Deleter> ptr) {
    set(std::move(ptr));
  }
  ~MasterPtr() {
    if (*this) {
      LOG(FATAL) << "MasterPtr has to be joined explicitly.";
    }
  }

  explicit operator bool() const {
    return !!innerPtr_;
  }

  // Attempts to lock a pointer. Returns null if pointer is not set or if join()
  // was called or the cleanup() task was started (even if the call to join()
  // hasn't returned yet and the cleanup() task has not completed yet).
  std::shared_ptr<T> lock() const {
    if (!*this) {
      return nullptr;
    }
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

  // Waits until all the refereces obtained via lock() are released. Then
  // destroys the object in the current thread.
  // Can not be called concurrently with set().
  void join() {
    if (!*this) {
      return;
    }
    this->cleanup().get();
  }

  // Returns: a SemiFuture that waits until all the refereces obtained via
  // lock() are released. Then destroys the object on the Executor provided to
  // the SemiFuture.
  //
  // The returned SemiFuture must run to completion before calling set()
  //
  folly::SemiFuture<folly::Unit> cleanup() {
    return folly::makeSemiFuture()
        // clear outerPtrShared_ after cleanup is started
        // to disable further calls to lock().
        // then wait for outstanding references.
        .deferValue([this](folly::Unit) {
          if (!this->outerPtrShared_) {
            LOG(FATAL)
                << "Cleanup already run - lock() was previouly disabled.";
          }
          this->outerPtrShared_.reset();
          return std::move(this->unreferenced_);
        })
        // start cleanup tasks
        .deferValue([this](folly::Unit) {
          auto cleanup = getCleanup(innerPtr_.get());
          return std::move(cleanup);
        })
        .defer([this](folly::Try<folly::Unit> r) {
          if (r.hasException()) {
            LOG(FATAL) << "Cleanup actions must be noexcept.";
          }
          this->innerPtr_.reset();
        });
  }

  // Sets the pointer. Can not be called concurrently with lock() or join() or
  // ref() or while the SemiFuture returned from cleanup() is running.
  template <class T2, class Deleter>
  void set(std::unique_ptr<T2, Deleter> ptr) {
    if (*this) {
      LOG(FATAL) << "MasterPtr has to be joined before being set.";
    }

    if (!ptr) {
      return;
    }

    auto rawPtr = ptr.get();
    innerPtr_ = std::unique_ptr<T, folly::Function<void(T*)>>{
        ptr.release(),
        [d = ptr.get_deleter(), rawPtr](T*) mutable { d(rawPtr); }};

    auto referencesContract = folly::makePromiseContract<folly::Unit>();
    unreferenced_ = std::move(std::get<1>(referencesContract));

    auto innerPtrShared = std::shared_ptr<T>(
        innerPtr_.get(),
        [lastReference = std::move(std::get<0>(referencesContract))](
            T*) mutable { lastReference.setValue(folly::Unit{}); });

    outerPtrWeak_ = outerPtrShared_ =
        std::make_shared<std::shared_ptr<T>>(innerPtrShared);

    // attaches optional EnableMasterFromThis base of innerPtr_ to this
    // MasterPtr
    EnableMasterFromThis<T>::set(innerPtr_.get(), *this);
  }

  // Gets a non-owning reference to the pointer. join() and the cleanup() work
  // do *NOT* wait for outstanding MasterPtrRef objects to be released.
  MasterPtrRef<T> ref() const {
    return MasterPtrRef<T>(outerPtrWeak_);
  }

 private:
  template <class>
  friend class EnableMasterFromThis;
  friend class MasterPtrRef<T>;

  folly::SemiFuture<folly::Unit> unreferenced_;
  std::shared_ptr<std::shared_ptr<T>> outerPtrShared_;
  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
  std::unique_ptr<T, folly::Function<void(T*)>> innerPtr_;
};

/**
 * MasterPtrRef is a non-owning reference to the pointer. MasterPtr::join()
 * and the MasterPtr::cleanup() work do *NOT* wait for outstanding MasterPtrRef
 * objects to be released.
 */
template <typename T>
class MasterPtrRef {
 public:
  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // join() was called or cleanup() work was started (even if the call to join()
  // hasn't returned yet or the cleanup() work has not completed yet).
  std::shared_ptr<T> lock() const {
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

 private:
  template <class>
  friend class EnableMasterFromThis;
  template <class>
  friend class MasterPtr;
  /* implicit */ MasterPtrRef(std::weak_ptr<std::shared_ptr<T>> outerPtrWeak)
      : outerPtrWeak_(std::move(outerPtrWeak)) {}

  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
};

} // namespace folly
