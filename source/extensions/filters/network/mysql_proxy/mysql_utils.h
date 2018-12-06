#pragma once
#include <cstdint>

#include "envoy/common/platform.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/logger.h"

#include "extensions/filters/network/mysql_proxy/mysql_codec_clogin.h"
#include "extensions/filters/network/mysql_proxy/mysql_codec_clogin_resp.h"
#include "extensions/filters/network/mysql_proxy/mysql_codec_command.h"
#include "extensions/filters/network/mysql_proxy/mysql_codec_greeting.h"
#include "extensions/filters/network/mysql_proxy/mysql_codec_switch_resp.h"
#include "extensions/filters/network/mysql_proxy/mysql_session.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MySQLProxy {

/**
 * IO helpers for reading/writing MySQL data from/to a buffer.
 */
class BufferHelper : public Logger::Loggable<Logger::Id::filter> {
public:
  static void addUint8(Buffer::Instance& buffer, uint8_t val);
  static void addUint16(Buffer::Instance& buffer, uint16_t val);
  static void addUint32(Buffer::Instance& buffer, uint32_t val);
  static void addString(Buffer::Instance& buffer, const std::string& str);
  static std::string toString(Buffer::Instance& buffer);
  static std::string encodeHdr(const std::string& cmd_str, int seq);
  static bool endOfBuffer(Buffer::Instance& buffer, uint64_t& offset);
  static int peekUint8(Buffer::Instance& buffer, uint64_t& offset, uint8_t& val);
  static int peekUint16(Buffer::Instance& buffer, uint64_t& offset, uint16_t& val);
  static int peekUint32(Buffer::Instance& buffer, uint64_t& offset, uint32_t& val);
  static int peekUint64(Buffer::Instance& buffer, uint64_t& offset, uint64_t& val);
  static int peekBySize(Buffer::Instance& buffer, uint64_t& offset, int len, int& val);
  static int peekLengthEncodedInteger(Buffer::Instance& buffer, uint64_t& offset, int& val);
  static int peekBytes(Buffer::Instance& buffer, uint64_t& offset, int skip_bytes);
  static int peekString(Buffer::Instance& buffer, uint64_t& offset, std::string& str);
  static int peekStringBySize(Buffer::Instance& buffer, uint64_t& offset, int len,
                              std::string& str);
  static int peekHdr(Buffer::Instance& buffer, uint64_t& offset, int& len, int& seq);
};

} // namespace MySQLProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
