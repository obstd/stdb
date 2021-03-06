/*!
 * \file resp.cc
 */
#include "stdb/protocol/resp.h"

#include "gtest/gtest.h"

namespace stdb {
namespace protocol {

TEST(TestResp, Test_respstream_read_integer) {
  const char* buffer = ":1234567890\r\n";
  MemStreamReader stream(buffer, 14);
  RESPStream resp(&stream);
  EXPECT_EQ(resp.next_type(), RESPStream::INTEGER);
  bool success;
  u64 result;
  std::tie(success, result) = resp.read_int();
  EXPECT_TRUE(success);
  EXPECT_EQ(result, 1234567890);
}

TEST(TestResp, Test_respstream_read_incomplete_integer) {
  const char* buffer = ":123456";
  MemStreamReader stream(buffer, 7);
  RESPStream resp(&stream);
  EXPECT_EQ(resp.next_type(), RESPStream::INTEGER);
  bool success;
  u64 result;
  std::tie(success, result) = resp.read_int();
  EXPECT_TRUE(!success);
}

TEST(TestResp, Test_respstream_read_integer_wrong_type) {
  const char* buffer = "+1234567890\r\n";
  MemStreamReader stream(buffer, 14);
  RESPStream resp(&stream);
  EXPECT_EQ(resp.next_type(), RESPStream::STRING);
  EXPECT_THROW(resp.read_int(), RESPError);
}

TEST(TestResp, Test_respstream_read_integer_bad_value) {
  const char* buffer = ":123fl\r\n";
  MemStreamReader stream(buffer, 14);
  RESPStream resp(&stream);
  EXPECT_THROW(resp.read_int(), RESPError);
}

TEST(TestResp, Test_respstream_read_integer_bad_end_seq) {
  const char* buffer = ":1234567890\r00\r\n";
  MemStreamReader stream(buffer, 16);
  RESPStream resp(&stream);
  EXPECT_THROW(resp.read_int(), RESPError);
}

TEST(TestResp, Test_respstream_read_integer_too_long) {
  const char* buffer = ":"
      "11111111111111111111"
      "22222222222222222222"
      "11111111111111111111"
      "22222222222222222222"
      "11110000000000000000"
      "\r\n";
  // Integer is too long
  MemStreamReader stream(buffer, 104);
  RESPStream resp(&stream);
  EXPECT_THROW(resp.read_int(), RESPError);
}

TEST(TestResp, Test_respstream_read_string) {
  const char* orig = "+foobar\r\n";
  MemStreamReader stream(orig, 10);
  RESPStream resp(&stream);
  EXPECT_EQ(resp.next_type(), RESPStream::STRING);
  const size_t buffer_size = RESPStream::STRING_LENGTH_MAX;
  Byte buffer[buffer_size];
  bool success;
  int bytes;
  std::tie(success, bytes) = resp.read_string(buffer, buffer_size);
  EXPECT_TRUE(success);
  EXPECT_TRUE(bytes > 0);
  EXPECT_EQ(bytes, 6);
  EXPECT_EQ(std::string(buffer, buffer + bytes), "foobar");
}

TEST(TestResp, Test_respstream_read_string_wrong_type) {
  const char* orig = ":foobar\r\n";
  MemStreamReader stream(orig, 10);
  RESPStream resp(&stream);
  const size_t buffer_size = RESPStream::STRING_LENGTH_MAX;
  Byte buffer[buffer_size];
  EXPECT_THROW(resp.read_string(buffer, buffer_size), RESPError);
}

TEST(TestResp, Test_respstream_read_string_small_buffer) {
  const char* orig = "+foobar\r\n";
  MemStreamReader stream(orig, 10);
  RESPStream resp(&stream);
  const size_t buffer_size = 4;
  Byte buffer[buffer_size];
  EXPECT_THROW(resp.read_string(buffer, buffer_size), RESPError);
}

TEST(TestResp, Test_respstream_read_string_large_string) {
  std::string orig = "+";
  for (int i = 0; i < RESPStream::STRING_LENGTH_MAX + 1; i++) {
    orig.push_back('X');
  }
  orig.push_back('\r');
  orig.push_back('\n');
  MemStreamReader stream(orig.data(), orig.size());
  RESPStream resp(&stream);
  const size_t buffer_size = RESPStream::STRING_LENGTH_MAX;
  Byte buffer[buffer_size];
  EXPECT_THROW(resp.read_string(buffer, buffer_size), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring) {
  const char* orig = "$6\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  EXPECT_EQ(resp.next_type(), RESPStream::BULK_STR);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  bool success;
  int bytes;
  std::tie(success, bytes) = resp.read_bulkstr(buffer.data(), buffer.size());
  EXPECT_TRUE(success);
  EXPECT_TRUE(bytes > 0);
  EXPECT_EQ(bytes, 6);
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + bytes), "foobar");
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_type) {
  const char* orig = ":6\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  EXPECT_NE(resp.next_type(), RESPStream::BULK_STR);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_header_1) {
  const char* orig = "$f\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_header_2) {
  const char* orig = "$\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_header_3) {
  const char* orig = "$6r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_len_1) {
  const char* orig = "$1\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_len_2) {
  const char* orig = "$7\r\nfoobar\r\n";
  MemStreamReader stream(orig, 13);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_bad_tail) {
  const char* orig = "$6\r\nfoobar\n";
  MemStreamReader stream(orig, 12);
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_bulkstring_too_large_to_handle) {
  std::string orig = "$10000000\r\n";
  for (int i = 10000000; i-->0;) {
    orig.push_back('x');
  }
  orig.push_back('\r');
  orig.push_back('\n');
  MemStreamReader stream(orig.data(), orig.size());
  RESPStream resp(&stream);
  std::vector<Byte> buffer;
  buffer.resize(RESPStream::BULK_LENGTH_MAX);
  EXPECT_THROW(resp.read_string(buffer.data(), buffer.size()), RESPError);
}

TEST(TestResp, Test_respstream_read_array) {
  const char* orig = "*3\r\n:1\r\n:2\r\n:3\r\n:8\r\n";
  MemStreamReader stream(orig, 21);
  RESPStream resp(&stream);
  bool success;
  u64 size;
  std::tie(success, size) = resp.read_array_size();
  EXPECT_TRUE(success);
  EXPECT_EQ(size, 3);
  u64 first;
  std::tie(success, first) = resp.read_int();
  EXPECT_TRUE(success);
  EXPECT_EQ(first, 1);
  u64 second;
  std::tie(success, second) = resp.read_int();
  EXPECT_TRUE(success);
  EXPECT_EQ(second, 2);
  u64 third;
  std::tie(success, third) = resp.read_int();
  EXPECT_TRUE(success);
  EXPECT_EQ(third, 3);
  // Read value after array end
  u64 eight;
  std::tie(success, eight) = resp.read_int();
  EXPECT_TRUE(success);
  EXPECT_EQ(eight, 8);
}

TEST(TestResp, Test_respstream_read_array_bad_call) {
  const char* orig = ":2\r\n:1\r\n:2\r\n:3\r\n";
  MemStreamReader stream(orig, 17);
  RESPStream resp(&stream);
  EXPECT_THROW(resp.read_array_size(), RESPError);
}

TEST(TestResp, Test_respstream_read_array_cant_parse) {
  const char* orig = "*X\r\n:1\r\n:2\r\n:3\r\n";
  MemStreamReader stream(orig, 17);
  RESPStream resp(&stream);
  EXPECT_THROW(resp.read_array_size(), RESPError);
}

}  // namespace protocol
}  // namespace stdb
