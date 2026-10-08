#pragma once
#include "CompanionCommandQueue.h"
#include "CompanionTransfer.h"
namespace companion {
inline constexpr size_t FRAME_BUFFER_SIZE = FRAME_HEADER_SIZE + MAX_CONTROL_PAYLOAD;
inline constexpr size_t RECEIVE_OFFSET = COMMAND_QUEUE_STORAGE_SIZE;
inline constexpr size_t REQUEST_OFFSET = RECEIVE_OFFSET + FRAME_BUFFER_SIZE;
inline constexpr size_t RESPONSE_OFFSET = REQUEST_OFFSET + FRAME_BUFFER_SIZE;
inline constexpr size_t TRANSFER_OFFSET = RESPONSE_OFFSET + FRAME_BUFFER_SIZE;
inline constexpr size_t TRANSFER_SCRATCH_SIZE = SESSION_WORKSPACE_SIZE - TRANSFER_OFFSET;
static_assert(TRANSFER_SCRATCH_SIZE >= TRANSFER_JOURNAL_SIZE);
}  // namespace companion
