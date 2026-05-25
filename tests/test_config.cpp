/**
 * @file test_config.cpp
 * @brief Config 单元测试 —— 验证配置文件加载、默认值导出与未知 key 容忍。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "corodb/common/config.h"

using namespace corodb;

namespace {

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& c = Config::instance();
        saved_page_size_ = c.page_size();
        saved_buffer_pages_ = c.buffer_pages();
        saved_memtable_ = c.memtable_size_bytes();
        saved_port_ = c.server_port();
        saved_data_dir_ = c.data_dir();
        saved_max_conn_ = c.max_connections();
        saved_gc_delay_ = c.group_commit_delay_us();
    }
    void TearDown() override {
        auto& c = Config::instance();
        c.set_page_size(saved_page_size_);
        c.set_buffer_pages(saved_buffer_pages_);
        c.set_memtable_size_bytes(saved_memtable_);
        c.set_server_port(saved_port_);
        c.set_data_dir(saved_data_dir_);
        c.set_max_connections(saved_max_conn_);
        c.set_group_commit_delay_us(saved_gc_delay_);
        if (!tmp_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(tmp_path_, ec);
        }
    }

    std::string write_temp(const std::string& content) {
        auto p = std::filesystem::temp_directory_path() /
                 ("corodb_test_config_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                  std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".conf");
        std::ofstream ofs(p);
        ofs << content;
        ofs.close();
        tmp_path_ = p.string();
        return tmp_path_;
    }

    std::size_t saved_page_size_{};
    std::size_t saved_buffer_pages_{};
    std::size_t saved_memtable_{};
    uint16_t saved_port_{};
    std::string saved_data_dir_;
    std::size_t saved_max_conn_{};
    uint32_t saved_gc_delay_{};
    std::string tmp_path_;
};

TEST_F(ConfigTest, LoadsBasicKeyValues) {
    const std::string content = R"(
# 注释
[storage]
page_size = 4096
buffer_pages = 128
memtable_size_bytes = 524288

[wal]
group_commit_delay_us = 2500

[server]
port = 5555
data_dir = /tmp/corodb-data
max_connections = 50
)";
    auto path = write_temp(content);
    std::string err;
    ASSERT_TRUE(Config::instance().load_from_file(path, &err)) << err;

    auto& c = Config::instance();
    EXPECT_EQ(c.page_size(), 4096u);
    EXPECT_EQ(c.buffer_pages(), 128u);
    EXPECT_EQ(c.memtable_size_bytes(), 524288u);
    EXPECT_EQ(c.group_commit_delay_us(), 2500u);
    EXPECT_EQ(c.server_port(), 5555u);
    EXPECT_EQ(c.data_dir(), "/tmp/corodb-data");
    EXPECT_EQ(c.max_connections(), 50u);
}

TEST_F(ConfigTest, FlatKeysWorkWithoutSection) {
    auto path = write_temp("storage.page_size = 16384\nserver.port=9000\n");
    ASSERT_TRUE(Config::instance().load_from_file(path));
    EXPECT_EQ(Config::instance().page_size(), 16384u);
    EXPECT_EQ(Config::instance().server_port(), 9000u);
}

TEST_F(ConfigTest, UnknownKeysAreIgnored) {
    auto path = write_temp("storage.page_size = 4096\nstorage.unknown_key = 123\nfoo.bar = baz\n");
    EXPECT_TRUE(Config::instance().load_from_file(path));
    EXPECT_EQ(Config::instance().page_size(), 4096u);
}

TEST_F(ConfigTest, MissingFileReturnsFalse) {
    std::string err;
    EXPECT_FALSE(Config::instance().load_from_file("/__nope__/does_not_exist.conf", &err));
    EXPECT_FALSE(err.empty());
}

TEST_F(ConfigTest, CommentsAndBlankLines) {
    auto path = write_temp("# header\n\n   ; another\nserver.port = 1234 # trailing\n");
    ASSERT_TRUE(Config::instance().load_from_file(path));
    EXPECT_EQ(Config::instance().server_port(), 1234u);
}

TEST_F(ConfigTest, WriteDefaultFileRoundTrip) {
    auto p = (std::filesystem::temp_directory_path() / "corodb_default.conf").string();
    ASSERT_TRUE(Config::write_default_file(p));
    tmp_path_ = p;

    // 修改单例后，再次加载默认文件应当还原
    Config::instance().set_server_port(7777);
    ASSERT_TRUE(Config::instance().load_from_file(p));
    EXPECT_EQ(Config::instance().server_port(), Config::kDefaultServerPort);
    EXPECT_EQ(Config::instance().page_size(), Config::kDefaultPageSize);
}

TEST_F(ConfigTest, InvalidPortIsRejectedSilently) {
    Config::instance().set_server_port(4000);
    auto path = write_temp("server.port = 99999\n");
    ASSERT_TRUE(Config::instance().load_from_file(path));
    EXPECT_EQ(Config::instance().server_port(), 4000u);  // 未被改写
}

} // namespace
