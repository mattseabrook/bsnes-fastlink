#if defined(PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "../bsnes.hpp"

#if defined(PLATFORM_WINDOWS)
#include <algorithm>
#include <cstring>
#include <sfc/sfc.hpp>

namespace {
using SocketHandle = SOCKET;

constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
// A snapshot can only become newer at an emulation-frame boundary.  A short
// timeout lets an unpaced client spin on stale snapshots, churning sockets and
// sending 128 KiB hundreds of times per second.  Active requests are woken by
// the next published emulation frame; the longer fallback only matters while
// paused or stalled, where it caps stale traffic at 10 responses per second
// without ever blocking the emulation thread.
constexpr uint32_t RequestWaitMilliseconds = 100;
constexpr char HTTPHeader[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-Type: application/octet-stream\r\n"
  "Content-Length: 131072\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Access-Control-Allow-Private-Network: true\r\n"
  "\r\n";
constexpr char HTTPOptionsHeader[] =
  "HTTP/1.1 204 No Content\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Access-Control-Allow-Private-Network: true\r\n"
  "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
  "Access-Control-Allow-Headers: Content-Type\r\n"
  "\r\n";

auto closeSocket(SocketHandle socket) -> void {
  if(socket != InvalidSocket) closesocket(socket);
}

auto setSocketTimeouts(SocketHandle socket) -> void {
  DWORD timeout = 150;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

  int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

auto sendAll(SocketHandle socket, const char* data, uint32_t size) -> bool {
  while(size) {
    int sent = send(socket, data, (int)std::min<uint32_t>(size, 64 * 1024), 0);
    if(sent <= 0) return false;
    data += sent;
    size -= sent;
  }
  return true;
}

auto receiveRequest(SocketHandle socket, char* request, uint32_t capacity) -> uint32_t {
  uint32_t used = 0;
  while(used + 1 < capacity) {
    int received = recv(socket, request + used, (int)(capacity - used - 1), 0);
    if(received <= 0) break;
    used += received;
    request[used] = 0;
    if(used >= 4 && std::strstr(request, "\r\n\r\n")) break;
  }
  return used;
}

}
#endif

FastLinkRAMTraceService fastLinkRAMTraceService;

FastLinkRAMTraceService::FastLinkRAMTraceService() {
  for(auto& readers : _readers) readers.store(0, std::memory_order_relaxed);
}

FastLinkRAMTraceService::~FastLinkRAMTraceService() {
  stop();
}

auto FastLinkRAMTraceService::start() -> void {
#if defined(PLATFORM_WINDOWS)
  bool expected = false;
  if(!_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    fastLinkDebug("RAMTrace::start ignored: already running");
    return;
  }

  fastLinkDebug("RAMTrace::start port=%u", Port);
  // A failed startup leaves a finished but joinable thread. Reap it before
  // replacing std::thread, whose assignment would otherwise terminate us.
  if(_thread.joinable()) _thread.join();
  clearSnapshot();
  _thread = std::thread([this] { threadMain(); });
#endif
}

auto FastLinkRAMTraceService::stop() -> void {
#if defined(PLATFORM_WINDOWS)
  fastLinkDebug("RAMTrace::stop begin running=%d", _running.load(std::memory_order_acquire));
  _running.store(false, std::memory_order_release);
  _snapshotRequested.store(false, std::memory_order_release);
  _condition.notify_all();

  if(_thread.joinable()) _thread.join();
  fastLinkDebug("RAMTrace::stop done");
#endif
}

auto FastLinkRAMTraceService::clearSnapshot() -> void {
#if defined(PLATFORM_WINDOWS)
  fastLinkDebug("RAMTrace::clearSnapshot");
  static const std::array<uint8_t, WRAMSize> empty = {};
  publish(empty.data(), WRAMSize);
#endif
}

auto FastLinkRAMTraceService::publishFrame() -> void {
#if defined(PLATFORM_WINDOWS)
  if(!_running.load(std::memory_order_relaxed)) return;
  if(!_snapshotRequested.load(std::memory_order_relaxed)) return;
  if(!_snapshotRequested.exchange(false, std::memory_order_acquire)) return;
  static uint publishTraceCount = 0;
  bool tracePublish = publishTraceCount++ < 40;
  if(tracePublish) fastLinkDebug("RAMTrace::publishFrame requested loaded=%d", emulator && emulator->loaded());

  if(!emulator || !emulator->loaded()) {
    clearSnapshot();
    return;
  }

  publish(SuperFamicom::cpu.wram, WRAMSize);
#endif
}

auto FastLinkRAMTraceService::requestSnapshot(uint64_t previousSequence) -> void {
#if defined(PLATFORM_WINDOWS)
  _snapshotRequested.store(true, std::memory_order_release);

  std::unique_lock<std::mutex> lock(_conditionMutex);
  _condition.wait_for(lock, std::chrono::milliseconds(RequestWaitMilliseconds), [&] {
    return !_running.load(std::memory_order_acquire)
        || _sequence.load(std::memory_order_acquire) != previousSequence;
  });
#endif
}

auto FastLinkRAMTraceService::publish(const uint8_t* data, uint32_t size) -> void {
#if defined(PLATFORM_WINDOWS)
  if(!data) return;
  if(size > WRAMSize) size = WRAMSize;

  auto activeSlot = _activeSlot.load(std::memory_order_acquire);
  uint32_t targetSlot = SnapshotSlots;

  for(uint32_t offset = 1; offset <= SnapshotSlots; offset++) {
    uint32_t candidate = (activeSlot + offset) % SnapshotSlots;
    if(candidate == activeSlot) continue;
    if(_readers[candidate].load(std::memory_order_acquire) != 0) continue;
    targetSlot = candidate;
    break;
  }

  if(targetSlot == SnapshotSlots) return;

  std::memcpy(_snapshots[targetSlot].data(), data, size);
  if(size < WRAMSize) {
    std::memset(_snapshots[targetSlot].data() + size, 0, WRAMSize - size);
  }

  _publishedSize.store(WRAMSize, std::memory_order_release);
  _activeSlot.store(targetSlot, std::memory_order_release);
  _sequence.fetch_add(1, std::memory_order_release);
  _condition.notify_all();
#endif
}

auto FastLinkRAMTraceService::copyActiveSnapshot(uint8_t* output, uint32_t capacity) -> uint32_t {
#if defined(PLATFORM_WINDOWS)
  if(!output || capacity == 0) return 0;
  uint32_t size = std::min<uint32_t>(_publishedSize.load(std::memory_order_acquire), capacity);

  while(_running.load(std::memory_order_acquire)) {
    uint32_t slot = _activeSlot.load(std::memory_order_acquire);
    _readers[slot].fetch_add(1, std::memory_order_acq_rel);

    if(slot == _activeSlot.load(std::memory_order_acquire)) {
      std::memcpy(output, _snapshots[slot].data(), size);
      _readers[slot].fetch_sub(1, std::memory_order_acq_rel);
      return size;
    }

    _readers[slot].fetch_sub(1, std::memory_order_acq_rel);
  }
#endif
  return 0;
}

auto FastLinkRAMTraceService::threadMain() -> void {
#if defined(PLATFORM_WINDOWS)
  fastLinkDebug("RAMTrace::thread begin");
  // The process runs in HIGH_PRIORITY_CLASS.  BELOW_NORMAL would still give
  // this ancillary HTTP worker a base priority above ordinary applications;
  // IDLE guarantees that emulation, PPU, audio, and driver work wins.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
  SetThreadPriorityBoost(GetCurrentThread(), FALSE);

  WSADATA wsaData = {};
  if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    fastLinkDebug("RAMTrace::thread WSAStartup failed");
    _running.store(false, std::memory_order_release);
    return;
  }

  SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(listenSocket == InvalidSocket) {
    fastLinkDebug("RAMTrace::thread socket failed error=%d", WSAGetLastError());
    WSACleanup();
    _running.store(false, std::memory_order_release);
    return;
  }

  int one = 1;
  setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((u_short)Port);

  if(bind(listenSocket, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR
  || listen(listenSocket, 8) == SOCKET_ERROR) {
    fastLinkDebug("RAMTrace::thread bind/listen failed port=%u error=%d", Port, WSAGetLastError());
    closeSocket(listenSocket);
    WSACleanup();
    _running.store(false, std::memory_order_release);
    return;
  }
  fastLinkDebug("RAMTrace::thread listening port=%u", Port);

  std::array<uint8_t, WRAMSize> sendBuffer = {};
  std::array<char, 1024> request = {};
  uint requestTraceCount = 0;

  while(_running.load(std::memory_order_acquire)) {
    fd_set readSet = {};
    readSet.fd_count = 1;
    readSet.fd_array[0] = listenSocket;

    timeval timeout = {};
    timeout.tv_usec = 50 * 1000;
    int selected = select(0, &readSet, nullptr, nullptr, &timeout);
    if(selected == SOCKET_ERROR) break;
    if(selected == 0) continue;

    SocketHandle client = accept(listenSocket, nullptr, nullptr);
    if(client == InvalidSocket) continue;
    setSocketTimeouts(client);

    request.fill(0);
    auto requestSize = receiveRequest(client, request.data(), (uint32_t)request.size());
    if(!requestSize) {
      closeSocket(client);
      continue;
    }
    bool isOptions = requestSize >= 7 && std::memcmp(request.data(), "OPTIONS", 7) == 0;
    bool traceRequest = requestTraceCount++ < 40;
    if(traceRequest) fastLinkDebug("RAMTrace::thread request size=%u options=%d", requestSize, isOptions);

    if(isOptions) {
      sendAll(client, HTTPOptionsHeader, sizeof(HTTPOptionsHeader) - 1);
    } else {
      auto sequence = _sequence.load(std::memory_order_acquire);
      requestSnapshot(sequence);
      copyActiveSnapshot(sendBuffer.data(), (uint32_t)sendBuffer.size());
      sendAll(client, HTTPHeader, sizeof(HTTPHeader) - 1);
      sendAll(client, (const char*)sendBuffer.data(), (uint32_t)sendBuffer.size());
      if(traceRequest) fastLinkDebug("RAMTrace::thread served snapshot bytes=%u", (uint)sendBuffer.size());
    }

    shutdown(client, SD_SEND);
    closeSocket(client);
  }

  closeSocket(listenSocket);
  WSACleanup();
  _running.store(false, std::memory_order_release);
  _condition.notify_all();
  fastLinkDebug("RAMTrace::thread exit");
#endif
}
