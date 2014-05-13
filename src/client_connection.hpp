/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_CLIENT_CONNECTION_HPP_INCLUDED__
#define __CASS_CLIENT_CONNECTION_HPP_INCLUDED__

#include "common.hpp"
#include "host.hpp"
#include "message.hpp"
#include "session.hpp"
#include "ssl_context.hpp"
#include "ssl_session.hpp"
#include "stream_storage.hpp"

#define CASS_STREAM_ID_MAX      127

namespace cass {

struct ClientConnection {
  enum ClientConnectionState {
    CLIENT_STATE_NEW,
    CLIENT_STATE_CONNECTED,
    CLIENT_STATE_HANDSHAKE,
    CLIENT_STATE_SUPPORTED,
    CLIENT_STATE_READY,
    CLIENT_STATE_DISCONNECTING,
    CLIENT_STATE_DISCONNECTED
  };

  enum Compression {
    CLIENT_COMPRESSION_NONE,
    CLIENT_COMPRESSION_SNAPPY,
    CLIENT_COMPRESSION_LZ4
  };

  enum SchemaEventType {
    CLIENT_EVENT_SCHEMA_CREATED,
    CLIENT_EVENT_SCHEMA_UPDATED,
    CLIENT_EVENT_SCHEMA_DROPPED
  };

  typedef int8_t Stream;

  typedef std::function<void(ClientConnection*,
                             Error*)> ConnectionCallback;

  typedef std::function<void(ClientConnection*)> RequestFinishedCallback;

  typedef std::function<void(ClientConnection*,
                             const char*, size_t)> KeyspaceCallback;

  typedef std::function<void(ClientConnection*,
                             SchemaEventType,
                             const char*, size_t,
                             const char*, size_t)> SchemaCallback;

  typedef std::function<void(ClientConnection*,
                             Error*,
                             const char*, size_t,
                             const char*, size_t)> PrepareCallback;

  typedef StreamStorage<
    Stream,
    MessageFutureImpl*,
    CASS_STREAM_ID_MAX> StreamStorageCollection;

  struct WriteRequestData {
    uv_buf_t buf;
    ClientConnection* connection;
  };

  ClientConnectionState       state_;
  uv_loop_t*                  loop_;
  std::unique_ptr<Message>    incoming_;
  StreamStorageCollection     stream_storage_;
  ConnectionCallback          connect_callback_;
  RequestFinishedCallback     request_finished_callback_;
  KeyspaceCallback            keyspace_callback_;
  PrepareCallback             prepare_callback_;
  LogCallback                 log_callback_;
  // DNS and hostname stuff
  Host                     host_;
  // the actual connection
  uv_connect_t                connect_request_;
  uv_tcp_t                    socket_;
  // ssl stuff
  SSLSession*                 ssl_;
  bool                        ssl_handshake_done_;
  // supported stuff sent in start up message
  std::string                 compression_;
  std::string                 version_;

  explicit
  ClientConnection(
      uv_loop_t*         loop,
      SSLSession*        ssl_session,
      const Host&     host) :
      state_(CLIENT_STATE_NEW),
      loop_(loop),
      incoming_(new Message()),
      connect_callback_(nullptr),
      request_finished_callback_(nullptr),
      keyspace_callback_(nullptr),
      prepare_callback_(nullptr),
      log_callback_(nullptr),         // use ipv4 by default
      host_(host),
      ssl_(ssl_session),
      ssl_handshake_done_(false),
      version_("3.0.0") {
    connect_request_.data         = this;
    socket_.data                  = this;

    if (ssl_) {
      ssl_->init();
      ssl_->handshake(true);
    }
  }

  inline void
  log(
      int         level,
      const char* message,
      size_t      size) {
    if (log_callback_) {
      log_callback_(level, message, size);
    }
  }

  inline void
  log(
      int         level,
      const char* message) {
    if (log_callback_) {
      log_callback_(level, message, strlen(message));
    }
  }

  inline size_t
  available_streams() {
    return stream_storage_.available_streams();
  }

  void
  event_received() {
    log(CASS_LOG_DEBUG, "event received");

    switch (state_) {
      case CLIENT_STATE_NEW:
        connect();
        break;
      case CLIENT_STATE_CONNECTED:
        ssl_handshake();
        break;
      case CLIENT_STATE_HANDSHAKE:
        send_options();
        break;
      case CLIENT_STATE_SUPPORTED:
        send_startup();
        break;
      case CLIENT_STATE_READY:
        notify_ready();
        break;
      default:
        assert(false);
    }
  }

  void
  consume(
      char*  input,
      size_t size) {
    char* buffer    = input;
    int   remaining = size;

    while (remaining != 0) {
      int consumed = incoming_->consume(buffer, remaining);
      if (consumed < 0) {
        // TODO(mstump) probably means connection closed/failed
        fprintf(stderr, "consume error\n");
      }

      if (incoming_->body_ready) {
        Message* message = incoming_.release();
        incoming_.reset(new Message());

        char log_message[512];
        snprintf(
            log_message,
            sizeof(log_message),
            "consumed message type %s with stream %d, input %zd, remaining %d",
            opcode_to_string(message->opcode).c_str(),
            message->stream,
            size,
            remaining);

        log(CASS_LOG_DEBUG, log_message);
        if (message->stream < 0) {
          // TODO(mstump) system events
          assert(false);
        } else {
          switch (message->opcode) {
            case CQL_OPCODE_SUPPORTED:
              on_supported(message);
              break;
            case CQL_OPCODE_ERROR:
              on_error(message);
              break;
            case CQL_OPCODE_READY:
              on_ready(message);
              break;
            case CQL_OPCODE_RESULT:
              on_result(message);
              break;
            default:
              assert(false);
              break;
          }
        }
      }
      remaining -= consumed;
      buffer    += consumed;
    }
  }

  static void
  on_close(
      uv_handle_t* client) {
    ClientConnection* connection
        = reinterpret_cast<ClientConnection*>(client->data);

    connection->log(CASS_LOG_DEBUG, "on_close");
    connection->state_ = CLIENT_STATE_DISCONNECTED;
    connection->event_received();
  }

  static void
  on_read(
      uv_stream_t* client,
      ssize_t      nread,
      uv_buf_t     buf) {
    ClientConnection* connection =
        reinterpret_cast<ClientConnection*>(client->data);

    connection->log(CASS_LOG_DEBUG, "on_read");
    if (nread == -1) {
      if (uv_last_error(connection->loop_).code != UV_EOF) {
        fprintf(stderr,
                "Read error %s\n",
                uv_err_name(uv_last_error(connection->loop_)));
      }
      connection->close();
      return;
    }

    if (connection->ssl_) {
      char*  read_input        = buf.base;
      size_t read_input_size   = nread;

      for (;;) {
        size_t read_size         = 0;
        char*  read_output       = NULL;
        size_t read_output_size  = 0;
        char*  write_output      = NULL;
        size_t write_output_size = 0;

        // TODO(mstump) error handling for SSL decryption
        connection->ssl_->read_write(
            read_input,
            read_input_size,
            read_size,
            &read_output,
            read_output_size,
            NULL,
            0,
            &write_output,
            write_output_size);

        if (read_output && read_output_size) {
          // TODO(mstump) error handling
          connection->consume(read_output, read_output_size);
          delete read_output;
        }

        if (write_output && write_output_size) {
          connection->send_data(write_output, write_output_size);
          // delete of write_output will be handled by on_write
        }

        if (read_size < read_input_size) {
          read_input += read_size;
          read_input_size -= read_size;
        } else {
          break;
        }

        if (!connection->ssl_handshake_done_) {
          if (connection->ssl_->handshake_done()) {
            connection->state_ = CLIENT_STATE_HANDSHAKE;
            connection->event_received();
          }
        }
      }
    } else {
      connection->consume(buf.base, nread);
    }
    free_buffer(buf);
  }

  Error*
  send_data(
      char*  input,
      size_t size) {
    return send_data(uv_buf_init(input, size));
  }

  Error*
  send_data(
      uv_buf_t buf) {
    uv_write_t        *req  = new uv_write_t;
    WriteRequestData*  data = new WriteRequestData;
    data->buf               = buf;
    data->connection        = this;
    req->data               = data;
    uv_write(
        req,
        reinterpret_cast<uv_stream_t*>(&socket_),
        &buf,
        1,
        ClientConnection::on_write);
    return nullptr;
  }

  void
  close() {
    log(CASS_LOG_DEBUG, "close");
    state_ = CLIENT_STATE_DISCONNECTING;
    uv_close(
        reinterpret_cast<uv_handle_t*>(&socket_),
        ClientConnection::on_close);
  }

  static void
  on_connect(
      uv_connect_t*     request,
      int               status) {
    ClientConnection* connection
        = reinterpret_cast<ClientConnection*>(request->data);

    connection->log(CASS_LOG_DEBUG, "on_connect");
    if (status == -1) {
      // TODO(mstump)
      fprintf(
          stderr,
          "connect failed error %s (%s)\n",
          uv_err_name(uv_last_error(connection->loop_)),
          connection->host_.address.to_string().c_str());
      if (connection->connect_callback_) {
        connection->connect_callback_(connection, NULL); // TODO: need error
      }
      return;
    }

    uv_read_start(
        reinterpret_cast<uv_stream_t*>(&connection->socket_),
        alloc_buffer,
        on_read);

    connection->state_ = CLIENT_STATE_CONNECTED;
    connection->event_received();
  }

  void
  connect() {
    log(CASS_LOG_DEBUG, "connect");
    // connect to the resolved host
    uv_tcp_init(loop_, &socket_);
    if(host_.address.family() == AF_INET) {
      uv_tcp_connect(
          &connect_request_,
          &socket_,
          *host_.address.addr_in(),
          ClientConnection::on_connect);
    } else {
      uv_tcp_connect6(
          &connect_request_,
          &socket_,
          *host_.address.addr_in6(),
          ClientConnection::on_connect);
    }
  }

  void
  ssl_handshake() {
    if (ssl_) {
      // calling read on a handshaked initiated ssl_ pipe
      // will gives us the first message to send to the server
      on_read(
          reinterpret_cast<uv_stream_t*>(&socket_),
          0,
          alloc_buffer(0));
    } else {
      state_ = CLIENT_STATE_HANDSHAKE;
      event_received();
    }
  }

  void
  on_result(
      Message* response) {
    log(CASS_LOG_DEBUG, "on_result");

    Error* error = nullptr;
    MessageFutureImpl* request = nullptr;
    Result*     result  = static_cast<Result*>(response->body.get());

    switch (result->kind) {
      case CASS_RESULT_KIND_SET_KEYSPACE:
        if (keyspace_callback_) {
          keyspace_callback_(this, result->keyspace, result->keyspace_size);
        }
        break;

      case CASS_RESULT_KIND_PREPARED:
        error = stream_storage_.get_stream(response->stream, request);
        if(error) {
          request->error.reset(error);
        } else {
          request->result.reset(response);
        }
        request->notify(loop_);
        prepare_callback_(
            this,
            error,
            request->data.c_str(),
            request->data.size(),
            result->prepared,
            result->prepared_size);
        break;

      default:
        error = stream_storage_.get_stream(response->stream, request);
        if(error) {
          request->error.reset(error);
        } else {
          request->result.reset(response);
        }
        request->notify(loop_);
        break;
    }

    if(request_finished_callback_) {
      request_finished_callback_(this);
    }
  }

  void
  on_error(
      Message* response) {
    log(CASS_LOG_DEBUG, "on_error");
    BodyError* error = static_cast<BodyError*>(response->body.get());

    if (state_ < CLIENT_STATE_READY) {
      notify_error(
          new Error(
              CASS_ERROR_SOURCE_SERVER,
              CASS_OK, // TODO(mpenick): Need valid error
              error->message,
              __FILE__,
              __LINE__));
    }
    delete response;
  }

  void
  on_ready(
      Message* response) {
    log(CASS_LOG_DEBUG, "on_ready");
    delete response;
    state_ = CLIENT_STATE_READY;
    event_received();
  }

  void
  on_supported(
      Message* response) {
    log(CASS_LOG_DEBUG, "on_supported");
    BodySupported* supported
        = static_cast<BodySupported*>(response->body.get());

    // TODO(mstump) do something with the supported info
    (void) supported;

    delete response;
    state_ = CLIENT_STATE_SUPPORTED;
    event_received();
  }

  void
  set_keyspace(
      const std::string& keyspace) {
    Message message(CQL_OPCODE_QUERY);
    QueryStatement* query = static_cast<QueryStatement*>(message.body.get());
    query->statement("USE " + keyspace);
    execute(&message, NULL);
  }

  void
  notify_ready() {
    log(CASS_LOG_DEBUG, "notify_ready");
    if (connect_callback_) {
      connect_callback_(this, NULL);
    }
  }

  void
  notify_error(
      Error* err) {
    log(CASS_LOG_DEBUG, "notify_error");
    if (connect_callback_) {
      connect_callback_(this, err);
    }
  }

  void
  send_options() {
    log(CASS_LOG_DEBUG, "send_options");
    Message message(CQL_OPCODE_OPTIONS);
    execute(&message, NULL);
  }

  void
  send_startup() {
    log(CASS_LOG_DEBUG, "send_startup");
    Message      message(CQL_OPCODE_STARTUP);
    BodyStartup* startup = static_cast<BodyStartup*>(message.body.get());
    startup->version = version_;
    execute(&message, NULL);
  }

  static void
  on_write(
      uv_write_t* req,
      int         status) {
    WriteRequestData* data
        = reinterpret_cast<WriteRequestData*>(req->data);

    ClientConnection* connection = data->connection;
    connection->log(CASS_LOG_DEBUG, "on_write");
    if (status == -1) {
      // TODO(mstump) need to trigger failure for all pending requests and notify connection close
      fprintf(
          stderr,
          "Write error %s\n",
          uv_err_name(uv_last_error(connection->loop_)));
    }
    delete data->buf.base;
    delete data;
    delete req;
  }

  Error*
  execute(
      Message* message,
      MessageFutureImpl* request = NULL) {
    uv_buf_t   buf;

    Error* error = stream_storage_.set_stream(request, message->stream);
    if(error) {
      return error;
    }

    if (!message->prepare(&buf.base, buf.len)) {
      return CASS_ERROR(CASS_ERROR_SOURCE_LIBRARY,
                        CASS_ERROR_LIB_MESSAGE_PREPARE,
                        "error preparing message");
    }

    char log_message[512];
    snprintf(
        log_message,
        sizeof(log_message),
        "sending message type %s with stream %d, size %zd",
        opcode_to_string(message->opcode).c_str(),
        message->stream,
        buf.len);

    log(CASS_LOG_DEBUG, log_message);
    return send_data(buf);
  }

  void
  init(
      ConnectionCallback connect     = nullptr,
      RequestFinishedCallback request_finished = nullptr,
      KeyspaceCallback   keyspace    = nullptr
      // SchemaCallback     schema   = nullptr,
      // TopologyCallback   topology = nullptr,
      // StatusCallback     status   = nullptr
       ) {
    connect_callback_     = connect;
    request_finished_callback_ = request_finished;
    keyspace_callback_    = keyspace;

    // schema_callback   = schema;
    // topology_callback = topology;
    // status_callback   = status;
    event_received();
  }

  void
  shutdown() {
  }

 private:
  ClientConnection(const ClientConnection&) {}
  void operator=(const ClientConnection&) {}
};

} // namespace cass

#endif
