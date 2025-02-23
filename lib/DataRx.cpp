/*
 * Copyright (c) 2017-2022 University of Washington
 * Author: Aaron Marburg <amarburg@uw.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of University of Washington nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "liboculus/DataRx.h"

#include <boost/bind.hpp>

#include "liboculus/Constants.h"

namespace liboculus {

namespace asio = boost::asio;

DataRx::DataRx(const IoServiceThread::IoContextPtr& iosrv)
    : OculusMessageHandler(),
      _socket(*iosrv),
      _buffer(std::make_shared<ByteVector>()),
      _onConnectCallback() {}

DataRx::~DataRx() {}

void DataRx::connect(const asio::ip::address& addr) {
  if (isConnected()) return;

  uint16_t port = liboculus::DataPort;

  boost::asio::ip::tcp::endpoint sonarEndpoint(addr, port);
  LOG(INFO) << "Connecting to sonar at " << sonarEndpoint;

  _socket.async_connect(sonarEndpoint,
                        boost::bind(&DataRx::onConnect, this, _1));
}

void DataRx::connect(const std::string& strAddr) {
  auto addr(boost::asio::ip::address_v4::from_string(strAddr));
  // LOG_IF(FATAL,addr.is_unspecified()) << "Couldn't parse IP address" <<
  // ipAddr;
  connect(addr);
}

void DataRx::onConnect(const boost::system::error_code& ec) {
  if (ec) {
    LOG(WARNING) << "Error on connect: " << ec.message();
    _socket.close();

    return;
  }

  LOG(DEBUG) << "Connected to sonar!";
  restartReceiveCycle();
  if (_onConnectCallback) _onConnectCallback();
}

//=== Readers
void DataRx::readUpTo(size_t bytes, StateMachineCallback callback) {
  const size_t current_sz = _buffer->size();
  _buffer->resize(bytes);
  asio::mutable_buffer buffer_view = asio::buffer(*_buffer) + current_sz;
#if BOOST_VERSION >= 106600
  asio::async_read(_socket, buffer_view, callback);
#else
  asio::async_read(_socket, asio::mutable_buffers_1(buffer_view), callback);
#endif
}

void DataRx::restartReceiveCycle() {
  LOG(DEBUG) << "== Restarting DataRx state machine ==";

  // Before abandoning the current data, post that it's been received
  if (_buffer->size() > 0) haveRead(*_buffer);

  if (_buffer.use_count() > 1) {
    _buffer = std::make_shared<ByteVector>();
  } else {
    _buffer->clear();
  }
  readUpTo(sizeof(uint8_t),
           boost::bind(&DataRx::rxFirstByteOculusId, this, _1, _2));
}

//==== States in the state machine... ====

void DataRx::rxFirstByteOculusId(const boost::system::error_code& ec,
                                 std::size_t bytes_transferred) {
  if (ec) {
    LOG(WARNING) << "Error on receive of header: " << ec.message();
    goto exit;
  }

  if (bytes_transferred != sizeof(uint8_t)) {
    goto exit;
  }

  if (_buffer->data()[0] == liboculus::PacketHeaderLSB) {
    readUpTo(sizeof(uint16_t),
             boost::bind(&DataRx::rxSecondByteOculusId, this, _1, _2));
    return;
  }

exit:
  restartReceiveCycle();
}

void DataRx::rxSecondByteOculusId(const boost::system::error_code& ec,
                                  std::size_t bytes_transferred) {
  if (ec) {
    LOG(WARNING) << "Error on receive of header: " << ec.message();
    goto exit;
  }

  if (bytes_transferred != sizeof(uint8_t)) {
    goto exit;
  }

  if (_buffer->data()[1] == liboculus::PacketHeaderMSB) {
    LOG(DEBUG) << "Received good OculusId at start of packet";

    readUpTo(sizeof(OculusMessageHeader),
             boost::bind(&DataRx::rxHeader, this, _1, _2));
    return;
  }

exit:
  restartReceiveCycle();
}

void DataRx::rxHeader(const boost::system::error_code& ec,
                      std::size_t bytes_transferred) {
  if (ec) {
    LOG(WARNING) << "Error on receive of header: " << ec.message();
    return;
  }

  if (bytes_transferred != (sizeof(OculusMessageHeader) - sizeof(uint16_t))) {
    LOG(WARNING) << "Received short header of " << bytes_transferred
                 << " expected " << sizeof(OculusMessageHeader);
    restartReceiveCycle();
  }

  MessageHeader hdr(_buffer);

  if (!hdr.valid()) {
    LOG(WARNING) << "!!! Received invalid header...";
    restartReceiveCycle();
    return;
  }

  LOG(DEBUG) << "Got message ID " << static_cast<int>(hdr.msgId()) << " ("
             << MessageTypeToString(hdr.msgId()) << ")";

  // hdr.dump();

  const auto packetSize = hdr.packetSize();
  readUpTo(packetSize, boost::bind(&DataRx::rxPacket, this, _1, _2));
}

void DataRx::rxPacket(const boost::system::error_code& ec,
                      std::size_t bytes_transferred) {
  MessageHeader hdr(_buffer);

  if (ec) {
    LOG(WARNING) << "Error on receive of packet data: " << ec.message();
    goto exit;
  }

  if (bytes_transferred < hdr.payloadSize()) {
    LOG(WARNING) << "Received short header of " << bytes_transferred
                 << ", expected " << hdr.payloadSize();
    goto exit;
  }

  if (hdr.msgId() == messageSimplePingResult) {
    if ((hdr.msgVersion() == 1) || (hdr.msgVersion() == 0)) {
      SimplePingResultV1 ping(_buffer);

      if (ping.valid()) {
        if (bytes_transferred < ping.payloadSize()) {
          LOG(WARNING) << "Did not read full data packet, resetting...";
          goto exit;
        }

        callback(ping);
      } else {
        LOG(WARNING) << "Incoming packet invalid";
      }
    } else if (hdr.msgVersion() == 2) {
      SimplePingResultV2 ping(_buffer);

      if (ping.valid()) {
        if (bytes_transferred < ping.payloadSize()) {
          LOG(WARNING) << "Did not read full data packet, resetting...";
          goto exit;
        }

        callback(ping);
      } else {
        LOG(WARNING) << "Incoming packet invalid";
      }
    } else {
      LOG(WARNING) << "Unknown message version " << hdr.msgVersion()
                   << " ignoring";
    }

  } else if (hdr.msgId() == messageLogs) {
    LOG(DEBUG) << "Received " << bytes_transferred << " of LogMessage data";
    LOG(INFO) << std::string(_buffer->begin() + sizeof(OculusMessageHeader),
                             _buffer->end());
  } else {
    LOG(WARNING) << "Ignoring " << MessageTypeToString(hdr.msgId())
                 << " message, id " << static_cast<int>(hdr.msgId());
  }

exit:
  restartReceiveCycle();
}

}  // namespace liboculus
