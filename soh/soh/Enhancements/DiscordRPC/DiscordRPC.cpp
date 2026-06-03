// console check
#if !defined(__SWITCH__) && !defined(__WIIU__)

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge.h>
#include <nlohmann/json.hpp>

#include "soh/util.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// macos has no MSG_NOSIGNAL, uses SO_NOSIGPIPE instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

extern "C" {
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

// discord application ID
#define DISCORD_APP_ID "1511502474530263141"
#define CVAR_DISCORD_RPC CVAR_ENHANCEMENT("DiscordRPC")

namespace {

// discord's local ipc: framed json, little-endian header (u32 opcode, u32 len) then payload
class DiscordIpc {
  public:
    enum Opcode : uint32_t {
        OP_HANDSHAKE = 0,
        OP_FRAME = 1,
        OP_CLOSE = 2,
        OP_PING = 3,
        OP_PONG = 4,
    };

    ~DiscordIpc() {
        Close();
    }

    bool IsConnected() const {
#ifdef _WIN32
        return mPipe != INVALID_HANDLE_VALUE;
#else
        return mFd >= 0;
#endif
    }

    // connect + handshake, false if discord isn't reachable
    bool Connect() {
        if (IsConnected()) {
            return true;
        }
        if (!OpenPipe()) {
            return false;
        }

        nlohmann::json handshake = {
            { "v", 1 },
            { "client_id", DISCORD_APP_ID },
        };
        if (!Write(OP_HANDSHAKE, handshake.dump())) {
            Close();
            return false;
        }
        return true;
    }

    void Close() {
#ifdef _WIN32
        if (mPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(mPipe);
            mPipe = INVALID_HANDLE_VALUE;
        }
#else
        if (mFd >= 0) {
            close(mFd);
            mFd = -1;
        }
#endif
    }

    bool SetActivity(const std::string& details, const std::string& state, int64_t startTimestamp) {
        nlohmann::json activity = {
            { "details", details },
            { "timestamps", { { "start", startTimestamp } } },
            { "assets", { { "large_image", "logo" }, { "large_text", "Ship of Harkinian" } } },
        };
        if (!state.empty()) {
            activity["state"] = state;
        }
        nlohmann::json frame = {
            { "cmd", "SET_ACTIVITY" },
            { "nonce", std::to_string(++mNonce) },
            { "args", { { "pid", GetPid() }, { "activity", activity } } },
        };

        // drain any reply so discord's buffer doesn't back up; contents ignored
        DrainReads();
        return Write(OP_FRAME, frame.dump());
    }

  private:
    int GetPid() const {
#ifdef _WIN32
        return static_cast<int>(GetCurrentProcessId());
#else
        return static_cast<int>(getpid());
#endif
    }

    bool Write(uint32_t opcode, const std::string& payload) {
        uint32_t header[2] = { opcode, static_cast<uint32_t>(payload.size()) };
        return WriteRaw(reinterpret_cast<const char*>(header), sizeof(header)) &&
               WriteRaw(payload.data(), payload.size());
    }

#ifdef _WIN32
    bool OpenPipe() {
        for (int i = 0; i < 10; i++) {
            std::string path = "\\\\?\\pipe\\discord-ipc-" + std::to_string(i);
            HANDLE pipe =
                CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                mPipe = pipe;
                return true;
            }
        }
        return false;
    }

    bool WriteRaw(const char* data, size_t length) {
        size_t written = 0;
        while (written < length) {
            DWORD chunk = 0;
            if (!WriteFile(mPipe, data + written, static_cast<DWORD>(length - written), &chunk, nullptr) ||
                chunk == 0) {
                Close();
                return false;
            }
            written += chunk;
        }
        return true;
    }

    void DrainReads() {
        char buffer[1024];
        DWORD available = 0;
        while (PeekNamedPipe(mPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            DWORD want = available < sizeof(buffer) ? available : sizeof(buffer);
            if (!ReadFile(mPipe, buffer, want, &read, nullptr) || read == 0) {
                break;
            }
        }
    }

    HANDLE mPipe = INVALID_HANDLE_VALUE;
#else
    bool OpenPipe() {
        // dirs discord might put the socket in
        std::vector<std::string> bases;
        for (const char* var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" }) {
            const char* value = getenv(var);
            if (value != nullptr && value[0] != '\0') {
                bases.push_back(value);
            }
        }
        bases.push_back("/tmp");

        // flatpak/snap nest it a dir deeper
        const char* subdirs[] = { "", "/app/com.discordapp.Discord", "/snap.discord" };

        for (const std::string& base : bases) {
            for (const char* sub : subdirs) {
                for (int i = 0; i < 10; i++) {
                    std::string path = base + sub + "/discord-ipc-" + std::to_string(i);
                    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
                        continue;
                    }
                    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (fd < 0) {
                        return false;
                    }
#ifdef SO_NOSIGPIPE
                    int on = 1;
                    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
                    sockaddr_un addr = {};
                    addr.sun_family = AF_UNIX;
                    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                        mFd = fd;
                        return true;
                    }
                    close(fd);
                }
            }
        }
        return false;
    }

    bool WriteRaw(const char* data, size_t length) {
        size_t written = 0;
        while (written < length) {
            ssize_t chunk = send(mFd, data + written, length - written, MSG_NOSIGNAL);
            if (chunk <= 0) {
                Close();
                return false;
            }
            written += static_cast<size_t>(chunk);
        }
        return true;
    }

    void DrainReads() {
        char buffer[1024];
        while (recv(mFd, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
    }

    int mFd = -1;
#endif

    uint64_t mNonce = 0;
};

DiscordIpc gIpc;
std::string gLastKey;
int64_t gStartTimestamp = 0;
// don't retry the socket every frame when discord's closed (~5s at 60fps)
int gReconnectCooldown = 0;
constexpr int RECONNECT_COOLDOWN_FRAMES = 300;

void UpdatePresence() {
    if (!CVarGetInteger(CVAR_DISCORD_RPC, 0)) {
        if (gIpc.IsConnected()) {
            gIpc.Close();
            gLastKey.clear();
        }
        return;
    }

    if (!gIpc.IsConnected()) {
        if (gReconnectCooldown > 0) {
            gReconnectCooldown--;
            return;
        }
        if (!gIpc.Connect()) {
            gReconnectCooldown = RECONNECT_COOLDOWN_FRAMES;
            return;
        }
        // just connected, force a fresh push
        gLastKey.clear();
        if (gStartTimestamp == 0) {
            gStartTimestamp = static_cast<int64_t>(time(nullptr));
        }
    }

    std::string details;
    std::string state;

    // the title demo also runs in a playstate, so only gameMode == GAMEMODE_NORMAL is real gameplay
    bool inGameplay = gPlayState != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL;
    if (inGameplay) {
        if (gPlayState->csCtx.state != CS_STATE_IDLE) {
            details = "Watching a Cutscene";
        } else {
            details = "Exploring " + SohUtils::GetSceneName(gPlayState->sceneNum);
        }

        // second line: age, plus mode if any
        state = LINK_IS_CHILD ? "Young Link" : "Adult Link";
        if (IS_BOSS_RUSH) {
            state += " | Boss Rush";
        } else if (IS_RANDO) {
            state += " | Randomizer";
        }
    } else {
        details = "In the Menus";
    }

    std::string key = details + '\x1f' + state;
    if (key == gLastKey) {
        return;
    }

    if (!gIpc.SetActivity(details, state, gStartTimestamp)) {
        gReconnectCooldown = RECONNECT_COOLDOWN_FRAMES;
        return;
    }
    gLastKey = key;
}

void RegisterDiscordRPC() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(UpdatePresence);
}

} // namespace

static RegisterShipInitFunc initFunc(RegisterDiscordRPC);

#endif // !__SWITCH__ && !__WIIU__
