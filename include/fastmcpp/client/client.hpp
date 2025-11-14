#pragma once
#include <memory>
#include <string>
#include "fastmcpp/types.hpp"
#include "fastmcpp/server/server.hpp"

namespace fastmcpp::client {

class ITransport {
 public:
  virtual ~ITransport() = default;
  virtual fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) = 0;
};

class LoopbackTransport : public ITransport {
 public:
  explicit LoopbackTransport(std::shared_ptr<fastmcpp::server::Server> server) : server_(std::move(server)) {}
  fastmcpp::Json request(const std::string& route, const fastmcpp::Json& payload) override {
    return server_->handle(route, payload);
  }

 private:
  std::shared_ptr<fastmcpp::server::Server> server_;
};

class Client {
 public:
  Client() = default;
  explicit Client(std::unique_ptr<ITransport> t) : transport_(std::move(t)) {}
  void set_transport(std::unique_ptr<ITransport> t) { transport_ = std::move(t); }
  fastmcpp::Json call(const std::string& route, const fastmcpp::Json& payload) {
    return transport_->request(route, payload);
  }

 private:
  std::unique_ptr<ITransport> transport_;
};

} // namespace fastmcpp::client

