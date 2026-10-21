#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <string>

#include "ymh/core/version.hpp"

namespace {

std::string generated_header_version() {
    std::ifstream header(YMH_GENERATED_VERSION_HEADER);
    const std::regex define_pattern(R"re(#define YMH_VERSION "([^"]+)")re");
    std::string line;
    while (std::getline(header, line)) {
        std::smatch match;
        if (std::regex_search(line, match, define_pattern)) {
            return match[1].str();
        }
    }
    return {};
}

}  // namespace

TEST(VersionStamp, MacroHasStampShape) {
    const std::string version{YMH_VERSION};
    EXPECT_TRUE(std::regex_match(version, std::regex(R"(^[0-9]{2}-[A-Z][a-z]{2}-[0-9]{4}:#[0-9]{2,}$)")))
        << version;
    EXPECT_NE(version, "unknown");
}

TEST(VersionStamp, MacroEqualsGeneratedHeader) {
    EXPECT_EQ(std::string{YMH_VERSION}, generated_header_version());
}
