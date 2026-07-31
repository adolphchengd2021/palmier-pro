#pragma once

#include "palmier/project/project.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace palmier::project {

using IdGenerator = std::function<std::string()>;

class ReadError final : public std::runtime_error {
public:
    ReadError(std::string code, std::string jsonPointer, std::string detail);

    const std::string code;
    const std::string jsonPointer;
};

ProjectDocument readProject(std::string_view source, const IdGenerator& idGenerator);
ProjectDocument readProject(palmier::json::Value source, const IdGenerator& idGenerator);
std::string normalizedModelJson(const ProjectDocument& document);

}
