// Copyright 2021-2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "instance_queue.h"

#include "triton/common/logging.h"

namespace triton { namespace core {

InstanceQueue::InstanceQueue(size_t max_batch_size, uint64_t max_queue_delay_ns)
    : max_batch_size_(max_batch_size), max_queue_delay_ns_(max_queue_delay_ns),
      waiting_consumer_count_(0)
{
}

size_t
InstanceQueue::Size()
{
  return payload_queue_.size();
}

bool
InstanceQueue::Empty()
{
  return payload_queue_.empty();
}

void
InstanceQueue::Enqueue(const std::shared_ptr<Payload>& payload)
{
  payload_queue_.push_back(payload);
}

void
InstanceQueue::Dequeue(
    std::shared_ptr<Payload>* payload,
    std::vector<std::shared_ptr<Payload>>* merged_payloads)
{
  *payload = payload_queue_.front();
  payload_queue_.pop_front();
  {
    std::lock_guard<std::mutex> exec_lock(*((*payload)->GetExecMutex()));
    (*payload)->SetState(Payload::State::EXECUTING);
    if ((!payload_queue_.empty()) && (max_queue_delay_ns_ > 0) &&
        (max_batch_size_ > 1) && (!(*payload)->IsSaturated())) {
      bool continue_merge;
      do {
        continue_merge = false;
        uint64_t now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        size_t batch_size = (*payload)->BatchSize();
        if ((!payload_queue_.empty()) &&
            (!payload_queue_.front()->IsSaturated()) &&
            (now_ns - payload_queue_.front()->BatcherStartNs()) >
                max_queue_delay_ns_) {
          std::lock_guard<std::mutex> exec_lock(
              *(payload_queue_.front()->GetExecMutex()));
          payload_queue_.front()->SetState(Payload::State::EXECUTING);
          size_t front_batch_size = payload_queue_.front()->BatchSize();
          if ((batch_size + front_batch_size) <= max_batch_size_) {
            const auto& status =
                (*payload)->MergePayload(payload_queue_.front());
            if (status.IsOk()) {
              merged_payloads->push_back(payload_queue_.front());
              payload_queue_.pop_front();
              continue_merge = true;
            }
          }
        }
      } while (continue_merge);
    }
  }
}

void
InstanceQueue::IncrementConsumerCount()
{
  {
    std::lock_guard<std::mutex> lock(waiting_consumer_mu_);
    waiting_consumer_count_++;
  }
  waiting_consumer_cv_.notify_one();
}

void
InstanceQueue::DecrementConsumerCount()
{
  {
    std::lock_guard<std::mutex> lock(waiting_consumer_mu_);
    waiting_consumer_count_--;
  }
  waiting_consumer_cv_.notify_one();
}

void
InstanceQueue::WaitForConsumer()
{
  std::unique_lock<std::mutex> lock(waiting_consumer_mu_);
  waiting_consumer_cv_.wait(
      lock, [this]() { return waiting_consumer_count_ > 0; });
}

int
InstanceQueue::WaitingConsumerCount()
{
  std::lock_guard<std::mutex> lock(waiting_consumer_mu_);
  return waiting_consumer_count_;
}

}}  // namespace triton::core
