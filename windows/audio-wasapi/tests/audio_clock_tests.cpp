#include "palmier/audio/audio_clock.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using palmier::audio::AudioClockAnchor;
using palmier::audio::AudioClockError;
using palmier::audio::AudioClockFailureCode;
using palmier::audio::AudioClockSample;
using palmier::audio::FrameRate;
using palmier::audio::timelineFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename Operation>
void requireError(Operation operation, AudioClockFailureCode code) {
    try {
        operation();
    } catch (const AudioClockError& error) {
        require(error.code == code, "unexpected audio clock error");
        return;
    }
    throw std::runtime_error("expected audio clock failure");
}

AudioClockAnchor anchor() {
    return {7, 1'000, 48'000, 100};
}

AudioClockSample sample(std::uint64_t position) {
    return {7, position, 123'456'789, false};
}

void mapsDevicePositionWithFloorRounding() {
    const FrameRate ntsc{30'000, 1'001};
    require(timelineFrame(anchor(), sample(49'000), ntsc) == 129, "wrong floor");
    require(timelineFrame(anchor(), sample(49'048), ntsc) == 130, "wrong exact frame");
    require(timelineFrame(anchor(), sample(1'000), ntsc) == 100, "wrong anchor");

    auto degraded = sample(49'000);
    degraded.precisionDegraded = true;
    degraded.qpc100Nanoseconds = 9'999;
    require(timelineFrame(anchor(), degraded, ntsc) == 129, "precision flag changed time");
}

void rejectsInvalidAndStaleSamples() {
    requireError(
        [] { timelineFrame(anchor(), sample(2'000), {0, 1}); },
        AudioClockFailureCode::invalidFrameRate
    );
    requireError(
        [] {
            auto value = anchor();
            value.frequency = 0;
            timelineFrame(value, sample(2'000), {30, 1});
        },
        AudioClockFailureCode::invalidFrequency
    );
    requireError(
        [] {
            auto value = anchor();
            value.timelineFrame = -1;
            timelineFrame(value, sample(2'000), {30, 1});
        },
        AudioClockFailureCode::invalidAnchorFrame
    );
    requireError(
        [] {
            auto value = sample(2'000);
            value.generation = 8;
            timelineFrame(anchor(), value, {30, 1});
        },
        AudioClockFailureCode::staleGeneration
    );
    requireError(
        [] { timelineFrame(anchor(), sample(999), {30, 1}); },
        AudioClockFailureCode::positionDiscontinuity
    );
}

void rejectsArithmeticOverflow() {
    requireError(
        [] {
            AudioClockAnchor value{1, 0, 1, 0};
            AudioClockSample far{1, std::numeric_limits<std::uint64_t>::max(), 0, false};
            timelineFrame(value, far, {std::numeric_limits<std::uint32_t>::max(), 1});
        },
        AudioClockFailureCode::arithmeticOverflow
    );
    requireError(
        [] {
            AudioClockAnchor value{1, 0, 1, std::numeric_limits<std::int64_t>::max()};
            AudioClockSample next{1, 1, 0, false};
            timelineFrame(value, next, {1, 1});
        },
        AudioClockFailureCode::arithmeticOverflow
    );
    requireError(
        [] {
            AudioClockAnchor value{1, 0, std::numeric_limits<std::uint64_t>::max(), 0};
            AudioClockSample next{1, 1, 0, false};
            timelineFrame(
                value,
                next,
                {1, std::numeric_limits<std::uint32_t>::max()}
            );
        },
        AudioClockFailureCode::arithmeticOverflow
    );
}

}

int main() {
    try {
        mapsDevicePositionWithFloorRounding();
        rejectsInvalidAndStaleSamples();
        rejectsArithmeticOverflow();
        std::cout << "WASAPI clock math tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
