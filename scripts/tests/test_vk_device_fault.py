"""Exercise production fault collection and bundle writing with a synthetic driver."""
import os
from pathlib import Path
import subprocess
import sys
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child


@pytest.mark.skipif(os.name != "nt", reason="Windows crash bundle writer")
def test_fault_report_reaches_breadcrumb_bundle(tmp_path):
    src = (ROOT / "GameOS/gameos/vk_render_infra.cpp").read_text()
    collect = src[src.index("static void takeDeviceFault() {"):src.index("void deviceLost() {")]
    snapshot = src[src.index("void crashSnapshot(std::string_view folder) {"):src.index("    CloseHandle(f);")]
    source = tmp_path / "fault.cpp"
    source.write_text(r'''
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>
int mode=0, calls=0;
struct VulkanContext {
    bool deviceFaultEnabled() const { return mode!=1; }
    VkDevice device() const { return {}; }
};
VulkanContext ctx;
VulkanContext& getOrCreateEngineVulkanContext() { return ctx; }
struct State {
    char deviceFault[4096]{}, driver[512]{}, packages[4096]{}, ledger[2048]{};
    struct Crumb { bool writing=false; char text[384]{}; };
    std::array<Crumb,256> identities{}, resources{}, crumbs{}, gpuCrumbs{};
    LONG cursor=0;
    uint32_t* markerMap=nullptr;
} s;
State* statePtr=&s;
State& state() { return s; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetDeviceFaultInfoEXT(VkDevice, VkDeviceFaultCountsEXT* c, VkDeviceFaultInfoEXT* i) {
    ++calls;
    if(mode==2 || (mode==3 && i)) return VK_ERROR_UNKNOWN;
    if(!i) { c->addressInfoCount=1; c->vendorInfoCount=1; return VK_SUCCESS; }
    std::strcpy(i->description,"synthetic page fault");
    i->pAddressInfos[0].reportedAddress=0x12345678000;
    i->pAddressInfos[0].addressType=VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT;
    i->pAddressInfos[0].addressPrecision=4096;
    std::strcpy(i->pVendorInfos[0].description,"synthetic vendor");
    i->pVendorInfos[0].vendorFaultCode=0x42;
    i->pVendorInfos[0].vendorFaultData=0x99;
    return VK_SUCCESS;
}
''' + collect + snapshot + r'''
    CloseHandle(f);
}
int main(int argc,char** argv) {
    mode=argc>2 ? argv[2][0]-'0' : 0;
    takeDeviceFault();
    if(calls!=(mode==1?0:mode==2?1:2)) return 1;
    crashSnapshot(argv[1]);
}
''')
    exe = tmp_path / "fault.exe"
    result = subprocess.run(["clang++", "-std=c++20", "-I", str(Path(os.environ["VULKAN_SDK"]) / "Include"),
                             str(source), "-o", str(exe)], capture_output=True, text=True, timeout=60,
                            **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    for mode in range(4):
        folder = tmp_path / str(mode)
        folder.mkdir()
        result = subprocess.run([str(exe), str(folder), str(mode)], capture_output=True, text=True, timeout=30,
                                **console_child.child_kwargs(redirected=True))
        assert result.returncode == 0, result.stderr
        report = (folder / "gpu_breadcrumbs.txt").read_text()
        if mode == 0:
            for text in ("synthetic page fault", "reported_address=0x0000012345678000", "address_precision=0x1000",
                         "synthetic vendor", "code=0x0000000000000042", "data=0x0000000000000099"):
                assert text in report
        elif mode == 1:
            assert "device_fault" not in report
        else:
            assert ("no report" if mode == 2 else "read failed") in report
