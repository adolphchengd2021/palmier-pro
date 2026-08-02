extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <iostream>
#include <string_view>

namespace {

struct Component final {
    std::string_view name;
    unsigned int version;
    const char* license;
    const char* configuration;
};

bool contains(std::string_view value, std::string_view token) {
    return value.find(token) != std::string_view::npos;
}

bool acceptable(const Component& component) {
    if (component.license == nullptr || component.configuration == nullptr) return false;
    const std::string_view license(component.license);
    const std::string_view configuration(component.configuration);
    return contains(license, "LGPL")
        && !contains(configuration, "--enable-gpl")
        && !contains(configuration, "--enable-nonfree");
}

}

int main() {
    const std::array components{
        Component{"libavcodec", avcodec_version(), avcodec_license(), avcodec_configuration()},
        Component{"libavformat", avformat_version(), avformat_license(), avformat_configuration()},
        Component{"libavutil", avutil_version(), avutil_license(), avutil_configuration()},
        Component{"libswresample", swresample_version(), swresample_license(), swresample_configuration()},
        Component{"libswscale", swscale_version(), swscale_license(), swscale_configuration()},
    };
    bool valid = true;
    for (const auto& component : components) {
        valid = acceptable(component) && valid;
        std::cout
            << "component=" << component.name << '\n'
            << "version=" << component.version << '\n'
            << "license=" << (component.license == nullptr ? "<missing>" : component.license) << '\n'
            << "configuration="
            << (component.configuration == nullptr ? "<missing>" : component.configuration)
            << "\n---\n";
    }
    if (!valid) {
        std::cerr << "FFmpeg distribution probe refused a GPL, nonfree, or non-LGPL component.\n";
        return 2;
    }
    return 0;
}
