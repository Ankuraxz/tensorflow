/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache_rpc_support.h"

#include "tensorflow/compiler/tf2xla/host_compute_metadata.pb.h"
#include "tensorflow/core/distributed_runtime/rpc/grpc_util.h"
#include "tensorflow/core/platform/casts.h"
#if defined(LIBTFTPU)
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache.pb.h"
#endif
#include "tensorflow/core/tpu/kernels/tpu_compilation_cache_common.pb.h"
#include "tensorflow/core/tpu/kernels/tpu_program_group.h"
#include "tensorflow/stream_executor/tpu/proto_helper.h"

namespace tensorflow {
namespace tpu {
std::shared_ptr<::grpc::ChannelCredentials> CreateChannelCredentials() {
  return ::grpc::InsecureChannelCredentials();  // NOLINT
}

#if defined(LIBTFTPU)
template <>
Status DeserializeRpcResponseToCacheEntry<GetTpuProgramResponseExternal>(
    absl::string_view local_proto_key, GetTpuProgramResponseExternal* response,
    std::shared_ptr<CacheEntry>* cache_entry) {
  CHECK_NE(response, nullptr);
  CHECK_NE(cache_entry, nullptr);
  *cache_entry = std::make_shared<CacheEntry>();
  CacheEntry& entry = **cache_entry;
  entry.key = std::string(local_proto_key);

  if (response->is_empty()) {
    entry.size = 0;
  } else {
    // When we lookup from remote cache, we fetch a TPU program for a specific
    // core hence we allocate TPU program group for a single program.
    entry.tpu_program_group = TpuProgramGroup::Create(/*count=*/1);

    TpuSerializedProto serialized_response_proto =
        stream_executor::tpu::SerializeProto(*response);
    auto cleanup = xla::MakeCleanup([&serialized_response_proto]() {
      stream_executor::tpu::SerializedProto_Free(serialized_response_proto);
    });
    // TODO(b/166575150): can be optimized by sending the buffer over the gRPC
    // without an extra deserializing.
    TpuProgramGroup* tpu_program_group =
        tensorflow::down_cast<TpuProgramGroup*>(entry.tpu_program_group.get());
    TF_RETURN_IF_ERROR(tpu_program_group->DeserializeFromProto(
        /*index=*/0, serialized_response_proto));
    entry.size = entry.tpu_program_group->program_size();
  }

  return Status::OK();
}

xla::StatusOr<std::vector<::grpc::Slice>> SerializeCacheEntryToBufferSlices(
    const TpuCompilationCacheEntry& cache_entry) {
  if (cache_entry.tpu_program_group() == nullptr) {
    // It's possible that the sharding/unsharding entry does not exist, but the
    // main entry must exist.
    GetTpuProgramResponseExternal header;
    header.set_is_empty(true);
    std::string encoded_header;
    if (!header.AppendToString(&encoded_header)) {
      return errors::Internal("Failed to serialize TPU program metadata.");
    }
    ::grpc::Slice slice(encoded_header);
    return std::vector<::grpc::Slice>{slice};
  }

  const TpuProgramGroup* tpu_program_group =
      tensorflow::down_cast<const TpuProgramGroup*>(
          cache_entry.tpu_program_group());
  CHECK_NE(tpu_program_group, nullptr);
  CHECK_GE(tpu_program_group->program_count(), 0);
  CHECK_GE(cache_entry.core_index(), 0);
  CHECK_LT(cache_entry.core_index(), tpu_program_group->program_count());
  const int64 program_size = tpu_program_group->program_size();
  if (program_size > INT_MAX) {
    return errors::Internal("TPU program exceeded 2 GiB.");
  }

  TpuExecutableSerializedProto executable;
  auto cleanup_executable = xla::MakeCleanup([&executable]() {
    if (executable.size > 0) {
      stream_executor::tpu::SerializedProto_Free(executable);
    }
  });
  auto get_executable_status = tpu_program_group->SerializeExecutable(
      cache_entry.core_index(), &executable);
  if (!get_executable_status.ok()) {
    return errors::Internal("Failed to serialize TPU program.");
  }

  // Encode and serialize header fields.
  GetTpuProgramResponseExternal header;
  header.mutable_proto()->ParseFromArray(executable.bytes, executable.size);
  header.set_is_empty(false);

  HostComputeMetadataSerializedProto host_compute_metadata;
  auto cleanup_host_compute_metadata =
      xla::MakeCleanup([&host_compute_metadata]() {
        if (host_compute_metadata.size > 0) {
          stream_executor::tpu::SerializedProto_Free(host_compute_metadata);
        }
      });
  Status get_host_compute_metadata_status =
      tpu_program_group->SerializeHostComputeMetadata(cache_entry.core_index(),
                                                      &host_compute_metadata);
  if (!get_host_compute_metadata_status.ok()) {
    return errors::Internal("Failed to serialize host compute metadata.");
  }
  tf2xla::HostComputeMetadata host_compute_metadata_proto =
      stream_executor::tpu::DeserializeProto<tf2xla::HostComputeMetadata>(
          host_compute_metadata);
  *header.mutable_host_compute_metadata() =
      std::move(host_compute_metadata_proto);

  bool may_modify_variables =
      tpu_program_group->may_modify_variables(cache_entry.core_index());
  header.set_may_modify_variables(may_modify_variables);

  CompilerMetadataSerializedProto compiler_metadata;
  auto cleanup_compiler_metadata = xla::MakeCleanup([&compiler_metadata]() {
    if (compiler_metadata.size > 0) {
      stream_executor::tpu::SerializedProto_Free(compiler_metadata);
    }
  });
  Status get_compiler_metadata_status =
      tpu_program_group->SerializeCompilerMetadata(cache_entry.core_index(),
                                                   &compiler_metadata);
  if (!get_compiler_metadata_status.ok()) {
    return errors::Internal("Failed to serialize compiler metadata.");
  }
  header.mutable_compiler_metadata()->mutable_data()->assign(
      compiler_metadata.bytes, compiler_metadata.size);

  std::string encoded_header;
  if (!header.AppendToString(&encoded_header)) {
    return errors::Internal("Failed to serialize TPU program metadata.");
  }

  return std::vector<::grpc::Slice>{::grpc::Slice(encoded_header)};
}
#endif  // LIBTFTPU
}  // namespace tpu
}  // namespace tensorflow
