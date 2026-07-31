#pragma once

#include "palmier/json/json_document.hpp"

namespace palmier::json::testing {

enum class ParseCheckpoint {
    progress,
    value,
    stringBytes,
};

class ParseCheckpoints {
public:
    virtual ~ParseCheckpoints() = default;
    virtual void arrive(ParseCheckpoint checkpoint, std::size_t amount) = 0;
};

Value parse(
    std::string_view source,
    std::stop_token cancellation,
    ParseCheckpoints* checkpoints
);

}
