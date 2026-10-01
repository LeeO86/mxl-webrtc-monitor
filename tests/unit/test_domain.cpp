#include "doctest/doctest.h"

#include "domain/scan.hpp"

#include <filesystem>
#include <fstream>

TEST_CASE("domain scan uses id and accepts mirror domains")
{
    auto const root = std::filesystem::temp_directory_path() / "mwm-domain-scan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "mirror-abc");
    std::filesystem::create_directories(root / "plain");
    std::filesystem::create_directories(root / "nested" / "child");
    std::filesystem::create_directories(root / "noid");
    {
        std::ofstream out(root / "mirror-abc" / "domain_def.json");
        out << R"({"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","x-mxl-fabrics-agent":{"mode":"mirror"},"ignored":true})";
    }
    {
        std::ofstream out(root / "plain" / "domain_def.json");
        out << R"({"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","label":"plain"})";
    }
    {
        std::ofstream out(root / "nested" / "child" / "domain_def.json");
        out << R"({"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc"})";
    }
    {
        std::ofstream out(root / "noid" / "domain_def.json");
        out << R"({"label":"missing id"})";
    }
    auto const found = mwm::scanDomains(root.string());
    CHECK(found.size() == 2);
    auto const mirror = mwm::resolveDomain(root.string(), "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    REQUIRE(mirror.has_value());
    CHECK(mirror->mirror);
    CHECK(mirror->path.find("mirror-abc") != std::string::npos);
    auto const plain = mwm::resolveDomain(root.string(), "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->mirror);
    CHECK_FALSE(mwm::resolveDomain(root.string(), "cccccccc-cccc-4ccc-8ccc-cccccccccccc").has_value());
    std::filesystem::remove_all(root);
}
