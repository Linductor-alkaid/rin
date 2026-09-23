// 公开契约边界：仅包含 rs_vision 公开头（目标未链接第三方 include 路径），
// 并实例化契约类型/接口，确保公开头零第三方类型（RULE-01）。
#include "test_util.hpp"

#include <memory>

#include <rs_vision/camera_service.hpp>

namespace {

class NullService final : public rsv::ICameraService {
public:
    rsv::StartOutcome start(const rsv::StreamRequest&) override { return {}; }
    bool requestResolution(const rsv::StreamRequest&, std::string*) override { return false; }
    void stop() override {}
    rsv::CameraServiceState state() const override { return rsv::CameraServiceState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadFrame(rsv::FrameKind, std::uint64_t&, rsv::Frame&) override { return false; }
    bool tryLoadIntrinsics(std::uint64_t&, rsv::IntrinsicsSnapshot&) override { return false; }
    bool tryLoadCapabilities(std::uint64_t&, rsv::StreamCapabilities&) override {
        return false;
    }
    bool tryLoadEvent(rsv::ServiceEvent&) override { return false; }
};

}  // namespace

int main() {
    std::shared_ptr<rsv::ICameraService> service = std::make_shared<NullService>();
    RSV_CHECK(service != nullptr);
    RSV_CHECK(!service->start({}).admitted);
    return rsv_test::exitStatus();
}
