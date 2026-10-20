#include <iostream>
#include <vector>
#include <map>
#include <cstdint>   // Required for uint16_t and uint32_t
#include <cstring>   // Required for memset / memcpy
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#pragma pack(push, 1) // Prevent byte padding for network transfer
struct PacketHeader {
    uint16_t clientID;   // Identifies sender in the room
    uint32_t sequenceNo; // Orders/detects dropped frames
    uint16_t roomID;     // Routes packet to correct group
};
#pragma pack(pop)

// Helper operator to compare sockaddr_in for client registry lookup
bool operator==(const sockaddr_in& a, const sockaddr_in& b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "Failed to initialize Winsock.\n";
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create server socket.\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9000); // Server port
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind server socket to port 9000.\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "[SERVER] Central UDP Voice Server active on port 9000...\n";

    // Room Manager: Maps RoomID -> List of registered Client Sockets (IP & Port)
    std::map<uint16_t, std::vector<sockaddr_in>> roomRegistry;

    char buffer[1500]; // Fits standard Ethernet MTU (~1500 bytes)
    sockaddr_in clientAddr;
    int clientAddrLen = sizeof(clientAddr);

    while (true) {
        int bytesReceived = recvfrom(serverSocket, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&clientAddr, &clientAddrLen);

        if (bytesReceived >= static_cast<int>(sizeof(PacketHeader))) {
            PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);

            uint16_t room = header->roomID;
            uint16_t senderID = header->clientID;

            // 1. Client Registry Lookup / Auto-Registration
            auto& members = roomRegistry[room];
            bool exists = false;
            for (const auto& member : members) {
                if (member == clientAddr) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                members.push_back(clientAddr);
                std::cout << "[SERVER] Registered Client " << senderID 
                          << " into Room " << room << "\n";
            }

            // 2. Broadcast / Relay Engine: Forward packet to all OTHER room members
            for (const auto& member : members) {
                if (!(member == clientAddr)) { // Do not echo back to sender
                    sendto(serverSocket, buffer, bytesReceived, 0,
                           (struct sockaddr*)&member, sizeof(member));
                }
            }
        }
    }

    closesocket(serverSocket);
    WSACleanup();
    return 0;
}