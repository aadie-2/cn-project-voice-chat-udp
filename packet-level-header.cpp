#include <iostream>
#include <cstdint>

#pragma pack(push, 1) 
struct PacketHeader {
    uint16_t clientID;    
    uint32_t sequenceNo;  
    uint16_t roomID;     
};
#pragma pack(pop)

struct VoicePacket {
    PacketHeader header;
    char audioData[960];  
};
