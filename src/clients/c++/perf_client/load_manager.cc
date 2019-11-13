// Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
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

#include "src/clients/c++/perf_client/load_manager.h"
#include "src/core/model_config.h"

LoadManager::~LoadManager()
{
  early_exit = true;
  // wake up all threads
  wake_signal_.notify_all();

  size_t cnt = 0;
  for (auto& thread : threads_) {
    thread.join();
    if (!threads_stat_[cnt]->status_.IsOk()) {
      std::cerr << "Thread [" << cnt
                << "] had error: " << (threads_stat_[cnt]->status_)
                << std::endl;
    }
    cnt++;
  }
}

nic::Error
LoadManager::CheckHealth()
{
  // Check thread status to make sure that the actual concurrency level is
  // consistent to the one being reported
  // If some thread return early, main thread will return and
  // the worker thread's error message will be reported
  // when ConcurrencyManager's destructor get called.
  for (auto& thread_stat : threads_stat_) {
    if (!thread_stat->status_.IsOk()) {
      return nic::Error(
          ni::RequestStatusCode::INTERNAL,
          "Failed to maintain concurrency level requested."
          " Worker thread(s) failed to generate concurrent requests.");
    }
  }
  return nic::Error::Success;
}

nic::Error
LoadManager::SwapTimestamps(TimestampVector& new_timestamps)
{
  TimestampVector total_timestamp;
  // Gather request timestamps with proper locking from all the worker threads
  for (auto& thread_stat : threads_stat_) {
    std::lock_guard<std::mutex> lock(thread_stat->mu_);
    total_timestamp.insert(
        total_timestamp.end(), thread_stat->request_timestamps_.begin(),
        thread_stat->request_timestamps_.end());
    thread_stat->request_timestamps_.clear();
  }
  // Swap the results
  total_timestamp.swap(new_timestamps);
  return nic::Error::Success;
}

nic::Error
LoadManager::GetAccumulatedContextStat(nic::InferContext::Stat* contexts_stat)
{
  for (auto& thread_stat : threads_stat_) {
    std::lock_guard<std::mutex> lock(thread_stat->mu_);
    for (auto& context_stat : thread_stat->contexts_stat_) {
      contexts_stat->completed_request_count +=
          context_stat.completed_request_count;
      contexts_stat->cumulative_total_request_time_ns +=
          context_stat.cumulative_total_request_time_ns;
      contexts_stat->cumulative_send_time_ns +=
          context_stat.cumulative_send_time_ns;
      contexts_stat->cumulative_receive_time_ns +=
          context_stat.cumulative_receive_time_ns;
    }
  }
  return nic::Error::Success;
}

nic::Error
LoadManager::InitManagerInputs(
    std::unique_ptr<LoadManager> local_manager, const size_t string_length,
    const std::string& string_data, const bool zero_input,
    const std::string& data_directory, std::unique_ptr<LoadManager>* manager)
{
  std::unique_ptr<nic::InferContext> ctx;
  RETURN_IF_ERROR(local_manager->factory_->CreateInferContext(&ctx));

  size_t max_input_byte_size = 0;
  size_t max_batch1_num_strings = 0;
  bool needs_string_input = false;

  for (const auto& input : ctx->Inputs()) {
    // Validate user provided shape
    if (!local_manager->input_shapes_.empty()) {
      auto it = local_manager->input_shapes_.find(input->Name());
      if (it != local_manager->input_shapes_.end()) {
        const auto& dims = it->second;
        const auto& config_dims = input->Dims();
        if (!ni::CompareDimsWithWildcard(config_dims, dims)) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' expects shape " +
                  ni::DimsListToString(config_dims) +
                  " and user supplied shape " + ni::DimsListToString(dims));
        }
      }
    }

    // For variable shape, set the shape if specified
    if (input->Shape().empty()) {
      auto it = local_manager->input_shapes_.find(input->Name());
      if (it != local_manager->input_shapes_.end()) {
        input->SetShape(it->second);
      }
    }

    const int64_t bs = input->ByteSize();
    if (bs < 0 && input->DType() != ni::DataType::TYPE_STRING) {
      if (input->Shape().empty()) {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "input '" + input->Name() +
                "' has variable-size shape and the shape to be used is not "
                "specified, unable to create input values for model '" +
                ctx->ModelName() + "'");
      }
    }

    // Validate the shape specification for TYPE_STRING
    if (input->DType() == ni::DataType::TYPE_STRING) {
      bool is_variable_shape = false;
      for (const auto dim : input->Dims()) {
        if (dim == -1) {
          is_variable_shape = true;
          break;
        }
      }
      if (is_variable_shape && input->Shape().empty()) {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "input '" + input->Name() +
                "' has variable-size shape and the shape to be used is not "
                "specified, unable to create input values for model '" +
                ctx->ModelName() + "'");
      }
    }

    // Read provided data
    if (!data_directory.empty()) {
      if (input->DType() != ni::DataType::TYPE_STRING) {
        const auto file_path = data_directory + "/" + input->Name();
        auto it = local_manager->input_data_
                      .emplace(input->Name(), std::vector<char>())
                      .first;
        RETURN_IF_ERROR(ReadFile(file_path, &it->second));
      } else {
        const auto file_path = data_directory + "/" + input->Name();
        auto it = local_manager->input_string_data_
                      .emplace(input->Name(), std::vector<std::string>())
                      .first;
        RETURN_IF_ERROR(ReadTextFile(file_path, &it->second));
      }
    } else {
      if (input->DType() != ni::DataType::TYPE_STRING) {
        max_input_byte_size =
            std::max(max_input_byte_size, (size_t)input->ByteSize());
      } else {
        // Get the number of strings needed for this input batch-1
        size_t batch1_num_strings = 1;
        if (!input->Shape().empty()) {
          for (const auto dim : input->Shape()) {
            batch1_num_strings *= dim;
          }
        } else {
          for (const auto dim : input->Dims()) {
            batch1_num_strings *= dim;
          }
        }

        needs_string_input = true;
        max_batch1_num_strings =
            std::max(max_batch1_num_strings, batch1_num_strings);
      }
    }
  }

  // Create a zero or randomly (as indicated by zero_input_)
  // initialized buffer that is large enough to provide the largest
  // needed input. We (re)use this buffer for all input values.
  if (max_input_byte_size > 0) {
    if (zero_input) {
      local_manager->input_buf_.resize(max_input_byte_size, 0);
    } else {
      local_manager->input_buf_.resize(max_input_byte_size);
      for (auto& byte : local_manager->input_buf_) {
        byte = rand();
      }
    }
  }

  // Similarly, handle the input_string_buf_
  if (needs_string_input) {
    local_manager->input_string_buf_.resize(max_batch1_num_strings);
    if (!string_data.empty()) {
      for (size_t i = 0; i < max_batch1_num_strings; i++) {
        local_manager->input_string_buf_[i] = string_data;
      }
    } else {
      for (size_t i = 0; i < max_batch1_num_strings; i++) {
        local_manager->input_string_buf_[i] = GetRandomString(string_length);
      }
    }
  }

  // Reserve the required vector space
  local_manager->threads_stat_.reserve(local_manager->max_threads_);

  *manager = std::move(local_manager);

  return nic::Error::Success;
}

nic::Error
LoadManager::PrepareInfer(
    std::unique_ptr<nic::InferContext>* ctx,
    std::unique_ptr<nic::InferContext::Options>* options)
{
  RETURN_IF_ERROR(factory_->CreateInferContext(ctx));

  uint64_t max_batch_size = (*ctx)->MaxBatchSize();

  // Model specifying maximum batch size of 0 indicates that batching
  // is not supported and so the input tensors do not expect a "N"
  // dimension (and 'batch_size' should be 1 so that only a single
  // image instance is inferred at a time).
  if (max_batch_size == 0) {
    if (batch_size_ != 1) {
      return nic::Error(
          ni::RequestStatusCode::INVALID_ARG,
          "expecting batch size 1 for model '" + (*ctx)->ModelName() +
              "' which does not support batching");
    }
  } else if (batch_size_ > max_batch_size) {
    return nic::Error(
        ni::RequestStatusCode::INVALID_ARG,
        "expecting batch size <= " + std::to_string(max_batch_size) +
            " for model '" + (*ctx)->ModelName() + "'");
  }

  // Prepare context for 'batch_size' batches. Request that all
  // outputs be returned.
  // Only set options if it has not been created, otherwise,
  // assuming that the options for this model has been created previously
  if (*options == nullptr) {
    RETURN_IF_ERROR(nic::InferContext::Options::Create(options));

    (*options)->SetBatchSize(batch_size_);
    for (const auto& output : (*ctx)->Outputs()) {
      (*options)->AddRawResult(output);
    }
  }

  RETURN_IF_ERROR((*ctx)->SetRunOptions(*(*options)));

  // Set the provided shape for variable shape tensor
  for (const auto& input : (*ctx)->Inputs()) {
    if (input->Shape().empty()) {
      auto it = input_shapes_.find(input->Name());
      if (it != input_shapes_.end()) {
        input->SetShape(it->second);
      }
    }
  }

  // Initialize inputs
  for (const auto& input : (*ctx)->Inputs()) {
    RETURN_IF_ERROR(input->Reset());

    if (input->DType() != ni::DataType::TYPE_STRING) {
      size_t batch1_size = (size_t)input->ByteSize();
      const uint8_t* data;
      // if available, use provided data instead
      auto it = input_data_.find(input->Name());
      if (it != input_data_.end()) {
        if (batch1_size != it->second.size()) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_size) +
                  " bytes for each batch, but provided data has " +
                  std::to_string(it->second.size()) + " bytes");
        }
        data = (const uint8_t*)&(it->second)[0];
      } else if (input_buf_.size() != 0) {
        if (batch1_size > input_buf_.size()) {
          return nic::Error(
              ni::RequestStatusCode::INTERNAL,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_size) +
                  " bytes for each batch, but generated data has " +
                  std::to_string(input_buf_.size()) + " bytes");
        } else {
          data = &input_buf_[0];
        }
      } else {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "unable to find data for input '" + input->Name() + "'.");
      }

      for (size_t i = 0; i < batch_size_; ++i) {
        RETURN_IF_ERROR(input->SetRaw(data, batch1_size));
      }
    } else {
      size_t batch1_num_strings = 1;
      if (!input->Shape().empty()) {
        for (const auto dim : input->Shape()) {
          batch1_num_strings *= dim;
        }
      } else {
        for (const auto dim : input->Dims()) {
          batch1_num_strings *= dim;
        }
      }

      bool used_new_allocation = false;
      std::vector<std::string>* data;

      // if available, use provided data instead
      auto it = input_string_data_.find(input->Name());
      if (it != input_string_data_.end()) {
        if (it->second.size() != batch1_num_strings) {
          return nic::Error(
              ni::RequestStatusCode::INVALID_ARG,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_num_strings) +
                  " strings for each batch, but provided data has " +
                  std::to_string(it->second.size()) + " strings.");
        }
        data = &it->second;
      } else if (input_string_buf_.size() != 0) {
        if (batch1_num_strings > input_string_buf_.size()) {
          return nic::Error(
              ni::RequestStatusCode::INTERNAL,
              "input '" + input->Name() + "' requires " +
                  std::to_string(batch1_num_strings) +
                  " strings for each batch, but generated data has " +
                  std::to_string(input_string_buf_.size()) + " strings.");
        } else {
          used_new_allocation = true;
          data = new std::vector<std::string>(
              input_string_buf_.begin(),
              input_string_buf_.begin() + batch1_num_strings);
        }
      } else {
        return nic::Error(
            ni::RequestStatusCode::INVALID_ARG,
            "unable to find data for input '" + input->Name() + "'.");
      }
      for (size_t i = 0; i < batch_size_; ++i) {
        RETURN_IF_ERROR(input->SetFromString(*data));
      }
      if (used_new_allocation) {
        delete data;
      }
    }
  }

  return nic::Error::Success;
}

size_t
LoadManager::GetRandomLength(double offset_ratio)
{
  int random_offset = ((2.0 * rand() / double(RAND_MAX)) - 1.0) * offset_ratio *
                      sequence_length_;
  if (int(sequence_length_) + random_offset <= 0) {
    return 1;
  }
  return sequence_length_ + random_offset;
}