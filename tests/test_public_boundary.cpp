// 公开契约边界：仅包含 rin 公开头（目标未链接第三方 include 路径），
// 并实例化契约类型/接口，确保公开头零第三方类型（RULE-01）。
#include "test_util.hpp"

#include <memory>

#include <rin/camera_service.hpp>

namespace {

class NullService final : public rin::ICameraService {
public:
    rin::StartOutcome start(const rin::StreamRequest&) override { return {}; }
    bool requestResolution(const rin::StreamRequest&, std::string*) override { return false; }
    bool requestDevice(const std::string&, std::string*) override { return false; }
    bool requestDepthColorScheme(rin::DepthColorScheme, std::string*) override
    {
        return false;
    }
    void stop() override {}
    rin::CameraServiceState state() const override { return rin::CameraServiceState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadFrame(rin::FrameKind, std::uint64_t&, rin::Frame&) override { return false; }
    bool tryLoadIntrinsics(std::uint64_t&, rin::IntrinsicsSnapshot&) override { return false; }
    bool tryLoadCatalog(std::uint64_t&, rin::DeviceCatalog&) override { return false; }
    bool tryLoadEvent(rin::ServiceEvent&) override { return false; }
};

}  // namespace

int main() {
    std::shared_ptr<rin::ICameraService> service = std::make_shared<NullService>();
    RIN_CHECK(service != nullptr);
    RIN_CHECK(!service->start({}).admitted);
    return rin_test::exitStatus();
}
