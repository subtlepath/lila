#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// State buffers contain the firmware's 16-byte little-endian ItemState.
bool tinta_scheduler_fresh(uint32_t uid, uint8_t* output);
bool tinta_scheduler_review(const uint8_t* input, uint8_t grade, uint16_t day, uint16_t retentionBasisPoints,
                            uint16_t maximumInterval, uint8_t* output);
bool tinta_scheduler_validate(const uint8_t* input);
bool tinta_scheduler_set_flag(const uint8_t* input, bool star, bool enabled, uint8_t* output);
typedef struct {
  bool new_item;
  bool review;
  bool correct;
} TintaReviewCounts;
bool tinta_scheduler_review_counted(const uint8_t* input, uint8_t grade, uint16_t day, uint16_t retentionBasisPoints,
                                    uint16_t maximumInterval, uint8_t* output, TintaReviewCounts* counts);

#ifdef __cplusplus
}
#endif
