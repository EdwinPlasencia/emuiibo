#include <ipc/nfp/nfp_Common.hpp>

namespace ipc::nfp {

    static void VirtualAmiiboStatusUpdateThread(void *iface_data) {
        auto iface_ptr = reinterpret_cast<CommonInterface*>(iface_data);
        while(true) {
            if(iface_ptr->ShouldExitThread()) {
                break;
            }
            auto status = sys::GetActiveVirtualAmiiboStatus();
            iface_ptr->HandleVirtualAmiiboStatus(status);
            svcSleepThread(100'000'000ul);
        }
        EMU_LOG_FMT("Exiting...")
    }

    CommonInterface::CommonInterface(Service fwd, u64 app_id) : state(NfpState_NonInitialized), device_state(NfpDeviceState_Unavailable), forward_service(fwd), client_app_id(app_id), amiibo_update_lock(true), should_exit_thread(false) {
        EMU_LOG_FMT("Ctor started");
        sys::RegisterInterceptedApplicationId(this->client_app_id);
        EMU_R_ASSERT(ams::os::CreateSystemEvent(&this->event_activate, ams::os::EventClearMode_AutoClear, true));
        EMU_R_ASSERT(ams::os::CreateSystemEvent(&this->event_deactivate, ams::os::EventClearMode_AutoClear, true));
        EMU_R_ASSERT(ams::os::CreateSystemEvent(&this->event_availability_change, ams::os::EventClearMode_AutoClear, true));
        EMU_R_ASSERT(threadCreate(&this->amiibo_update_thread, &VirtualAmiiboStatusUpdateThread, reinterpret_cast<void*>(this), nullptr, 0x1000, 0x2B, -2));
        EMU_R_ASSERT(threadStart(&this->amiibo_update_thread));
        EMU_LOG_FMT("Ctor ended");
    }

    CommonInterface::~CommonInterface() {
        EMU_LOG_FMT("Dtor started");
        serviceClose(&this->forward_service);
        sys::UnregisterInterceptedApplicationId(this->client_app_id);
        this->NotifyThreadExitAndWait();
        EMU_LOG_FMT("Dtor ended");
    }

    void CommonInterface::HandleVirtualAmiiboStatus(sys::VirtualAmiiboStatus status) {
        EMU_LOCK_SCOPE_WITH(this->amiibo_update_lock);
        auto state = this->GetDeviceStateValue();
        switch(status) {
            case sys::VirtualAmiiboStatus::Connected: {
                switch(state) {
                    case NfpDeviceState_SearchingForTag: {
                        // The client was waiting for an amiibo, tell it that it's connected now
                        this->SetDeviceStateValue(NfpDeviceState_TagFound);
                        ams::os::SignalSystemEvent(&this->event_activate);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case sys::VirtualAmiiboStatus::Disconnected: {
                switch(state) {
                    case NfpDeviceState_TagFound:
                    case NfpDeviceState_TagMounted: {
                        // The client thinks that the amiibo is connected, tell it that it was disconnected
                        this->SetDeviceStateValue(NfpDeviceState_SearchingForTag);
                        ams::os::SignalSystemEvent(&this->event_deactivate);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }

    NfpState CommonInterface::GetStateValue() {
        EMU_LOCK_SCOPE_WITH(this->amiibo_update_lock);
        return this->state;
    }

    void CommonInterface::SetStateValue(NfpState val) {
        EMU_LOCK_SCOPE_WITH(this->amiibo_update_lock);
        this->state = val;
    }

    NfpDeviceState CommonInterface::GetDeviceStateValue() {
        EMU_LOCK_SCOPE_WITH(this->amiibo_update_lock);
        return this->device_state;
    }

    void CommonInterface::SetDeviceStateValue(NfpDeviceState val) {
        EMU_LOCK_SCOPE_WITH(this->amiibo_update_lock);
        this->device_state = val;
    }

    void CommonInterface::Initialize(const ams::sf::ClientAppletResourceUserId &client_aruid, const ams::sf::ClientProcessId &client_pid, const ams::sf::InBuffer &mcu_data) {
        EMU_LOG_FMT("Process ID: 0x" << std::hex << client_pid.GetValue().value << ", ARUID: 0x" << std:: hex << client_aruid.GetValue().value)
        this->SetStateValue(NfpState_Initialized);
        this->SetDeviceStateValue(NfpDeviceState_Initialized);
    }

    void CommonInterface::Finalize() {
        EMU_LOG_FMT("Finalizing...")
        this->SetStateValue(NfpState_NonInitialized);
        this->SetDeviceStateValue(NfpDeviceState_Finalized);
    }

    ams::Result CommonInterface::ListDevices(const ams::sf::OutPointerArray<DeviceHandle> &out_devices, ams::sf::Out<s32> out_count) {
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        
        EMU_LOG_FMT("Device handle array length: " << out_devices.GetSize())
        // Here, in emuiibo, we will only return one available device(handheld or player 1)
        DeviceHandle handle = {};
        handle.npad_id = HandheldNpadId;
        // If player 1 is connected (aka joycons are detached), use that id, otherwise handheld will be connected
        hidScanInput();
        if(hidIsControllerConnected(CONTROLLER_PLAYER_1)) {
            handle.npad_id = Player1NpadId;
        }
        out_devices[0] = handle;
        out_count.SetValue(1);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::StartDetection(DeviceHandle handle) {
        EMU_LOG_FMT("Started detection")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        R_UNLESS(this->IsStateAny<NfpDeviceState>(NfpDeviceState_Initialized, NfpDeviceState_TagRemoved), result::nfp::ResultDeviceNotFound);

        this->SetDeviceStateValue(NfpDeviceState_SearchingForTag);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::StopDetection(DeviceHandle handle) {
        EMU_LOG_FMT("Stopped detection")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);

        this->SetDeviceStateValue(NfpDeviceState_Initialized);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::Mount(DeviceHandle handle, u32 type, u32 target) {
        EMU_LOG_FMT("Mounted")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);

        this->SetDeviceStateValue(NfpDeviceState_TagMounted);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::Unmount(DeviceHandle handle) {
        EMU_LOG_FMT("Unmounted")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);

        this->device_state = NfpDeviceState_TagFound;
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::Flush(DeviceHandle handle) {
        EMU_LOG_FMT("Flushed")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::Restore(DeviceHandle handle) {
        EMU_LOG_FMT("Restored")
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::GetTagInfo(ams::sf::Out<TagInfo> out_info, DeviceHandle handle) {
        auto &amiibo = sys::GetActiveVirtualAmiibo();
        EMU_LOG_FMT("Tag info - is amiibo valid? " << std::boolalpha << amiibo.IsValid() << ", amiibo name: " << amiibo.GetName())
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        R_UNLESS(this->IsStateAny<NfpDeviceState>(NfpDeviceState_TagFound, NfpDeviceState_TagMounted), result::nfp::ResultDeviceNotFound);
        R_UNLESS(amiibo.IsValid(), result::nfp::ResultDeviceNotFound);

        auto info = amiibo.ProduceTagInfo();
        out_info.SetValue(info);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::GetRegisterInfo(ams::sf::Out<RegisterInfo> out_info, DeviceHandle handle) {
        auto &amiibo = sys::GetActiveVirtualAmiibo();
        EMU_LOG_FMT("Register info - is amiibo valid? " << std::boolalpha << amiibo.IsValid() << ", amiibo name: " << amiibo.GetName())
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        R_UNLESS(this->IsStateAny<NfpDeviceState>(NfpDeviceState_TagMounted), result::nfp::ResultDeviceNotFound);
        R_UNLESS(amiibo.IsValid(), result::nfp::ResultDeviceNotFound);

        auto info = amiibo.ProduceRegisterInfo();
        out_info.SetValue(info);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::GetModelInfo(ams::sf::Out<ModelInfo> out_info, DeviceHandle handle) {
        auto &amiibo = sys::GetActiveVirtualAmiibo();
        EMU_LOG_FMT("Model info - is amiibo valid? " << std::boolalpha << amiibo.IsValid() << ", amiibo name: " << amiibo.GetName())
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        R_UNLESS(this->IsStateAny<NfpDeviceState>(NfpDeviceState_TagMounted), result::nfp::ResultDeviceNotFound);
        R_UNLESS(amiibo.IsValid(), result::nfp::ResultDeviceNotFound);

        auto info = amiibo.ProduceModelInfo();
        out_info.SetValue(info);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::GetCommonInfo(ams::sf::Out<CommonInfo> out_info, DeviceHandle handle) {
        auto &amiibo = sys::GetActiveVirtualAmiibo();
        EMU_LOG_FMT("Common info - is amiibo valid? " << std::boolalpha << amiibo.IsValid() << ", amiibo name: " << amiibo.GetName())
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        R_UNLESS(this->IsStateAny<NfpDeviceState>(NfpDeviceState_TagMounted), result::nfp::ResultDeviceNotFound);
        R_UNLESS(amiibo.IsValid(), result::nfp::ResultDeviceNotFound);

        auto info = amiibo.ProduceCommonInfo();
        out_info.SetValue(info);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::AttachActivateEvent(DeviceHandle handle, ams::sf::Out<ams::sf::CopyHandle> event) {
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);

        event.SetValue(ams::os::GetReadableHandleOfSystemEvent(&this->event_activate));
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::AttachDeactivateEvent(DeviceHandle handle, ams::sf::Out<ams::sf::CopyHandle> event) {
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);

        event.SetValue(ams::os::GetReadableHandleOfSystemEvent(&this->event_deactivate));
        return ams::ResultSuccess();
    }

    void CommonInterface::GetState(ams::sf::Out<u32> out_state) {
        auto state = this->GetStateValue();
        EMU_LOG_FMT("State: " << static_cast<u32>(state));
        out_state.SetValue(static_cast<u32>(state));
    }

    void CommonInterface::GetDeviceState(DeviceHandle handle, ams::sf::Out<u32> out_state) {
        auto state = this->GetDeviceStateValue();
        EMU_LOG_FMT("Device state: " << static_cast<u32>(state));
        out_state.SetValue(static_cast<u32>(state));
    }

    ams::Result CommonInterface::GetNpadId(DeviceHandle handle, ams::sf::Out<u32> out_npad_id) {
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        
        out_npad_id.SetValue(handle.npad_id);
        return ams::ResultSuccess();
    }

    ams::Result CommonInterface::AttachAvailabilityChangeEvent(ams::sf::Out<ams::sf::CopyHandle> event) {
        R_UNLESS(this->IsStateAny<NfpState>(NfpState_Initialized), result::nfp::ResultDeviceNotFound);
        
        event.SetValue(ams::os::GetReadableHandleOfSystemEvent(&this->event_availability_change));
        return ams::ResultSuccess();
    }

    ams::Result ManagerBase::CreateForwardInterface(Service *manager, Service *out) {
        R_UNLESS(sys::GetEmulationStatus() == sys::EmulationStatus::On, ams::sm::mitm::ResultShouldForwardToSession());
        R_TRY(serviceDispatch(manager, 0,
            .out_num_objects = 1,
            .out_objects = out,
        ));
        EMU_LOG_FMT("Created custom NFP interface for emuiibo!")
        return ams::ResultSuccess();
    }

}