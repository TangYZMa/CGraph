/***************************
@Author: Chunel
@Contact: chunel@foxmail.com
@File: UThreadPool.h
@Time: 2021/7/4 1:34 下午
@Desc: 
***************************/

#ifndef CGRAPH_UTHREADPOOL_H
#define CGRAPH_UTHREADPOOL_H

#include <vector>
#include <list>
#include <future>
#include <thread>
#include <algorithm>
#include <memory>
#include <functional>

#include "UThreadObject.h"
#include "AtomicQueue/UAtomicQueue.h"
#include "Thread/UThreadPrimary.h"
#include "Thread/UThreadSecondary.h"
#include "TaskGroup/UTaskGroup.h"

CGRAPH_NAMESPACE_BEGIN

class UThreadPool : public UThreadObject {

public:
    explicit UThreadPool() {
        cur_index_ = 0;
        is_init_ = false;

        /* 开启监控线程 */
        is_monitor_ = true;
        monitor_thread_ = std::move(std::thread(&UThreadPool::monitor, this));
        init();
    }


    ~UThreadPool() override {
        // 在析构的时候，才释放监控线程
        is_monitor_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }

        deinit();
    }


    /**
     * 开启所有的线程信息
     * @return
     */
    CStatus init() final {
        CGRAPH_FUNCTION_BEGIN
        if (is_init_) {
            CGRAPH_FUNCTION_END
        }

        primary_threads_.reserve(CGRAPH_DEFAULT_THREAD_SIZE);
        for (int i = 0; i < CGRAPH_DEFAULT_THREAD_SIZE; ++i) {
            auto ptr = CGRAPH_SAFE_MALLOC_COBJECT(UThreadPrimary)    // 创建核心线程数

            ptr->setThreadPoolInfo(i, &this->task_queue_, &this->primary_threads_);
            status = ptr->init();
            CGRAPH_FUNCTION_CHECK_STATUS

            primary_threads_.emplace_back(ptr);
        }

        is_init_ = true;
        CGRAPH_FUNCTION_END
    }

    /**
     * 提交任务信息
     * @tparam FunctionType
     * @param func
     * @return
     */
    template<typename FunctionType>
    std::future<typename std::result_of<FunctionType()>::type> commit(const FunctionType& func) {
        typedef typename std::result_of<FunctionType()>::type resultType;

        std::packaged_task<resultType()> task(func);
        std::future<resultType> result(task.get_future());

        if (!CGRAPH_FAIR_LOCK_ENABLE && cur_index_ >= 0 && cur_index_ < CGRAPH_DEFAULT_THREAD_SIZE) {
            // 部分任务直接放到线程的队列中执行
            primary_threads_[cur_index_]->work_stealing_queue_.push(std::move(task));
        } else {
            // 部分数据被分流到线程池（总体）的任务队列中
            task_queue_.push(std::move(task));
        }

        cur_index_++;
        if (cur_index_ >= CGRAPH_MAX_THREAD_SIZE || cur_index_ < 0) {
            cur_index_ = 0;
        }

        return result;
    }


    /**
     * 执行任务组信息
     * 取taskGroup内部ttl和入参ttl的最小值，为计算ttl标准。默认均为INT_MAX值
     * @param taskGroup
     * @param ttlMs
     * @return
     */
    CStatus commit(const UTaskGroup& taskGroup,
                   int ttlMs = CGRAPH_DEFAULT_GROUP_TTL_MS) {
        CGRAPH_FUNCTION_BEGIN
        CGRAPH_ASSERT_INIT(true)

        std::vector<std::future<void>> futures;
        for (const auto& task : taskGroup.task_arr_) {
            futures.emplace_back(this->commit(task));
        }

        // 计算最终运行时间信息
        std::chrono::system_clock::time_point deadline
                = std::chrono::system_clock::now() + std::chrono::milliseconds(std::min(taskGroup.getTtlMs(), ttlMs));

        for (auto& fut : futures) {
            const auto& futStatus = fut.wait_until(deadline);
            switch (futStatus) {
                case std::future_status::ready: break;    // 正常情况，直接返回了
                case std::future_status::timeout: status = CStatus("thread status timeout"); break;
                case std::future_status::deferred: status = CStatus("thread status deferred"); break;
                default: status = CStatus("thread status unknown");
            }
            CGRAPH_FUNCTION_CHECK_STATUS
        }

        CGRAPH_FUNCTION_END
    }


    /**
     * 释放所有的线程信息
     * @return
     */
    CStatus deinit() final {
        CGRAPH_FUNCTION_BEGIN
        if (!is_init_) {
            CGRAPH_FUNCTION_END
        }

        is_init_ = false;
        // primary 线程是普通指针，需要delete
        for (auto& thread : primary_threads_) {
            status = thread->deinit();
            CGRAPH_FUNCTION_CHECK_STATUS
            CGRAPH_DELETE_PTR(thread)
        }
        primary_threads_.clear();

        // secondary 线程是智能指针，不需要delete
        for (auto& thread : secondary_threads_) {
            thread->deinit();
            CGRAPH_FUNCTION_CHECK_STATUS
        }
        secondary_threads_.clear();

        CGRAPH_FUNCTION_END
    }


protected:
    /**
     * 监控线程执行函数，主要是判断是否需要增加线程，或销毁线程
     * 增/删 操作，仅针对secondary类型线程生效
     */
    void monitor() {
        while (is_monitor_) {
            while (is_monitor_ && !is_init_) {
                // 如果没有init，则一直处于空跑状态
                CGRAPH_SLEEP_SECOND(1)
            }

            int span = CGRAPH_MONITOR_SPAN;
            while (is_monitor_ && is_init_ && span--) {
                CGRAPH_SLEEP_SECOND(1)    // 保证可以快速退出
            }

            // 如果 primary线程都在执行，则表示忙碌
            bool busy = std::all_of(primary_threads_.begin(), primary_threads_.end(),
                                    [](UThreadPrimaryPtr ptr) {
                                        return ptr->is_running_;
                                    });

            // 如果忙碌，则需要添加 secondary线程
            if (busy && secondary_threads_.size() + CGRAPH_DEFAULT_THREAD_SIZE < CGRAPH_MAX_THREAD_SIZE) {
                auto ptr = std::make_unique<UThreadSecondary>();
                ptr->setThreadPoolInfo(&task_queue_);
                ptr->init();
                secondary_threads_.emplace_back(std::move(ptr));
            }

            // 判断 secondary 线程是否需要退出
            for (auto iter = secondary_threads_.begin(); iter != secondary_threads_.end(); ) {
                if (unlikely((*iter)->freeze())) {
                    secondary_threads_.erase(iter++);
                } else {
                    iter++;
                }
            }
        }
    }


private:
    bool is_init_ { false };                                                        // 是否初始化
    bool is_monitor_ { true };                                                      // 是否需要监控
    int cur_index_;                                                                 // 记录放入的线程数
    UAtomicQueue<UTaskWrapper> task_queue_;                                         // 用于存放普通任务
    std::vector<UThreadPrimaryPtr> primary_threads_;                                // 记录所有的核心线程
    std::list<std::unique_ptr<UThreadSecondary>> secondary_threads_;                // 用于记录所有的非核心线程数
    std::thread monitor_thread_;                                                    // 监控线程
};

using UThreadPoolPtr = UThreadPool *;

CGRAPH_NAMESPACE_END

#endif //CGRAPH_UTHREADPOOL_H
