#include "WoW/Source/Client.h"
#include "Engine/Source/Base/CDataStore.h"
#include "WoW/Source/WowSvcs/WowSvcsClient/AccountData.h"
#include <cstdio>

// CHANGED: these used to abort the client (on purpose: there is no native scanner, and no clean or detected
// result should be made up). The bot check timer then closed the game about 20 minutes into every session
// (GatherAdditionalChecks). They now do what the Win64 build does: no scan, nothing reported to the server,
// which is also what a client sends when its checks gather nothing. Each says so once on stderr.
namespace {
void NoBotCheck(const char* operation) {
    static uint32_t s_reported;
    uint32_t bit = 0;

    for (const char* c = operation; *c; c++) {
        bit = (bit * 31 + static_cast<uint8_t>(*c)) & 31;
    }

    if (s_reported & (1u << bit)) {
        return;
    }

    s_reported |= 1u << bit;
    std::fprintf(stderr, "Wow: native security operation %s not supported, skipped (nothing sent)\n", operation);
}
}

void AdditionalCheck1() {
    if (!s_botCheckEnabled || s_botCheckDelay <= 0 || s_botCheckDelay > 420) {
        return;
    }
    long expected = 1;
    if (!__atomic_compare_exchange_n(&s_additionalCheckArmed, &expected, 0, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return;
    }
    NoBotCheck("AdditionalCheck1");
}

void InitTokenSeed(const uint8_t* key) {
    NoBotCheck("InitTokenSeed");
}

int32_t Packet_SMSG_CHECK_FOR_BOTS(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    NoBotCheck("Packet_SMSG_CHECK_FOR_BOTS");

    if (msg) {
        uint8_t key[16];
        msg->GetArray(key, sizeof(key));
    }

    return 1;
}

int32_t GatherAdditionalChecks(uint8_t* check1, uint8_t* check2, uint8_t* check3) {
    NoBotCheck("GatherAdditionalChecks");

    if (check1 && check2 && check3) {
        *check1 = 0;
        *check2 = 0;
        *check3 = 0;
    }

    return 0;
}

uint8_t AdditionalCheck1Scan(const AdditionalCheckInterface* iface, int32_t a2) {
    NoBotCheck("AdditionalCheck1Scan");
    return 0;
}

void LoadEncryptedModule(const uint8_t* key, uint32_t length, void (*callback)()) {
    NoBotCheck("LoadEncryptedModule");
}
