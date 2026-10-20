#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <string>
#include <cmath>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t clientID;
    uint32_t sequenceNo;
    uint16_t roomID;
};
#pragma pack(pop)

const int SAMPLE_RATE = 16000;
const int BITS_PER_SAMPLE = 16;
const int CHANNELS = 1;
const int FRAME_DURATION_MS = 20;
const int AUDIO_BUFFER_SIZE = (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8) * FRAME_DURATION_MS) / 1000; 

#define IDC_EDIT_CLIENT_ID   101
#define IDC_EDIT_ROOM_ID     102
#define IDC_EDIT_SERVER_IP   103
#define IDC_EDIT_SERVER_PORT 104
#define IDC_COMBO_MIC        105
#define IDC_COMBO_SPEAKER    106
#define IDC_BTN_CONNECT      107
#define IDC_BTN_PTT          108
#define IDC_STATUS_LABEL     109
#define IDC_STATS_LABEL      110

HWAVEOUT hWaveOut = NULL;
HWAVEIN hWaveIn = NULL;
SOCKET clientSocket = INVALID_SOCKET;
sockaddr_in serverAddr{};

std::atomic<bool> isConnected(false);
std::atomic<bool> isTransmitting(false);
std::atomic<uint32_t> packetsSent(0);
std::atomic<uint32_t> packetsRecv(0);
std::atomic<int> audioVolumeLevel(0); 

uint16_t g_clientID = 101;
uint16_t g_roomID = 1;
uint32_t g_seqNum = 0;

HWND hMainWnd;
HWND hStatusLabel, hStatsLabel, hBtnPTT, hBtnConnect;
HWND hEditClientID, hEditRoomID, hEditIP, hEditPort;
HWND hComboMic, hComboSpeaker;

HBRUSH hBgBrush, hPanelBrush, hGreenBrush, hDarkGreenBrush;

int calculateAudioRMS(const char* pcmData, int size) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(pcmData);
    int count = size / 2;
    double sum = 0;
    for (int i = 0; i < count; i++) {
        sum += samples[i] * samples[i];
    }
    double rms = std::sqrt(sum / count);
    int level = static_cast<int>((rms / 32768.0) * 100.0 * 3.0); 
    return (level > 100) ? 100 : level;
}

bool initSpeaker(UINT deviceID) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = BITS_PER_SAMPLE;
    wfx.nBlockAlign = (CHANNELS * BITS_PER_SAMPLE) / 8;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * wfx.nBlockAlign;

    return (waveOutOpen(&hWaveOut, deviceID, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR);
}

bool initMicrophone(UINT deviceID) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = BITS_PER_SAMPLE;
    wfx.nBlockAlign = (CHANNELS * BITS_PER_SAMPLE) / 8;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * wfx.nBlockAlign;

    return (waveInOpen(&hWaveIn, deviceID, &wfx, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR);
}

void playAudioChunk(char* pcmData, int dataSize) {
    if (!hWaveOut) return;
    WAVEHDR waveHeader{};
    waveHeader.lpData = pcmData;
    waveHeader.dwBufferLength = dataSize;

    waveOutPrepareHeader(hWaveOut, &waveHeader, sizeof(WAVEHDR));
    waveOutWrite(hWaveOut, &waveHeader, sizeof(WAVEHDR));

    while (!(waveHeader.dwFlags & WHDR_DONE)) {
        Sleep(1);
    }
    waveOutUnprepareHeader(hWaveOut, &waveHeader, sizeof(WAVEHDR));
}

void receiveLoop() {
    std::vector<char> buffer(sizeof(PacketHeader) + AUDIO_BUFFER_SIZE);
    sockaddr_in fromAddr;
    int fromLen = sizeof(fromAddr);

    while (isConnected) {
        int bytesRecv = recvfrom(clientSocket, buffer.data(), static_cast<int>(buffer.size()), 0,
                                 (struct sockaddr*)&fromAddr, &fromLen);

        if (bytesRecv > static_cast<int>(sizeof(PacketHeader))) {
            packetsRecv++;
            PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer.data());
            char* audioPayload = buffer.data() + sizeof(PacketHeader);
            int payloadSize = bytesRecv - sizeof(PacketHeader);

            audioVolumeLevel = calculateAudioRMS(audioPayload, payloadSize);

            playAudioChunk(audioPayload, payloadSize);
        }
    }
}

void populateAudioDevices() {
    SendMessageA(hComboMic, CB_ADDSTRING, 0, (LPARAM)"0: Default Microphone (WAVE_MAPPER)");
    UINT numMics = waveInGetNumDevs();
    for (UINT i = 0; i < numMics; i++) {
        WAVEINCAPS caps;
        if (waveInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::string label = std::to_string(i + 1) + ": " + caps.szPname;
            SendMessageA(hComboMic, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
    }
    SendMessageA(hComboMic, CB_SETCURSEL, 0, 0);

    SendMessageA(hComboSpeaker, CB_ADDSTRING, 0, (LPARAM)"0: Default Speaker (WAVE_MAPPER)");
    UINT numSpeakers = waveOutGetNumDevs();
    for (UINT i = 0; i < numSpeakers; i++) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::string label = std::to_string(i + 1) + ": " + caps.szPname;
            SendMessageA(hComboSpeaker, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        }
    }
    SendMessageA(hComboSpeaker, CB_SETCURSEL, 0, 0);
}

void transmitAudioFrame() {
    if (!isConnected || !hWaveIn) return;

    std::vector<char> packetBuffer(sizeof(PacketHeader) + AUDIO_BUFFER_SIZE);
    std::vector<char> micAudioBuffer(AUDIO_BUFFER_SIZE);

    WAVEHDR micHeader{};
    micHeader.lpData = micAudioBuffer.data();
    micHeader.dwBufferLength = AUDIO_BUFFER_SIZE;

    waveInPrepareHeader(hWaveIn, &micHeader, sizeof(WAVEHDR));
    waveInAddBuffer(hWaveIn, &micHeader, sizeof(WAVEHDR));
    waveInStart(hWaveIn);

    while (!(micHeader.dwFlags & WHDR_DONE)) {
        Sleep(1);
    }

    waveInStop(hWaveIn);
    waveInUnprepareHeader(hWaveIn, &micHeader, sizeof(WAVEHDR));

    audioVolumeLevel = calculateAudioRMS(micAudioBuffer.data(), AUDIO_BUFFER_SIZE);

    PacketHeader header = { g_clientID, g_seqNum++, g_roomID };
    std::memcpy(packetBuffer.data(), &header, sizeof(PacketHeader));
    std::memcpy(packetBuffer.data() + sizeof(PacketHeader), micAudioBuffer.data(), AUDIO_BUFFER_SIZE);

    sendto(clientSocket, packetBuffer.data(), static_cast<int>(packetBuffer.size()), 0,
           (struct sockaddr*)&serverAddr, sizeof(serverAddr));

    packetsSent++;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        hBgBrush = CreateSolidBrush(RGB(30, 30, 46));       
        hPanelBrush = CreateSolidBrush(RGB(45, 45, 68));    
        hGreenBrush = CreateSolidBrush(RGB(80, 250, 123));   
        hDarkGreenBrush = CreateSolidBrush(RGB(40, 100, 60));

        CreateWindowA("STATIC", "Client ID:", WS_VISIBLE | WS_CHILD, 20, 20, 80, 20, hwnd, NULL, NULL, NULL);
        hEditClientID = CreateWindowA("EDIT", "101", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 100, 18, 70, 22, hwnd, (HMENU)IDC_EDIT_CLIENT_ID, NULL, NULL);

        CreateWindowA("STATIC", "Room ID:", WS_VISIBLE | WS_CHILD, 190, 20, 60, 20, hwnd, NULL, NULL, NULL);
        hEditRoomID = CreateWindowA("EDIT", "1", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 260, 18, 70, 22, hwnd, (HMENU)IDC_EDIT_ROOM_ID, NULL, NULL);

        CreateWindowA("STATIC", "Server IP:", WS_VISIBLE | WS_CHILD, 20, 50, 80, 20, hwnd, NULL, NULL, NULL);
        hEditIP = CreateWindowA("EDIT", "127.0.0.1", WS_VISIBLE | WS_CHILD | WS_BORDER, 100, 48, 110, 22, hwnd, (HMENU)IDC_EDIT_SERVER_IP, NULL, NULL);

        CreateWindowA("STATIC", "Port:", WS_VISIBLE | WS_CHILD, 220, 50, 40, 20, hwnd, NULL, NULL, NULL);
        hEditPort = CreateWindowA("EDIT", "9000", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 260, 48, 70, 22, hwnd, (HMENU)IDC_EDIT_SERVER_PORT, NULL, NULL);

        CreateWindowA("STATIC", "Microphone Input:", WS_VISIBLE | WS_CHILD, 20, 80, 150, 20, hwnd, NULL, NULL, NULL);
        hComboMic = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 100, 310, 120, hwnd, (HMENU)IDC_COMBO_MIC, NULL, NULL);

        CreateWindowA("STATIC", "Speaker Output:", WS_VISIBLE | WS_CHILD, 20, 130, 150, 20, hwnd, NULL, NULL, NULL);
        hComboSpeaker = CreateWindowA("COMBOBOX", "", WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 150, 310, 120, hwnd, (HMENU)IDC_COMBO_SPEAKER, NULL, NULL);

        hBtnConnect = CreateWindowA("BUTTON", "CONNECT TO ROOM SERVER", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 185, 310, 32, hwnd, (HMENU)IDC_BTN_CONNECT, NULL, NULL);

        hBtnPTT = CreateWindowA("BUTTON", "HOLD 'T' KEY TO TALK", WS_VISIBLE | WS_CHILD | WS_DISABLED, 20, 225, 310, 45, hwnd, (HMENU)IDC_BTN_PTT, NULL, NULL);

        hStatusLabel = CreateWindowA("STATIC", "Status: DISCONNECTED", WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 280, 310, 20, hwnd, (HMENU)IDC_STATUS_LABEL, NULL, NULL);
        hStatsLabel = CreateWindowA("STATIC", "Packets Sent: 0 | Packets Recv: 0", WS_VISIBLE | WS_CHILD | SS_CENTER, 20, 300, 310, 20, hwnd, (HMENU)IDC_STATS_LABEL, NULL, NULL);

        populateAudioDevices();

        SetTimer(hwnd, 1, 20, NULL);
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(248, 248, 242)); 
        SetBkColor(hdcStatic, RGB(30, 30, 46));      
        return (INT_PTR)hBgBrush;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT windowRect;
        GetClientRect(hwnd, &windowRect);
        FillRect(hdc, &windowRect, hBgBrush);

        RECT vuBgRect = { 20, 325, 330, 345 };
        FillRect(hdc, &vuBgRect, hPanelBrush);

        int barWidth = static_cast<int>((310.0 * audioVolumeLevel) / 100.0);
        RECT vuFillRect = { 20, 325, 20 + barWidth, 345 };
        FillRect(hdc, &vuFillRect, hGreenBrush);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDC_BTN_CONNECT && !isConnected) {
            char clientBuf[16], roomBuf[16], ipBuf[64], portBuf[16];
            GetWindowTextA(hEditClientID, clientBuf, 16);
            GetWindowTextA(hEditRoomID, roomBuf, 16);
            GetWindowTextA(hEditIP, ipBuf, 64);
            GetWindowTextA(hEditPort, portBuf, 16);

            g_clientID = static_cast<uint16_t>(std::stoi(clientBuf));
            g_roomID = static_cast<uint16_t>(std::stoi(roomBuf));
            int port = std::stoi(portBuf);

            int micSel = SendMessageA(hComboMic, CB_GETCURSEL, 0, 0);
            int speakerSel = SendMessageA(hComboSpeaker, CB_GETCURSEL, 0, 0);

            UINT micDevID = (micSel <= 0) ? WAVE_MAPPER : (micSel - 1);
            UINT speakerDevID = (speakerSel <= 0) ? WAVE_MAPPER : (speakerSel - 1);

            if (!initSpeaker(speakerDevID) || !initMicrophone(micDevID)) {
                MessageBoxA(hwnd, "Could not open selected audio hardware.", "Audio Error", MB_ICONERROR);
                break;
            }

            clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);
            inet_pton(AF_INET, ipBuf, &serverAddr.sin_addr);

            isConnected = true;
            std::thread(receiveLoop).detach();

            EnableWindow(hBtnConnect, FALSE);
            EnableWindow(hBtnPTT, TRUE);
            EnableWindow(hEditClientID, FALSE);
            EnableWindow(hEditRoomID, FALSE);
            EnableWindow(hEditIP, FALSE);
            EnableWindow(hEditPort, FALSE);

            SetWindowTextA(hStatusLabel, ("Status: CONNECTED (Room " + std::to_string(g_roomID) + ")").c_str());
        }
        break;
    }

    case WM_TIMER: {
        if (isConnected) {
            bool pttKeyPressed = (GetAsyncKeyState('T') & 0x8000);
            bool pttButtonPressed = (SendMessageA(hBtnPTT, BM_GETSTATE, 0, 0) & BST_PUSHED);

            if (pttKeyPressed || pttButtonPressed) {
                if (!isTransmitting) {
                    isTransmitting = true;
                    SetWindowTextA(hStatusLabel, "Status: TRANSMITTING VOICE...");
                }
                transmitAudioFrame();
            } else {
                if (isTransmitting) {
                    isTransmitting = false;
                    audioVolumeLevel = 0;
                    SetWindowTextA(hStatusLabel, ("Status: CONNECTED (Room " + std::to_string(g_roomID) + ")").c_str());
                } else if (audioVolumeLevel > 0) {
                    audioVolumeLevel = std::max(0, audioVolumeLevel - 5);
                }
            }

            std::string statsStr = "Packets Sent: " + std::to_string(packetsSent) + 
                                   " | Packets Recv: " + std::to_string(packetsRecv);
            SetWindowTextA(hStatsLabel, statsStr.c_str());

            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }

    case WM_DESTROY: {
        isConnected = false;
        KillTimer(hwnd, 1);
        DeleteObject(hBgBrush);
        DeleteObject(hPanelBrush);
        DeleteObject(hGreenBrush);
        DeleteObject(hDarkGreenBrush);
        if (hWaveOut) waveOutClose(hWaveOut);
        if (hWaveIn) waveInClose(hWaveIn);
        if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
        WSACleanup();
        PostQuitMessage(0);
        break;
    }

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 46));
    wc.lpszClassName = "UDPVoiceChatGUIClassV2";

    RegisterClassExA(&wc);

    hMainWnd = CreateWindowExA(
        0, "UDPVoiceChatGUIClassV2", "UDP Push-To-Talk Voice Chat",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 365, 400,
        NULL, NULL, hInstance, NULL
    );

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
