#pragma once
#include "common/buffer/buffer_impl.h"

#include "extensions/filters/network/mysql_proxy/mysql_codec.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MySQLProxy {

class ClientSwitchResponse : public MySQLCodec {
public:
  // MySQLCodec
  int Decode(Buffer::Instance& buffer) override;
  std::string Encode() override;

  int GetSeq() const { return seq_; }
  void SetSeq(int seq);
  const std::string& GetAuthPluginResp() const { return auth_plugin_resp_; }
  void SetAuthPluginResp(std::string& auth_swith_resp);

private:
  std::string auth_plugin_resp_;
  int seq_;
};

} // namespace MySQLProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
