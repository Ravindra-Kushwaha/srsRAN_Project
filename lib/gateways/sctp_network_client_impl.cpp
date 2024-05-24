/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "sctp_network_client_impl.h"
#include "srsran/support/io/sockets.h"
#include <netinet/sctp.h>

using namespace srsran;

// the stream number to use for sending
const unsigned stream_no = 0;

class sctp_network_client_impl::sctp_send_notifier final : public sctp_association_sdu_notifier
{
public:
  sctp_send_notifier(sctp_network_client_impl& parent_, const transport_layer_address& server_addr_) :
    client_name(parent_.client_name),
    ppid(parent_.node_cfg.ppid),
    fd(parent_.socket.fd().value()),
    logger(parent_.logger),
    server_addr(server_addr_),
    closed_flag(parent_.shutdown_received)
  {
  }
  ~sctp_send_notifier() override { close(); }

  bool on_new_sdu(byte_buffer sdu) override
  {
    if (closed_flag->load(std::memory_order_relaxed)) {
      // Already closed.
      return false;
    }

    if (sdu.length() > network_gateway_sctp_max_len) {
      logger.error("PDU of {} bytes exceeds maximum length of {} bytes", sdu.length(), network_gateway_sctp_max_len);
      return false;
    }
    logger.debug("Sending PDU of {} bytes", sdu.length());

    // Note: each sender needs its own buffer to avoid race conditions with the recv.
    std::array<uint8_t, network_gateway_sctp_max_len> temp_send_buffer;
    span<const uint8_t>                               pdu_span = to_span(sdu, temp_send_buffer);

    auto dest_addr  = server_addr.native();
    int  bytes_sent = sctp_sendmsg(fd,
                                  pdu_span.data(),
                                  pdu_span.size(),
                                  const_cast<struct sockaddr*>(dest_addr.addr),
                                  dest_addr.addrlen,
                                  htonl(ppid),
                                  0,
                                  stream_no,
                                  0,
                                  0);
    if (bytes_sent == -1) {
      logger.error("{}: Closing SCTP association. Cause: Couldn't send {} B of data. errno={}",
                   client_name,
                   pdu_span.size_bytes(),
                   strerror(errno));
      close();
      return false;
    }
    return true;
  }

private:
  void close()
  {
    if (closed_flag->load(std::memory_order_relaxed)) {
      // Already closed.
      return;
    }

    // Send EOF to SCTP server.
    auto dest_addr  = server_addr.native();
    int  bytes_sent = sctp_sendmsg(fd,
                                  nullptr,
                                  0,
                                  const_cast<struct sockaddr*>(dest_addr.addr),
                                  dest_addr.addrlen,
                                  htonl(ppid),
                                  SCTP_EOF,
                                  stream_no,
                                  0,
                                  0);

    if (bytes_sent == -1) {
      // Failed to send EOF.
      // Note: It may happen when the sender notifier is removed just before the SCTP shutdown event is handled in
      // the server recv thread.
      logger.info("{}: Couldn't send EOF during shut down (errno=\"{}\")", client_name, strerror(errno));
    } else {
      logger.debug("{}: Sent EOF to SCTP client and closed SCTP association", client_name);
    }

    // Signal sender closed the channel.
    closed_flag->store(true, std::memory_order_relaxed);
  }

  const std::string             client_name;
  const uint32_t                ppid;
  int                           fd;
  srslog::basic_logger&         logger;
  const transport_layer_address server_addr;

  std::shared_ptr<std::atomic<bool>> closed_flag;
};

sctp_network_client_impl::sctp_network_client_impl(const std::string&                 client_name_,
                                                   const sctp_network_gateway_config& sctp_cfg,
                                                   io_broker&                         broker_) :
  sctp_network_gateway_common_impl(sctp_cfg),
  client_name(client_name_),
  broker(broker_),
  temp_recv_buffer(network_gateway_sctp_max_len)
{
}

sctp_network_client_impl::~sctp_network_client_impl()
{
  logger.debug("{}: Closing...", client_name);

  // Signal that the upper layer sender should stop sending new SCTP data (including the EOF).
  if (shutdown_received != nullptr) {
    shutdown_received->store(true);
    shutdown_received = nullptr;
  }

  io_sub.reset();

  {
    // Note: we have to protect the shutdown of the socket in case the io_broker is handling concurrently the io_broker
    // unsubscription.
    // TODO: Refactor.
    std::lock_guard<std::mutex> lock(shutdown_mutex);
    socket.close();
  }

  logger.info("{}: SCTP client closed", client_name);
}

std::unique_ptr<sctp_association_sdu_notifier>
sctp_network_client_impl::connect_to(const std::string&                             connection_name,
                                     const std::string&                             dest_addr,
                                     int                                            dest_port,
                                     std::unique_ptr<sctp_association_sdu_notifier> recv_handler_)
{
  if (shutdown_received != nullptr and not shutdown_received->load(std::memory_order_relaxed)) {
    // If this is not the first connection.
    logger.error("{}: New connection to {} on {}:{} failed. Cause: Connection is already in progress",
                 client_name,
                 connection_name,
                 dest_addr,
                 dest_port);
    return nullptr;
  }
  if (not node_cfg.bind_address.empty()) {
    // Make sure to close any socket created for any previous connection.
    std::lock_guard<std::mutex> lock(shutdown_mutex);
    socket.close();
  }

  logger.info("{}: Connecting to {} on {}:{}...", client_name, connection_name, dest_addr, dest_port);
  fmt::print("{}: Connecting to {} on {}:{}...\n", client_name, connection_name, dest_addr, dest_port);

  sockaddr_searcher searcher{dest_addr, dest_port, logger};
  auto              start = std::chrono::steady_clock::now();
  // Create SCTP socket only if not created earlier during bind. Otherwise, reuse socket.
  bool             reuse_socket = socket.is_open();
  struct addrinfo* result       = nullptr;
  for (auto candidate = searcher.next(); candidate != nullptr and result == nullptr; candidate = searcher.next()) {
    if (not reuse_socket) {
      // Create SCTP socket only if not created earlier through bind or another connection.
      expected<sctp_socket> outcome = create_socket(candidate->ai_family, candidate->ai_socktype);
      if (outcome.is_error()) {
        if (errno == ESOCKTNOSUPPORT) {
          // Stop the search.
          break;
        }
        continue;
      }
      socket = std::move(outcome.value());
    }

    bool connection_success = socket.connect(*candidate->ai_addr, candidate->ai_addrlen);
    if (not connection_success) {
      // connection failed, but before trying the next address, make sure the just created socket is deleted.
      if (not reuse_socket) {
        socket.close();
      }
      continue;
    }

    // Register the socket in the IO broker.
    io_sub = broker.register_fd(
        socket.fd().value(),
        [this]() { receive(); },
        [this](io_broker::error_code code) {
          std::string cause = fmt::format("IO error code={}", (int)code);
          handle_connection_close(cause.c_str());
        });
    if (not io_sub.registered()) {
      // IO subscription failed.
      if (not reuse_socket) {
        socket.close();
      }
      continue;
    }

    // Found a valid candidate.
    result = candidate;
  }

  if (result == nullptr) {
    auto        end    = std::chrono::steady_clock::now();
    auto        now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::string cause  = strerror(errno);
    if (errno == 0) {
      cause = "IO broker could not register socket";
    }
    fmt::print("{}: Failed to connect to {} on {}:{}. error=\"{}\" timeout={}ms\n",
               client_name,
               connection_name,
               dest_addr,
               dest_port,
               cause,
               now_ms.count());
    logger.error("{}: Failed to connect SCTP socket to {}:{}. error=\"{}\" timeout={}ms",
                 client_name,
                 dest_addr,
                 dest_port,
                 cause,
                 now_ms.count());
    return nullptr;
  }

  // Subscribe to the IO broker.
  recv_handler      = std::move(recv_handler_);
  shutdown_received = std::make_shared<std::atomic<bool>>(false);
  auto addr         = transport_layer_address::create_from_sockaddr(*result->ai_addr, result->ai_addrlen);

  logger.info("{}: SCTP connection to {}:{} was successful", client_name, dest_addr, dest_port);

  return std::make_unique<sctp_send_notifier>(*this, addr);
}

void sctp_network_client_impl::receive()
{
  struct sctp_sndrcvinfo sri       = {};
  int                    msg_flags = 0;
  // fromlen is an in/out variable in sctp_recvmsg.
  sockaddr_storage msg_src_addr;
  socklen_t        msg_src_addrlen = sizeof(msg_src_addr);

  int rx_bytes = ::sctp_recvmsg(socket.fd().value(),
                                temp_recv_buffer.data(),
                                temp_recv_buffer.size(),
                                (struct sockaddr*)&msg_src_addr,
                                &msg_src_addrlen,
                                &sri,
                                &msg_flags);

  // Handle error.
  if (rx_bytes == -1) {
    if (errno != EAGAIN) {
      std::string cause = fmt::format("Error reading from SCTP socket: {}", strerror(errno));
      handle_connection_close(cause.c_str());
    } else {
      if (!node_cfg.non_blocking_mode) {
        logger.debug("Socket timeout reached");
      }
    }
    return;
  }

  span<const uint8_t> payload(temp_recv_buffer.data(), rx_bytes);
  if (msg_flags & MSG_NOTIFICATION) {
    handle_notification(payload, sri, (const sockaddr&)msg_src_addr, msg_src_addrlen);
  } else {
    handle_data(payload);
  }
}

void sctp_network_client_impl::handle_connection_close(const char* cause)
{
  if (shutdown_received == nullptr) {
    // It has already been closed.
    return;
  }

  // Signal that the upper layer sender should stop sending new SCTP data (including the EOF, which would fail anyway).
  bool prev         = shutdown_received->exchange(true);
  shutdown_received = nullptr;

  if (not prev and cause != nullptr) {
    // The SCTP sender (the upper layers) didn't yet close the connection.
    logger.info("{}: SCTP connection was shut down. Cause: {}", client_name, cause);
  }
}

void sctp_network_client_impl::handle_sctp_shutdown_comp()
{
  // Notify the connection drop to the SCTP sender.
  recv_handler.reset();

  // Unsubscribe from listening to new IO events.
  std::lock_guard<std::mutex> lock(shutdown_mutex);
  io_sub.reset();
}

void sctp_network_client_impl::handle_data(span<const uint8_t> payload)
{
  logger.debug("{}: Received {} bytes", client_name, payload.size());

  // Note: For SCTP, we avoid byte buffer allocation failures by resorting to fallback allocation.
  recv_handler->on_new_sdu(byte_buffer{byte_buffer::fallback_allocation_tag{}, payload});
}

void sctp_network_client_impl::handle_notification(span<const uint8_t>           payload,
                                                   const struct sctp_sndrcvinfo& sri,
                                                   const sockaddr&               src_addr,
                                                   socklen_t                     src_addr_len)
{
  if (not validate_and_log_sctp_notification(payload)) {
    // Handle error.
    handle_connection_close("The received message is invalid");
    return;
  }

  const auto* notif = reinterpret_cast<const union sctp_notification*>(payload.data());
  switch (notif->sn_header.sn_type) {
    case SCTP_ASSOC_CHANGE: {
      const struct sctp_assoc_change* n = &notif->sn_assoc_change;
      switch (n->sac_state) {
        case SCTP_COMM_UP:
          break;
        case SCTP_COMM_LOST:
          handle_connection_close("Communication to the server was lost");
          break;
        case SCTP_SHUTDOWN_COMP:
          handle_sctp_shutdown_comp();
          break;
        case SCTP_CANT_STR_ASSOC:
          handle_connection_close("Can't start association");
          break;
        default:
          break;
      }
      break;
    }
    case SCTP_SHUTDOWN_EVENT: {
      handle_connection_close("Server closed SCTP association");
      break;
    }
    default:
      break;
  }
}

std::unique_ptr<sctp_network_client> sctp_network_client_impl::create(const std::string&                 client_name,
                                                                      const sctp_network_gateway_config& sctp_cfg,
                                                                      io_broker&                         broker_)
{
  // Create a SCTP server instance.
  std::unique_ptr<sctp_network_client_impl> server{new sctp_network_client_impl(client_name, sctp_cfg, broker_)};

  // If a bind address is provided, create a socket here and bind it.
  if (not sctp_cfg.bind_address.empty()) {
    if (not server->create_and_bind_common()) {
      return nullptr;
    }
  }

  return server;
}
