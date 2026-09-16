#pragma once
#include <dpp/dpp.h>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "FileLoader.h"

struct GuildInfo {
    dpp::voiceconn* vconn;
    VideoDbInfo currentPlay;
    std::vector<VideoDbInfo> videoLists;
    dpp::snowflake voiceChannelId;
    std::atomic<bool> skipRequested;
    std::atomic<bool> shouldStop;
    std::thread audioThread;
    std::mutex listMutex;

    GuildInfo() : vconn(nullptr), voiceChannelId(0), skipRequested(false), shouldStop(false) {}

    GuildInfo(dpp::voiceconn* vconn, dpp::snowflake voiceChannelId)
        : vconn(vconn), voiceChannelId(voiceChannelId), skipRequested(false), shouldStop(false) {}

    // 복사 생성자/대입 삭제 (스레드는 복사할 수 없음)
    GuildInfo(const GuildInfo&) = delete;
    GuildInfo& operator=(const GuildInfo&) = delete;
    GuildInfo& operator=(GuildInfo&&) = delete;

    // 이동 생성자: 모든 필드를 명시적으로 옮긴다 (currentPlay 포함 - 레거시에서 이 부분이 빠져있었음)
    GuildInfo(GuildInfo&& other) noexcept
        : vconn(other.vconn),
          currentPlay(std::move(other.currentPlay)),
          videoLists(std::move(other.videoLists)),
          voiceChannelId(other.voiceChannelId),
          skipRequested(other.skipRequested.load()),
          shouldStop(other.shouldStop.load()),
          audioThread(std::move(other.audioThread)) {}

    ~GuildInfo() {
        if (audioThread.joinable()) {
            shouldStop = true;
            audioThread.join();
        }
    }
};

class DiscordBotClient {
public:
    DiscordBotClient(const std::string& token);
    void run();

private:
    void HandleCommand(const std::string& command, const dpp::message_create_t& event);
    void HandlePlayCommand(const std::string& args, const dpp::message_create_t& event);
    void HandleFirstPlayCommand(const std::string& args, const dpp::message_create_t& event);
    void HandleJoinCommand(const dpp::message_create_t& event);
    void HandleLeaveCommand(const dpp::message_create_t& event);
    void HandleSkipCommand(const dpp::message_create_t& event);
    void HandleOmakaseCommand(const std::string& args, const dpp::message_create_t& event);
    void HandleListCommand(const dpp::message_create_t& event);
    void HandleRandomCommand(const dpp::message_create_t& event);
    void HandleRemoveCommand(const dpp::message_create_t& event);
    void HandleDeleteCommand(const dpp::message_create_t& event, bool shouldSkip = false);

    // 검색어/URL을 서버 /process로 보내고, 응답(단일 곡 또는 플레이리스트)을 재생 큐에 추가한다.
    void RequestAndEnqueue(const std::string& query, const dpp::message_create_t& event, bool insertFront);

    // guildId에 대한 GuildInfo가 없으면 만든다. guildInfosMutex를 잠근 상태에서 호출해야 한다.
    GuildInfo& EnsureGuildInfo(dpp::snowflake guildId, dpp::discord_client* shard);

    void PlayAudioThread(dpp::snowflake guildId);
    void StartAudioThread(dpp::snowflake guildId);
    void StopAudioThread(dpp::snowflake guildId);

    void SendBotMessage(const dpp::snowflake& msgChannelId, const std::string& message);
    void DeleteMessage(const dpp::snowflake& msgId, const dpp::snowflake& msgChannelId);
    void SendListMessage(dpp::snowflake channelId, dpp::snowflake guildId);

    dpp::cluster bot;
    std::map<dpp::snowflake, GuildInfo> guildInfos;
    std::mutex guildInfosMutex;

    // 채널별 마지막 봇 메시지 ID (다음 메시지를 보낼 때 이전 것을 지우기 위함)
    std::map<dpp::snowflake, dpp::snowflake> lastBotMessages;
    std::mutex lastBotMessagesMutex;
};
