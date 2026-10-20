#include <iostream>
#include <cstdint>

#pragma pack(push, 1) // Prevent byte padding for network transfer
struct PacketHeader {
    uint16_t clientID;    // Identifies sender in the room
    uint32_t sequenceNo;  // Orders/detects dropped frames
    uint16_t roomID;      // Routes packet to correct group
};
#pragma pack(pop)

// Structure combining header + audio payload
struct VoicePacket {
    PacketHeader header;
    char audioData[960];  // PCM or Opus frame payload (e.g., 20ms of 48kHz audio)
};