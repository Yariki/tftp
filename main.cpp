#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <filesystem>
#include <array>
#include <fstream>
#include <algorithm>


#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using Socket = SOCKET;
    using AddressLength = int;
    constexpr Socket INVALID_UDP_SOCKET = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <netinet/in.h>

    using Socket = int;
    using AddressLength = socklen_t;
    constexpr Socket INVALID_UDP_SOCKET = 1;
#endif

#ifdef _WIN32
    WSDATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "Failed to initialize Winsock" << std::endl;
        return 1;
    }

#endif

#ifdef  _WIN32
    WSACleanup();
#endif

void close_socket(Socket sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

namespace fs = std::filesystem;
using  Byte = std::uint8_t;

constexpr std::size_t BLOCK_SIZE = 512;
constexpr int RETRIES = 5;
constexpr int PACKET_CAPACITY = 2048;

enum OpCode : std::uint16_t {
    RRQ = 1,
    WRQ = 2,
    DATA = 3,
    ACK = 4,
    ERROR = 5
};

std::uint16_t read_u16(const Byte* buffer)
{
    return (static_cast<std::uint16_t>(buffer[0]) << 8) | buffer[1];
}

void add_u16(std::vector<Byte>&  buffer, std::uint16_t value)
{
    buffer.push_back(static_cast<Byte>(value >> 8));
    buffer.push_back(static_cast<Byte>(value & 0xFF));
}

std::vector<Byte> make_ack(std::uint16_t block, const char* bytes, std::size_t count)
{
    std::vector<Byte> packet;
    add_u16(packet, ACK);
    add_u16(packet, block);
    for (std::size_t i = 0; i < count; ++i)
    {
        packet.push_back(static_cast<Byte>(bytes[i]));
    }
    return packet;
}

std::vector<Byte> make_data(std::uint16_t block, const char* bytes, std::size_t count)
{
    std::vector<Byte> packet;
    add_u16(packet, DATA);
    add_u16(packet, block);
    for(std::uint16_t i = 0; i < count; ++i)
    {
        packet.push_back(static_cast<Byte>(bytes[i]));
    }
    return packet;
}

bool send_packet(Socket s, const sockaddr_in& peer, const std::vector<Byte>& packet)
{
    int sent = sendto(s, 
                        reinterpret_cast<const char*>(packet.data()),
                        static_cast<int>(packet.size()), 
                        0,
                        reinterpret_cast<const sockaddr*>(&peer), 
                        sizeof(peer));

    return sent == static_cast<int>(packet.size());
}

int receive_packet(Socket s, std::array<Byte, PACKET_CAPACITY>& buffer, sockaddr_in& peer)
{
    AddressLength length = sizeof(peer);
    
    return recvfrom(s, 
                    reinterpret_cast<char*>(buffer.data()), 
                    static_cast<int>(buffer.size()),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    &length);
}

bool same_peer(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_family == b.sin_family 
            && a.sin_addr.s_addr == b.sin_addr.s_addr 
            && a.sin_port == b.sin_port;
}

void send_error(Socket s, 
    const sockaddr_in& peer,
    const std::uint16_t error_code, 
    const std::string& message)
{
    std::vector<Byte> packet;
    add_u16(packet, ERROR);
    add_u16(packet, error_code);
    packet.insert(packet.end(), message.begin(), message.end());
    packet.push_back(0);
    send_packet(s, peer, packet);
}

void set_one_second_timeout(Socket s)
{
#ifdef _WIN32
    DWORD timeout_ms = 1000;
    setsockopt(s, 
        SOL_SOCKET, 
        SO_RCVTIMEO, 
        reinterpret_cast<const char*>(&timeout_ms), 
        sizeof(timeout_ms));
#else
    timeval timeout{1,0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

Socket open_udp_socket(std::uint16_t port, bool timed)
{
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(s == INVALID_UDP_SOCKET) return s;    

    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(bind(
            s,
            reinterpret_cast<sockaddr*>(&local), 
            sizeof(local)
        ) == -1)
    {
        close_socket(s);
        return INVALID_UDP_SOCKET;
    }

    if(timed) set_one_second_timeout(s);
    return s;
}

bool safe_name(const std::string& name)
{
    if(name.empty() || name.size() > 100 || name == "." || name == "..")
    {
        return false;
    }
    for(unsigned char ch : name)
    {
        if(std::isalnum(ch) &&  ch != '.' && ch != '_' && ch !=  '-')
        {
            return false;
        }
    }
}

struct Request {
    std::uint16_t opcode = 0;
    std::string filename;
    std::string mode;
};

bool parse_request(const Byte* bytes, int count, Request& req)
{
    if(count < 5) 
    {
        return  false;
    }
    req.opcode = read_u16(bytes);
    if(req.opcode != RRQ && req.opcode != WRQ)
    {
        return false;
    }

    const Byte* end = bytes + count;
    const Byte* filename_end = std::find(bytes + 2, end, Byte{0});

    if (filename_end == end || filename_end + 1 == end) 
        return false;
    const Byte* mode_start  = filename_end + 1;
    const Byte* mode_end = std::find(mode_start, end, Byte{0});
    if(mode_end == end || mode_start == mode_end) 
        return false;
    
    req.filename.assign(reinterpret_cast<const char*>(bytes + 2), reinterpret_cast<const char*>(filename_end));
    req.mode.assign(reinterpret_cast<const char*>(mode_start), reinterpret_cast<const char*>(mode_end));

    for(char& c: req.mode)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return true;
}

int main ()
{
    Socket server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    sockaddr_in  address {};

    address.sin_family = AF_INET;
    address.sin_port = htons(69);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(
        bind(
            server_socket, 
            reinterpret_cast<sockaddr*>(&address), 
            sizeof(address)
        ) == -1)
    {
        std::cerr << "Failed to bind socket" << std::endl;
        return 1;
    }




}