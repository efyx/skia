/*
 * Copyright 2018 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrVkPipelineLayout.h"
#include "GrVkGpu.h"
#include "GrVkUtil.h"

void GrVkPipelineLayout::freeGPUData(const GrVkGpu* gpu) const {
    GR_VK_CALL(gpu->vkInterface(), DestroyPipelineLayout(gpu->device(), fPipelineLayout, nullptr));
}
