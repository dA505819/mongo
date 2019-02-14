/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/client/read_preference.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace txn {

/**
 * This class groups all the asynchronous work scheduled by a given TransactionCoordinatorDriver.
 */
class AsyncWorkScheduler {
public:
    AsyncWorkScheduler(ServiceContext* serviceContext);
    ~AsyncWorkScheduler();

    /**
     * Schedules the specified callable to execute asynchronously and returns a future which will be
     * set with its result.
     */
    template <class Callable>
    Future<FutureContinuationResult<Callable, OperationContext*>> scheduleWork(
        Callable&& task) noexcept {
        return scheduleWorkIn(Milliseconds(0), std::forward<Callable>(task));
    }

    template <class Callable>
    Future<FutureContinuationResult<Callable, OperationContext*>> scheduleWorkIn(
        Milliseconds millis, Callable&& task) noexcept {
        return scheduleWorkAt(_executor->now() + millis, std::forward<Callable>(task));
    }

    template <class Callable>
    Future<FutureContinuationResult<Callable, OperationContext*>> scheduleWorkAt(
        Date_t when, Callable&& task) noexcept {
        using ReturnType = FutureContinuationResult<Callable, OperationContext*>;
        auto pf = makePromiseFuture<ReturnType>();
        auto taskCompletionPromise = std::make_shared<Promise<ReturnType>>(std::move(pf.promise));
        try {
            stdx::unique_lock<stdx::mutex> ul(_mutex);
            uassertStatusOK(_shutdownStatus);

            auto scheduledWorkHandle = uassertStatusOK(_executor->scheduleWorkAt(
                when,
                [ this, task = std::forward<Callable>(task), taskCompletionPromise ](
                    const executor::TaskExecutor::CallbackArgs&) mutable noexcept {
                    taskCompletionPromise->setWith([&] {
                        ThreadClient tc("TransactionCoordinator", _serviceContext);
                        stdx::unique_lock<stdx::mutex> ul(_mutex);
                        uassertStatusOK(_shutdownStatus);

                        auto uniqueOpCtxIter = _activeOpContexts.emplace(
                            _activeOpContexts.begin(), tc->makeOperationContext());
                        ul.unlock();

                        auto scopedGuard = makeGuard([&] {
                            ul.lock();
                            _activeOpContexts.erase(uniqueOpCtxIter);
                        });

                        return task(uniqueOpCtxIter->get());
                    });
                }));

            auto it =
                _activeHandles.emplace(_activeHandles.begin(), std::move(scheduledWorkHandle));

            ul.unlock();

            return std::move(pf.future).tapAll(
                [ this, it = std::move(it) ](StatusOrStatusWith<ReturnType> s) {
                    stdx::lock_guard<stdx::mutex> lg(_mutex);
                    _activeHandles.erase(it);
                });
        } catch (const DBException& ex) {
            taskCompletionPromise->setError(ex.toStatus());
            return std::move(pf.future);
        }
    }

    /**
     * Sends a command asynchronously to the given shard and returns a Future when that request
     * completes (with error or not).
     */
    Future<executor::TaskExecutor::ResponseStatus> scheduleRemoteCommand(
        const ShardId& shardId, const ReadPreferenceSetting& readPref, const BSONObj& commandObj);

    /**
     * Allows sub-tasks on this scheduler to be grouped together and works-around the fact that
     * futures are not cancellable.
     *
     * Shutting down the returned child scheduler has no effect on the parent. Shutting down the
     * parent scheduler also shuts down all child schedulers and prevents new ones from starting.
     */
    std::unique_ptr<AsyncWorkScheduler> makeChildScheduler();

    /**
     * Non-blocking method, which interrupts all currently active scheduled commands or tasks and
     * prevents any new ones from starting.
     * After this method is called, all returned futures, which haven't yet been signalled will be
     * set to the specified status. Attempting to schedule any new operations will return ready
     * futures set to the specified status.
     *
     * Must not be called with Status::OK.
     */
    void shutdown(Status status);

private:
    using ChildIteratorsList = std::list<AsyncWorkScheduler*>;

    /**
     * Finds the host and port for a shard.
     */
    Future<HostAndPort> _targetHostAsync(const ShardId& shardId,
                                         const ReadPreferenceSetting& readPref);

    // Service context under which this executor runs
    ServiceContext* const _serviceContext;

    // Cached reference to the executor to use
    executor::TaskExecutor* const _executor;

    // If this work scheduler was constructed through 'makeChildScheduler', points to the parent
    // scheduler and contains the iterator from the parent, which needs to be removed on destruction
    AsyncWorkScheduler* _parent{nullptr};
    ChildIteratorsList::iterator _itToRemove;

    // Mutex to protect the shared state below
    stdx::mutex _mutex;

    // If shutdown() is called, this contains the first status that was passed to it and is an
    // indication that no more operations can be scheduled
    Status _shutdownStatus{Status::OK()};

    // Any active scheduled work will have its operation context stored here
    std::list<ServiceContext::UniqueOperationContext> _activeOpContexts;

    // Any active scheduled work or network operation will have its TaskExecutor handle stored here
    std::list<executor::TaskExecutor::CallbackHandle> _activeHandles;

    // Any outstanding child schedulers created though 'makeChildScheduler'
    ChildIteratorsList _childSchedulers;
};

enum class ShouldStopIteration { kYes, kNo };

/**
 * Helper function that allows you to asynchronously aggregate the results of a vector of Futures.
 * It's essentially an async foldLeft.
 *
 * The combiner function specifies how to take an incoming result (the second parameter) and combine
 * it to create the final ('global') result (the first parameter). The inital value for the 'global
 * result' is specified by initValue.
 *
 * Example from the unit tests:
 *
 *  TEST_F(TransactionCoordinatorTest, CollectReturnsCombinedResultWithSeveralInputFutures) {
 *      std::vector<Future<int>> futures;
 *      std::vector<Promise<int>> promises;
 *      std::vector<int> futureValues;
 *      for (int i = 0; i < 5; ++i) {
 *          auto pf = makePromiseFuture<int>();
 *          futures.push_back(std::move(pf.future));
 *          promises.push_back(std::move(pf.promise));
 *          futureValues.push_back(i);
 *      }
 *
 *      // Sum all of the inputs.
 *      auto resultFuture = collect<int, int>(futures, 0, [](int& result, const int& next) {
 *          result += next;
 *          return true;
 *      });
 *
 *      for (size_t i = 0; i < promises.size(); ++i) {
 *          promises[i].emplaceValue(futureValues[i]);
 *      }
 *
 *      // Result should be the sum of all the values emplaced into the promises.
 *      ASSERT_EQ(resultFuture.get(), std::accumulate(futureValues.begin(), futureValues.end(), 0));
 * }
 *
 */
template <class IndividualResult, class GlobalResult, class Callable>
Future<GlobalResult> collect(std::vector<Future<IndividualResult>>&& futures,
                             GlobalResult&& initValue,
                             Callable&& combiner) {
    if (futures.size() == 0) {
        return initValue;
    }

    /**
     * Shared state for the continuations of the individual futures in the array.
     */
    struct SharedBlock {
        SharedBlock(size_t numOutstandingResponses,
                    GlobalResult globalResult,
                    Promise<GlobalResult> resultPromise,
                    Callable&& combiner)
            : numOutstandingResponses(numOutstandingResponses),
              globalResult(std::move(globalResult)),
              resultPromise(std::move(resultPromise)),
              combiner(std::move(combiner)) {}
        /*****************************************************
         * The first few fields have fixed values.           *
        ******************************************************/
        // Protects all state in the SharedBlock.
        stdx::mutex mutex;

        // If any response returns an error prior to a response setting shouldStopIteration to
        // ShouldStopIteration::kYes, the promise will be set with that error rather than the global
        // result.
        Status status{Status::OK()};

        // If the combiner returns kYes after processing any response, the combiner will not be
        // applied to any further responses.
        ShouldStopIteration shouldStopIteration{ShouldStopIteration::kNo};

        /*****************************************************
         * The below have initial values based on user input.*
        ******************************************************/
        // The number of input futures that have not yet been resolved and processed.
        size_t numOutstandingResponses;
        // The variable where the intermediate results and final result is stored.
        GlobalResult globalResult;
        // The promise to be fulfilled when the result is ready.
        Promise<GlobalResult> resultPromise;
        // The input combiner function.
        Callable combiner;
    };

    // Create the promise and future used to fulfill the result.
    auto resultPromiseAndFuture = makePromiseFuture<GlobalResult>();

    // Create the shared context used by all continuations
    auto sharedBlock = std::make_shared<SharedBlock>(futures.size(),
                                                     std::move(initValue),
                                                     std::move(resultPromiseAndFuture.promise),
                                                     std::move(combiner));

    // For every input future, add a continuation that will asynchronously update the
    // SharedBlock upon completion of the input future.
    for (auto&& localFut : futures) {
        std::move(localFut)
            .then([sharedBlock](IndividualResult res) {
                stdx::unique_lock<stdx::mutex> lk(sharedBlock->mutex);
                if (sharedBlock->shouldStopIteration == ShouldStopIteration::kNo &&
                    sharedBlock->status.isOK()) {
                    sharedBlock->shouldStopIteration =
                        sharedBlock->combiner(sharedBlock->globalResult, std::move(res));
                }
            })
            .onError([sharedBlock](Status s) {
                stdx::unique_lock<stdx::mutex> lk(sharedBlock->mutex);
                if (sharedBlock->shouldStopIteration == ShouldStopIteration::kNo &&
                    sharedBlock->status.isOK()) {
                    sharedBlock->status = s;
                }
            })
            .getAsync([sharedBlock](Status s) {
                stdx::unique_lock<stdx::mutex> lk(sharedBlock->mutex);
                sharedBlock->numOutstandingResponses--;
                if (sharedBlock->numOutstandingResponses == 0) {
                    // Unlock before emplacing the result in case any continuations do expensive
                    // work.
                    lk.unlock();
                    if (sharedBlock->status.isOK()) {
                        sharedBlock->resultPromise.emplaceValue(sharedBlock->globalResult);
                    } else {
                        sharedBlock->resultPromise.setError(sharedBlock->status);
                    }
                }
            });
    }

    return std::move(resultPromiseAndFuture.future);
}

/**
 * Returns a future that will be resolved when all of the input futures have resolved, or rejected
 * when any of the futures is rejected.
 */
Future<void> whenAll(std::vector<Future<void>>& futures);

/**
 * Executes a function returning a Future until the function does not return an error status or
 * until one of the provided error codes is returned.
 */
template <class LoopBodyFn, class ShouldRetryFn>
Future<FutureContinuationResult<LoopBodyFn>> doWhile(AsyncWorkScheduler& scheduler,
                                                     boost::optional<Backoff> backoff,
                                                     ShouldRetryFn&& shouldRetryFn,
                                                     LoopBodyFn&& f) {
    using ReturnType = typename decltype(f())::value_type;
    auto future = f();
    return std::move(future).onCompletion([
        &scheduler,
        backoff = std::move(backoff),
        shouldRetryFn = std::forward<ShouldRetryFn>(shouldRetryFn),
        f = std::forward<LoopBodyFn>(f)
    ](StatusOrStatusWith<ReturnType> s) mutable {
        if (!shouldRetryFn(s))
            return Future<ReturnType>(std::move(s));

        // Retry after a delay.
        const auto delayMillis = (backoff ? backoff->nextSleep() : Milliseconds(0));
        return scheduler.scheduleWorkIn(delayMillis, [](OperationContext* opCtx) {}).then([
            &scheduler,
            backoff = std::move(backoff),
            shouldRetryFn = std::move(shouldRetryFn),
            f = std::move(f)
        ]() mutable {
            return doWhile(scheduler, std::move(backoff), std::move(shouldRetryFn), std::move(f));
        });
    });
}

}  // namespace txn
}  // namespace mongo