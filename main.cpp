#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
using AddressLength = int;
constexpr Socket INVALID_UDP_SOCKET = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using Socket = int;
using AddressLength = socklen_t;
constexpr Socket INVALID_UDP_SOCKET = 1;
#endif

void close_socket(Socket sock) {
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
}

namespace fs = std::filesystem;
using Byte = std::uint8_t;

constexpr std::size_t BLOCK_SIZE = 512;
constexpr int RETRIES = 5;
constexpr int PACKET_CAPACITY = 2048;

enum OpCode : std::uint16_t { RRQ = 1, WRQ = 2, DATA = 3, ACK = 4, ERROR = 5 };

std::uint16_t read_u16(const Byte *buffer) {
  return (static_cast<std::uint16_t>(buffer[0]) << 8) | buffer[1];
}

void add_u16(std::vector<Byte> &buffer, std::uint16_t value) {
  buffer.push_back(static_cast<Byte>(value >> 8));
  buffer.push_back(static_cast<Byte>(value & 0xFF));
}

std::vector<Byte> make_ack(std::uint16_t block) {
  std::vector<Byte> packet;
  add_u16(packet, ACK);
  add_u16(packet, block);
  return packet;
}

std::vector<Byte> make_data(std::uint16_t block, const char *bytes,
                            std::size_t count) {
  std::vector<Byte> packet;
  add_u16(packet, DATA);
  add_u16(packet, block);
  for (std::uint16_t i = 0; i < count; ++i) {
    packet.push_back(static_cast<Byte>(bytes[i]));
  }
  return packet;
}

bool send_packet(Socket s, const sockaddr_in &peer,
                 const std::vector<Byte> &packet) {
  int sent = sendto(s, reinterpret_cast<const char *>(packet.data()),
                    static_cast<int>(packet.size()), 0,
                    reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));

  return sent == static_cast<int>(packet.size());
}

int receive_packet(Socket s, std::array<Byte, PACKET_CAPACITY> &buffer,
                   sockaddr_in &peer) {
  AddressLength length = sizeof(peer);

  return recvfrom(s, reinterpret_cast<char *>(buffer.data()),
                  static_cast<int>(buffer.size()), 0,
                  reinterpret_cast<sockaddr *>(&peer), &length);
}

bool same_peer(const sockaddr_in &a, const sockaddr_in &b) {
  return a.sin_family == b.sin_family &&
         a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void send_error(Socket s, const sockaddr_in &peer,
                const std::uint16_t error_code, const std::string &message) {
  std::vector<Byte> packet;
  add_u16(packet, ERROR);
  add_u16(packet, error_code);
  packet.insert(packet.end(), message.begin(), message.end());
  packet.push_back(0);
  send_packet(s, peer, packet);
}

void set_one_second_timeout(Socket s) {
#ifdef _WIN32
  DWORD timeout_ms = 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
  timeval timeout{1, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

Socket open_udp_socket(std::uint16_t port, bool timed) {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_UDP_SOCKET)
    return s;

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(s, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == -1) {
    close_socket(s);
    return INVALID_UDP_SOCKET;
  }

  if (timed)
    set_one_second_timeout(s);
  return s;
}

bool safe_name(const std::string &name) {
  if (name.empty() || name.size() > 100 || name == "." || name == "..") {
    return false;
  }
  for (unsigned char ch : name) {
    if (std::isalnum(ch) && ch != '.' && ch != '_' && ch != '-') {
      return false;
    }
  }
}

struct Request {
  std::uint16_t opcode = 0;
  std::string filename;
  std::string mode;
};

bool parse_request(const Byte *bytes, int count, Request &req) {
  if (count < 5) {
    return false;
  }
  req.opcode = read_u16(bytes);
  if (req.opcode != RRQ && req.opcode != WRQ) {
    return false;
  }

  const Byte *end = bytes + count;
  const Byte *filename_end = std::find(bytes + 2, end, Byte{0});

  if (filename_end == end || filename_end + 1 == end)
    return false;
  const Byte *mode_start = filename_end + 1;
  const Byte *mode_end = std::find(mode_start, end, Byte{0});
  if (mode_end == end || mode_start == mode_end)
    return false;

  req.filename.assign(reinterpret_cast<const char *>(bytes + 2),
                      reinterpret_cast<const char *>(filename_end));
  req.mode.assign(reinterpret_cast<const char *>(mode_start),
                  reinterpret_cast<const char *>(mode_end));

  for (char &c : req.mode)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  return true;
}

bool send_and_wait_for_ack(Socket s, const sockaddr_in &client,
                           const std::vector<Byte> &data,
                           std::uint16_t expected_block) {
  std::array<Byte, PACKET_CAPACITY> buffer{};

  for (int attempt = 0; attempt < RETRIES; ++attempt) {
    if (!send_packet(s, client, data))
      return false;
    for (int ignored = 0; ignored < 100; ++ignored) {
      sockaddr_in from{};
      int n = receive_packet(s, buffer, from);
      if (n < 0)
        break;
      if (!same_peer(from, client)) {
        send_error(s, from, 5, "Unknown tranfer ID");
        continue;
      }
      if (n >= 4 && read_u16(buffer.data()) == ACK &&
          read_u16(buffer.data() + 2) == expected_block)
        return true;
      if (n >= 2 && read_u16(buffer.data()) == ERROR)
        return false;
    }
  }
}

void handle_rrq(Socket s, const sockaddr_in &client, const fs::path &root,
                const std::string &filename) {
  std::ifstream file(root / filename, std::ios::binary);
  if (!file) {
    send_error(s, client, 0, "File not found or not readable");
    return;
  }

  std::array<char, BLOCK_SIZE> chunk{};
  std::uint16_t block = 1;
  while (true) {
    file.read(chunk.data(), chunk.size());
    std::streamsize count = file.gcount();
    if (file.bad()) {
      send_error(s, client, 0, "File read error");
      return;
    }
    auto packet =
        make_data(block, chunk.data(), static_cast<std::size_t>(count));

    if (!send_and_wait_for_ack(s, client, packet, block)) {
      std::cerr << "RRQ aborted (no ACK): " << filename << '\n';
      return;
    }

    if (count < static_cast<std::streamsize>(BLOCK_SIZE))
      break;
    ++block;
  }
}

void acknowledge_final_duplicates(Socket s, const sockaddr_in &client,
                                  std::uint16_t final_block) {
  std::array<Byte, PACKET_CAPACITY> buffer{};
  for (int i = 0; i < 2; i++) {
    sockaddr_in from{};
    int n = receive_packet(s, buffer, from);
    if (n < 0)
      continue;
    if (!same_peer(from, client)) {
      send_error(s, from, 5, "Unknown transfer ID");
      continue;
    }
    if (n >= 4 && read_u16(buffer.data()) == DATA &&
        read_u16(buffer.data() + 2) == final_block)
      send_packet(s, client, make_ack(final_block));
  }
}

void handle_wrq(Socket s, const sockaddr_in &client, const fs::path &root,
                const std::string &filename) {
  fs::path destination = root / filename;
  if (fs::exists(destination)) {
    send_error(s, client, 6, "File already exists");
    return;
  }
  std::ofstream file(destination, std::ios::binary | std::ios::trunc);
  if (!file) {
    send_error(s, client, 2, "Cannot create file");
    return;
  }

  std::uint16_t expected = 1;
  auto last_ack = make_ack(0);
  bool success = false;
  if (!send_packet(s, client, last_ack)) {
    file.close();
    fs::remove(destination);
    return;
  }

  std::array<Byte, PACKET_CAPACITY> buffer{};
  int timeouts = 0;
  while (timeouts < RETRIES) {
    sockaddr_in from{};
    int n = receive_packet(s, buffer, from);
    if (n < 0) {
      ++timeouts;
      send_packet(s, client, last_ack);
      continue;
    }
    if (!same_peer(from, client)) {
      send_error(s, client, 5, "Unknown transfer ID");
      continue;
    }
    if (n >= 2 && read_u16(buffer.data()) == ERROR)
      break;
    if (n < 4 || n > static_cast<int>(BLOCK_SIZE * 4) ||
        read_u16(buffer.data()) != DATA) {
      send_error(s, client, 4, "Expected DATA packet");
      break;
    }

    std::uint16_t received_block = read_u16(buffer.data() + 2);
    if (received_block == expected) {
      int payload_size = n - 4;
      file.write(reinterpret_cast<char *>(buffer.data() + 4), payload_size);
      if (!file) {
        send_error(s, client, 3, "Disk wrate failed");
        break;
      }
      last_ack = make_ack(received_block);
      if (!send_packet(s, client, last_ack))
        break;
      timeouts = 0;
      ++expected;

      if (payload_size < static_cast<int>(BLOCK_SIZE)) {
        file.close();
        if (!file) {
          send_error(s, client, 3, "File close failed");
          break;
        }
        success = true;
        acknowledge_final_duplicates(s, client, received_block);
        break;
      } else if (received_block == static_cast<std::uint16_t>(expected - 1)) {
        send_packet(s, client, last_ack);
      }
    }
  }

  if (!success) {
    file.close();
    std::error_code ignored;
    fs::remove(destination, ignored);
    std::cerr << "WRQ aborted: " << filename << std::endl;
  } else {
    std::cout << "Upload complete: " << filename << std::endl;
  }
}

int main(int argc, char *argv[]) {
  fs::path root = (argc > 1) ? argv[1] : fs::current_path();
  int port = 6969;
  try {
    if (argc > 2)
      port = argv[2] ? std::stoi(argv[2]) : port;
    if (port < 1 || port > 65535)
      throw std::invalid_argument("Port number must be between 1 and 65535");
    fs::create_directories(root);
    root = fs::absolute(root);
  } catch (const std::exception &e) {
    std::cerr << "Startup error: " << e.what() << std::endl;
    return 1;
  }
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    std::cerr << "WSAStartup failed" << std::endl;
    return 1;
  }
#endif

  Socket listener = open_udp_socket(static_cast<std::uint16_t>(port), false);
  if (listener == INVALID_UDP_SOCKET) {
    std::cerr << "Failed to open UDP socket on port " << port << std::endl;
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  std::cout << "TFTP server listening on port " << port
            << ", root directory: " << root << std::endl;
  std::array<Byte, PACKET_CAPACITY> buffer{};

  while (true) {
    sockaddr_in client{};
    int n = receive_packet(listener, buffer, client);
    if (n < 0)
      continue;

    Request request;
    if (!parse_request(buffer.data(), n, request)) {
      send_error(listener, client, 4, "Bar TFTP request");
      continue;
    }
    if (!safe_name(request.filename)) {
      send_error(listener, client, 4, "Invalid filename");
      continue;
    }
    if (request.mode != "octet") {
      send_error(listener, client, 4, "Only octet mode is supported");
      continue;
    }
    Socket transfer = open_udp_socket(0, true);
    if (transfer == INVALID_UDP_SOCKET) {
      send_error(listener, client, 0, "Cannot create trnafer socket");
      continue;
    }

    std::cout << ((request.opcode == RRQ) ? "RPQ" : "WRQ") << request.filename
              << std::endl;
    if (request.opcode == RRQ) {
      handle_rrq(listener, client, root, request.filename);
    } else {
      handle_wrq(listener, client, root, request.filename);
    }
    close_socket(transfer);
  }

  close_socket(listener);

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
