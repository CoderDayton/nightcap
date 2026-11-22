#ifndef MOCKTAIL_UI_SLOT_FILES_H_
#define MOCKTAIL_UI_SLOT_FILES_H_

#include <filesystem>
#include <system_error>
#include <vector>

namespace mocktail {
namespace ui {

bool ReplaceAllWith(const std::filesystem::path& source,
                    const std::vector<std::filesystem::path>& targets,
                    std::error_code* failure);

std::filesystem::path FirstRegularFile(
    const std::vector<std::filesystem::path>& files);

bool RemoveAll(const std::vector<std::filesystem::path>& files,
               std::error_code* failure);

}
}

#endif
