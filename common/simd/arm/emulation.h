// Copyright 2009-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

/* Make precision match SSE, at the cost of some performance */
#define SSE2NEON_PRECISE_MINMAX 1
#define SSE2NEON_PRECISE_DIV 1
#define SSE2NEON_PRECISE_SQRT 1

#include "sse2neon.h"

/* Dummy defines for floating point control */
#define _MM_MASK_MASK 0x1f80
#define _MM_MASK_DIV_ZERO 0x200
#define _MM_FLUSH_ZERO_ON 0x8000
#define _MM_MASK_DENORM 0x100
#define _MM_SET_EXCEPTION_MASK(x)
#define _MM_SET_FLUSH_ZERO_MODE(x)

__forceinline int _mm_getcsr()
{
  return 0;
}

__forceinline void _mm_mfence()
{
  __sync_synchronize();
}
