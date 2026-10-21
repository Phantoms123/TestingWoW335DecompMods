#include "ClientServices.h"
#include "Engine/Source/Storm/H/storm.h"

// VERIFY: f3d32485 auto
// OFFSET: 0x006B2030
int32_t ConsoleCommand_Logout(const char* command, const char* arguments) {
    if (!ClientServices::s_currentConnection) {
        SErrDisplayAppFatalMessage("Internal check failed", __FILE__, __LINE__);
    }

    if (ClientServices::s_currentConnection->m_inGame) {
        ClientServices::s_currentConnection->CharacterLogout(false, true);
    }

    return 1;
}
