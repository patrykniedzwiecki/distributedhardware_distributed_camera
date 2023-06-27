/*
 * Copyright (c) 2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "decodeonoutputbufferavailable_fuzzer.h"

#include "decode_video_callback.h"

namespace OHOS {
namespace DistributedHardware {
void DecodeOnOutputBufferAvailableFuzzTest(const uint8_t* data, size_t size)
{
    if ((data == nullptr) || (size < sizeof(int64_t))) {
        return;
    }

    uint32_t index = *(reinterpret_cast<const uint32_t*>(data));
    int64_t presentTimeUs = *(reinterpret_cast<const int64_t*>(data));
    int32_t infoSize = *(reinterpret_cast<const int32_t*>(data));
    int32_t offset = *(reinterpret_cast<const int32_t*>(data));
    int32_t bufferFlag = *(reinterpret_cast<const int32_t*>(data));
    Media::AVCodecBufferInfo info = { presentTimeUs, infoSize, offset};
    Media::AVCodecBufferFlag flag = static_cast<Media::AVCodecBufferFlag>(bufferFlag);
    std::shared_ptr<DCameraPipelineSource> sourcePipeline = std::make_shared<DCameraPipelineSource>();
    std::shared_ptr<EventBus> eventBusPipeline = nullptr;
    std::shared_ptr<DecodeDataProcess> decodeDataProcess =
        std::make_shared<DecodeDataProcess>(eventBusPipeline, sourcePipeline);
    std::shared_ptr<DecodeVideoCallback> decodeVideoCallback = std::make_shared<DecodeVideoCallback>(decodeDataProcess);

    decodeVideoCallback->OnOutputBufferAvailable(index, info, flag);
}
}
}

/* Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    /* Run your code on data */
    OHOS::DistributedHardware::DecodeOnOutputBufferAvailableFuzzTest(data, size);
    return 0;
}
