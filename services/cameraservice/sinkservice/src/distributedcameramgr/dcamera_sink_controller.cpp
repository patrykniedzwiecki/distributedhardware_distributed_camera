/*
 * Copyright (c) 2021-2022 Huawei Device Co., Ltd.
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

#include "dcamera_sink_controller.h"

#include <securec.h>
#include <thread>

#include "anonymous_string.h"
#include "dcamera_channel_sink_impl.h"
#include "dcamera_client.h"
#include "dcamera_metadata_setting_cmd.h"
#include "dcamera_protocol.h"
#include "dcamera_utils_tools.h"

#include "dcamera_sink_access_control.h"
#include "dcamera_sink_controller_channel_listener.h"
#include "dcamera_sink_controller_state_callback.h"
#include "dcamera_sink_output.h"
#include "dcamera_sink_service_ipc.h"

#include "distributed_camera_constants.h"
#include "distributed_camera_errno.h"
#include "distributed_hardware_log.h"
#include "idistributed_camera_source.h"
#include "json/json.h"
#include "dcamera_low_latency.h"

namespace OHOS {
namespace DistributedHardware {
DCameraSinkController::DCameraSinkController(std::shared_ptr<ICameraSinkAccessControl>& accessControl)
    : isInit_(false), sessionState_(DCAMERA_CHANNEL_STATE_DISCONNECTED), accessControl_(accessControl)
{
}

DCameraSinkController::~DCameraSinkController()
{
    if (isInit_) {
        UnInit();
    }
}

int32_t DCameraSinkController::StartCapture(std::vector<std::shared_ptr<DCameraCaptureInfo>>& captureInfos)
{
    DHLOGI("StartCapture dhId: %s", GetAnonyString(dhId_).c_str());
    std::string accessType = "";
    if ((accessControl_->IsSensitiveSrcAccess(SRC_TYPE)) &&
        (accessControl_->GetAccessControlType(accessType) == DCAMERA_SAME_ACCOUNT)) {
        int32_t ret = StartCaptureInner(captureInfos);
        if (ret == DCAMERA_OK) {
            accessControl_->NotifySensitiveSrc(SRC_TYPE);
        }
        return ret;
    } else {
        std::string param = "";
        DCameraFrameTriggerEvent triggerEvent(*this, param);
        DCameraPostAuthorizationEvent authEvent(*this, captureInfos);
        eventBus_->PostEvent<DCameraFrameTriggerEvent>(triggerEvent, POSTMODE::POST_ASYNC);
        eventBus_->PostEvent<DCameraPostAuthorizationEvent>(authEvent, POSTMODE::POST_ASYNC);
    }
    return DCAMERA_OK;
}

int32_t DCameraSinkController::StopCapture()
{
    DHLOGI("StopCapture dhId: %s", GetAnonyString(dhId_).c_str());
    std::lock_guard<std::mutex> autoLock(captureLock_);
    int32_t ret = operator_->StopCapture();
    if (ret != DCAMERA_OK) {
        DHLOGE("client stop capture failed, dhId: %s, ret: %d",
               GetAnonyString(dhId_).c_str(), ret);
        DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_ERROR, std::string("operator stop capture failed."));
        return ret;
    }

    ret = output_->StopCapture();
    if (ret != DCAMERA_OK) {
        DHLOGE("output stop capture failed, dhId: %s, ret: %d",
               GetAnonyString(dhId_).c_str(), ret);
        DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_ERROR, std::string("output stop capture failed"));
        return ret;
    }

    DHLOGI("StopCapture %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::ChannelNeg(std::shared_ptr<DCameraChannelInfo>& info)
{
    DHLOGI("ChannelNeg dhId: %s", GetAnonyString(dhId_).c_str());
    int32_t ret = output_->OpenChannel(info);
    if (ret != DCAMERA_OK) {
        DHLOGE("channel negotiate failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    DHLOGI("ChannelNeg %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::DCameraNotify(std::shared_ptr<DCameraEvent>& events)
{
    DHLOGI("DCameraNotify dhId: %s", GetAnonyString(dhId_).c_str());
    if (srcDevId_.empty()) {
        DHLOGE("source deviceId is empty");
        return DCAMERA_BAD_VALUE;
    }

    sptr<IDistributedCameraSource> sourceSA = DCameraSinkServiceIpc::GetInstance().GetSourceRemoteCamSrv(srcDevId_);
    if (sourceSA == nullptr) {
        DHLOGE("sourceSA is null");
        return DCAMERA_BAD_VALUE;
    }

    DCameraEventCmd eventCmd;
    std::string jsonStr = "";
    eventCmd.type_ = DCAMERA_PROTOCOL_TYPE_MESSAGE;
    eventCmd.dhId_ = dhId_;
    eventCmd.command_ = DCAMERA_PROTOCOL_CMD_STATE_NOTIFY;
    eventCmd.value_ = events;
    int32_t ret = eventCmd.Marshal(jsonStr);
    if (ret != DCAMERA_OK) {
        DHLOGE("DCameraEventCmd marshal failed, dhId: %s, ret: %d",
               GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    std::string sinkDevId;
    ret = GetLocalDeviceNetworkId(sinkDevId);
    if (ret != DCAMERA_OK) {
        DHLOGE("GetLocalDeviceNetworkId failed, devId: %s, dhId: %s, ret: %d",
               GetAnonyString(sinkDevId).c_str(), GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    ret = sourceSA->DCameraNotify(sinkDevId, dhId_, jsonStr);
    if (ret != DCAMERA_OK) {
        DHLOGE("SourceSA notify failed, srcId: %s, sinkId: %s, dhId: %s, ret: %d",
               GetAnonyString(srcDevId_).c_str(), GetAnonyString(sinkDevId).c_str(),
               GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    DHLOGI("DCameraNotify %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::UpdateSettings(std::vector<std::shared_ptr<DCameraSettings>>& settings)
{
    DHLOGI("UpdateSettings dhId: %s", GetAnonyString(dhId_).c_str());
    int32_t ret = operator_->UpdateSettings(settings);
    if (ret != DCAMERA_OK) {
        DHLOGE("UpdateSettings failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    DHLOGI("UpdateSettings %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::GetCameraInfo(std::shared_ptr<DCameraInfo>& camInfo)
{
    DHLOGI("GetCameraInfo dhId: %s, session state: %d", GetAnonyString(dhId_).c_str(), sessionState_);
    camInfo->state_ = sessionState_;
    return DCAMERA_OK;
}

int32_t DCameraSinkController::OpenChannel(std::shared_ptr<DCameraOpenInfo>& openInfo)
{
    DHLOGI("OpenChannel dhId: %s", GetAnonyString(dhId_).c_str());
    if (sessionState_ != DCAMERA_CHANNEL_STATE_DISCONNECTED) {
        DHLOGE("wrong state, dhId: %s, sessionState: %d", GetAnonyString(dhId_).c_str(), sessionState_);
        return DCAMERA_WRONG_STATE;
    }
    DCameraLowLatency::GetInstance().EnableLowLatency();
    srcDevId_ = openInfo->sourceDevId_;
    std::vector<DCameraIndex> indexs;
    indexs.push_back(DCameraIndex(srcDevId_, dhId_));
    auto controller = std::shared_ptr<DCameraSinkController>(shared_from_this());
    std::shared_ptr<ICameraChannelListener> listener =
        std::make_shared<DCameraSinkControllerChannelListener>(controller);
    int32_t ret = channel_->CreateSession(indexs, SESSION_FLAG, DCAMERA_SESSION_MODE_CTRL, listener);
    if (ret != DCAMERA_OK) {
        DHLOGE("channel create session failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }
    DHLOGI("OpenChannel %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::CloseChannel()
{
    DHLOGI("CloseChannel dhId: %s", GetAnonyString(dhId_).c_str());
    std::lock_guard<std::mutex> autoLock(channelLock_);
    DCameraLowLatency::GetInstance().DisableLowLatency();
    DCameraSinkServiceIpc::GetInstance().DeleteSourceRemoteCamSrv(srcDevId_);
    srcDevId_.clear();
    int32_t ret = channel_->ReleaseSession();
    if (ret != DCAMERA_OK) {
        DHLOGE("DCameraSinkController release channel failed, dhId: %s, ret: %d",
               GetAnonyString(dhId_).c_str(), ret);
    }

    ret = output_->CloseChannel();
    if (ret != DCAMERA_OK) {
        DHLOGE("DCameraSinkController CloseChannel failed, dhId: %s, ret: %d",
               GetAnonyString(dhId_).c_str(), ret);
    }
    sessionState_ = DCAMERA_CHANNEL_STATE_DISCONNECTED;
    DHLOGI("CloseChannel %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::Init(std::vector<DCameraIndex>& indexs)
{
    DHLOGI("DCameraSinkController Init");
    dhId_ = indexs[0].dhId_;
    operator_ = std::make_shared<DCameraClient>(dhId_);
    output_ = std::make_shared<DCameraSinkOutput>(dhId_, operator_);
    int32_t ret = output_->Init();
    if (ret != DCAMERA_OK) {
        DHLOGE("output init failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    auto controller = std::shared_ptr<DCameraSinkController>(shared_from_this());
    std::shared_ptr<StateCallback> stateCallback = std::make_shared<DCameraSinkControllerStateCallback>(controller);
    operator_->SetStateCallback(stateCallback);
    ret = operator_->Init();
    if (ret != DCAMERA_OK) {
        DHLOGE("operator init failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return ret;
    }

    channel_ = std::make_shared<DCameraChannelSinkImpl>();
    eventBus_ = std::make_shared<EventBus>("SinkCtlHandler");
    DCameraFrameTriggerEvent triggerEvent(*this);
    DCameraPostAuthorizationEvent authEvent(*this);
    eventBus_->AddHandler<DCameraFrameTriggerEvent>(triggerEvent.GetType(), *this);
    eventBus_->AddHandler<DCameraPostAuthorizationEvent>(authEvent.GetType(), *this);

    isInit_ = true;
    DHLOGI("DCameraSinkController Init %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::UnInit()
{
    DHLOGI("DCameraSinkController UnInit dhId: %s", GetAnonyString(dhId_).c_str());
    std::lock_guard<std::mutex> autoLock(autoLock_);
    int32_t ret = StopCapture();
    if (ret != DCAMERA_OK) {
        DHLOGE("DCameraSinkController UnInit %s stop capture failed, ret: %d", GetAnonyString(dhId_).c_str(), ret);
    }

    ret = CloseChannel();
    if (ret != DCAMERA_OK) {
        DHLOGE("DCameraSinkController UnInit %s close channel failed, ret: %d", GetAnonyString(dhId_).c_str(), ret);
    }

    if (output_ != nullptr) {
        ret = output_->UnInit();
        if (ret != DCAMERA_OK) {
            DHLOGE("DCameraSinkController release output failed, dhId: %s, ret: %d",
                   GetAnonyString(dhId_).c_str(), ret);
        }
    }

    if (operator_ != nullptr) {
        ret = operator_->UnInit();
        if (ret != DCAMERA_OK) {
            DHLOGE("DCameraSinkController release operator failed, dhId: %s, ret: %d",
                   GetAnonyString(dhId_).c_str(), ret);
        }
    }

    isInit_ = false;
    DHLOGI("DCameraSinkController UnInit %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

void DCameraSinkController::OnEvent(DCameraFrameTriggerEvent& event)
{
    std::string param = event.GetParam();
    accessControl_->TriggerFrame(param);
}

void DCameraSinkController::OnEvent(DCameraPostAuthorizationEvent& event)
{
    std::vector<std::shared_ptr<DCameraCaptureInfo>> captureInfos = event.GetParam();
    PostAuthorization(captureInfos);
}

void DCameraSinkController::OnStateChanged(std::shared_ptr<DCameraEvent>& event)
{
    DHLOGI("DCameraSinkController::OnStateChanged dhId: %s, result: %d",
           GetAnonyString(dhId_).c_str(), event->eventResult_);
    if (event->eventResult_ == DCAMERA_EVENT_DEVICE_PREEMPT) {
        DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_PREEMPT, std::string("camera device preempted"));
    } else {
        DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_ERROR, std::string("camera error"));
    }
}

void DCameraSinkController::OnMetadataResult(std::vector<std::shared_ptr<DCameraSettings>>& settings)
{
    DHLOGI("DCameraSinkController::OnMetadataResult dhId: %s", GetAnonyString(dhId_).c_str());
    if (settings.empty()) {
        DHLOGE("camera settings is empty");
        return;
    }
    DCameraMetadataSettingCmd cmd;
    cmd.type_ = DCAMERA_PROTOCOL_TYPE_MESSAGE;
    cmd.dhId_ = dhId_;
    cmd.command_ = DCAMERA_PROTOCOL_CMD_METADATA_RESULT;
    cmd.value_.assign(settings.begin(), settings.end());
    std::string jsonStr;
    int32_t ret = cmd.Marshal(jsonStr);
    if (ret != DCAMERA_OK) {
        DHLOGE("Marshal metadata settings failed, dhId: %s ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return;
    }
    std::shared_ptr<DataBuffer> buffer = std::make_shared<DataBuffer>(jsonStr.length() + 1);
    ret = memcpy_s(buffer->Data(), buffer->Capacity(), reinterpret_cast<uint8_t *>(const_cast<char *>(jsonStr.c_str())),
        jsonStr.length());
    if (ret != EOK) {
        DHLOGE("memcpy_s failed, dhId: %s ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return;
    }
    ret = channel_->SendData(buffer);
    if (ret != DCAMERA_OK) {
        DHLOGE("channel send data failed, dhId: %s ret: %d", GetAnonyString(dhId_).c_str(), ret);
        return;
    }
    DHLOGI("DCameraSinkController::OnMetadataResult dhId: %s success", GetAnonyString(dhId_).c_str());
}

void DCameraSinkController::OnSessionState(int32_t state)
{
    DHLOGI("DCameraSinkController::OnSessionState dhId: %s, state: %d", GetAnonyString(dhId_).c_str(), state);
    sessionState_ = state;
    switch (state) {
        case DCAMERA_CHANNEL_STATE_CONNECTING: {
            DHLOGI("channel is connecting");
            break;
        }
        case DCAMERA_CHANNEL_STATE_CONNECTED: {
            DHLOGI("channel is connected");
            break;
        }
        case DCAMERA_CHANNEL_STATE_DISCONNECTED: {
            DHLOGI("channel is disconnected");
            std::thread([this]() {
                DHLOGI("DCameraSinkController::OnSessionState %s new thread session state: %d",
                    GetAnonyString(dhId_).c_str(), sessionState_);
                std::lock_guard<std::mutex> autoLock(autoLock_);
                int32_t ret = CloseChannel();
                if (ret != DCAMERA_OK) {
                    DHLOGE("session state: %d, %s close channel failed, ret: %d",
                        sessionState_, GetAnonyString(dhId_).c_str(), ret);
                }
                ret = StopCapture();
                if (ret != DCAMERA_OK) {
                    DHLOGE("session state: %d, %s stop capture failed, ret: %d",
                        sessionState_, GetAnonyString(dhId_).c_str(), ret);
                }
            }).detach();
            break;
        }
        default: {
            DHLOGE("unknown session state");
            break;
        }
    }
}

void DCameraSinkController::OnSessionError(int32_t eventType, int32_t eventReason, std::string detail)
{
    DHLOGI("DCameraSinkController::OnSessionError dhId: %s, eventType: %d, eventReason: %d, detail: %s",
           GetAnonyString(dhId_).c_str(), eventType, eventReason, detail.c_str());
}

void DCameraSinkController::OnDataReceived(std::vector<std::shared_ptr<DataBuffer>>& buffers)
{
    DHLOGI("OnReceivedData %s control channel receive data", GetAnonyString(dhId_).c_str());
    for (auto& buffer : buffers) {
        if (buffer->Size() <= 0 || buffer->Size() > DATABUFF_MAX_SIZE) {
            DHLOGI("buffer is invalid");
            return;
        }
        HandleReceivedData(buffer);
    }
}

void DCameraSinkController::PostAuthorization(std::vector<std::shared_ptr<DCameraCaptureInfo>>& captureInfos)
{
    DHLOGI("DCameraSinkController::PostAuthorization dhId: %s", GetAnonyString(dhId_).c_str());
    int32_t ret = StartCaptureInner(captureInfos);
    if (ret == DCAMERA_OK) {
        accessControl_->NotifySensitiveSrc(SRC_TYPE);
    }
}

int32_t DCameraSinkController::StartCaptureInner(std::vector<std::shared_ptr<DCameraCaptureInfo>>& captureInfos)
{
    DHLOGI("StartCaptureInner dhId: %s", GetAnonyString(dhId_).c_str());
    std::lock_guard<std::mutex> autoLock(captureLock_);
    int32_t ret = output_->StartCapture(captureInfos);
    if (ret != DCAMERA_OK) {
        DHLOGE("output start capture failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_ERROR, std::string("output start capture failed"));
        return ret;
    }
    PropertyCarrier carrier;
    ret = output_->GetProperty(SURFACE, carrier);
    if (ret != DCAMERA_OK) {
        DHLOGD("StartCaptureInner: get property fail.");
        return DCAMERA_BAD_VALUE;
    }
    ret = operator_->StartCapture(captureInfos, carrier.surface_);
    if (ret != DCAMERA_OK) {
        DHLOGE("camera client start capture failed, dhId: %s, ret: %d", GetAnonyString(dhId_).c_str(), ret);
        if (ret == DCAMERA_ALLOC_ERROR) {
            DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_NO_PERMISSION,
                               std::string("operator start capture permission denial."));
        } else if (ret == DCAMERA_DEVICE_BUSY) {
            DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_DEVICE_IN_USE,
                               std::string("operator start capture in used."));
        }
        return ret;
    }

    DCameraNotifyInner(DCAMERA_MESSAGE, DCAMERA_EVENT_CAMERA_SUCCESS, std::string("operator start capture success"));
    DHLOGI("DCameraSinkController::StartCaptureInner %s success", GetAnonyString(dhId_).c_str());
    return DCAMERA_OK;
}

int32_t DCameraSinkController::DCameraNotifyInner(int32_t type, int32_t result, std::string reason)
{
    std::shared_ptr<DCameraEvent> event = std::make_shared<DCameraEvent>();
    event->eventType_ = type;
    event->eventResult_ = result;
    event->eventContent_ = reason;
    return DCameraNotify(event);
}

int32_t DCameraSinkController::HandleReceivedData(std::shared_ptr<DataBuffer>& dataBuffer)
{
    DHLOGI("DCameraSinkController::HandleReceivedData dhId: %s", GetAnonyString(dhId_).c_str());
    uint8_t *data = dataBuffer->Data();
    std::string jsonStr(reinterpret_cast<const char *>(data), dataBuffer->Capacity());

    JSONCPP_STRING errs;
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
    if ((!jsonReader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.length(), &root, &errs)) ||
        (!root.isObject())) {
        DHLOGE("parse json string failed");
        return DCAMERA_BAD_VALUE;
    }

    if ((!root.isMember("Command")) || (!root["Command"].isString())) {
        DHLOGE("parse command failed");
        return DCAMERA_BAD_VALUE;
    }

    std::string command = root["Command"].asString();
    if ((!command.empty()) && (command.compare(DCAMERA_PROTOCOL_CMD_CAPTURE) == 0)) {
        DCameraCaptureInfoCmd captureInfoCmd;
        int ret = captureInfoCmd.Unmarshal(jsonStr);
        if (ret != DCAMERA_OK) {
            DHLOGE("Capture Info Unmarshal failed, dhId: %s ret: %d", GetAnonyString(dhId_).c_str(), ret);
            return ret;
        }
        return StartCapture(captureInfoCmd.value_);
    } else if ((!command.empty()) && (command.compare(DCAMERA_PROTOCOL_CMD_UPDATE_METADATA) == 0)) {
        DCameraMetadataSettingCmd metadataSettingCmd;
        int ret = metadataSettingCmd.Unmarshal(jsonStr);
        if (ret != DCAMERA_OK) {
            DHLOGE("Metadata Setting Unmarshal failed, dhId: %s ret: %d",
                   GetAnonyString(dhId_).c_str(), ret);
            return ret;
        }
        return UpdateSettings(metadataSettingCmd.value_);
    }
    return DCAMERA_BAD_VALUE;
}
} // namespace DistributedHardware
} // namespace OHOS