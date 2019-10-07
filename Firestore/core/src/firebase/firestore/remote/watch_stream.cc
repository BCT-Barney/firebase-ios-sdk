/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utility>

#include "Firestore/core/src/firebase/firestore/remote/watch_stream.h"

#include "Firestore/core/src/firebase/firestore/nanopb/message.h"
#include "Firestore/core/src/firebase/firestore/util/hard_assert.h"
#include "Firestore/core/src/firebase/firestore/util/log.h"
#include "Firestore/core/src/firebase/firestore/util/status.h"

namespace firebase {
namespace firestore {
namespace remote {

using auth::CredentialsProvider;
using auth::Token;
using local::QueryData;
using model::TargetId;
using nanopb::MaybeMessage;
using nanopb::Message;
using util::AsyncQueue;
using util::Status;
using util::TimerId;

WatchStream::WatchStream(
    const std::shared_ptr<AsyncQueue>& async_queue,
    std::shared_ptr<CredentialsProvider> credentials_provider,
    Serializer serializer,
    GrpcConnection* grpc_connection,
    WatchStreamCallback* callback)
    : Stream{async_queue, std::move(credentials_provider), grpc_connection,
             TimerId::ListenStreamConnectionBackoff, TimerId::ListenStreamIdle},
      serializer_bridge_{std::move(serializer)},
      callback_{NOT_NULL(callback)} {
}

void WatchStream::WatchQuery(const QueryData& query) {
  EnsureOnQueue();

  auto request = serializer_bridge_.CreateWatchRequest(query);
  LOG_DEBUG("%s watch: %s", GetDebugDescription(),
            serializer_bridge_.Describe(request.proto()));
  Write(request.CreateByteBuffer());
}

void WatchStream::UnwatchTargetId(TargetId target_id) {
  EnsureOnQueue();

  auto request = serializer_bridge_.CreateUnwatchRequest(target_id);
  LOG_DEBUG("%s unwatch: %s", GetDebugDescription(),
            serializer_bridge_.Describe(request.proto()));
  Write(request.CreateByteBuffer());
}

std::unique_ptr<GrpcStream> WatchStream::CreateGrpcStream(
    GrpcConnection* grpc_connection, const Token& token) {
  return grpc_connection->CreateStream("/google.firestore.v1.Firestore/Listen",
                                       token, this);
}

void WatchStream::TearDown(GrpcStream* grpc_stream) {
  grpc_stream->FinishImmediately();
}

void WatchStream::NotifyStreamOpen() {
  callback_->OnWatchStreamOpen();
}

Status WatchStream::NotifyStreamResponse(const grpc::ByteBuffer& message) {
  MaybeMessage<google_firestore_v1_ListenResponse> maybe_response =
      serializer_bridge_.ParseResponse(message);
  if (!maybe_response.ok()) {
    return maybe_response.status();
  }

  const auto& response = maybe_response.ValueOrDie().proto();

  if (bridge::IsLoggingEnabled()) {
    LOG_DEBUG("%s response: %s", GetDebugDescription(),
              serializer_bridge_.Describe(response));
  }

  // A successful response means the stream is healthy.
  backoff_.Reset();

  callback_->OnWatchStreamChange(
      *serializer_bridge_.ToWatchChange(response),
      serializer_bridge_.ToSnapshotVersion(response));
  return Status::OK();
}

void WatchStream::NotifyStreamClose(const Status& status) {
  callback_->OnWatchStreamClose(status);
}

}  // namespace remote
}  // namespace firestore
}  // namespace firebase
