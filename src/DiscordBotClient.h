#pragma once
#include <dpp/dpp.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include "FileLoader.h"

struct GuildInfo {
    // dpp 객체 포인터(voiceconn*, discord_client*)를 캐싱하지 않는다.
    // - voiceconn: 음성 full reconnection 때 통째로 새로 만들어진다.
    // - discord_client(shard): 게이트웨이 재연결(resume) 때 기존 객체를 복사해서 새 객체를 만들고
    //   이전 객체는 파괴된다 (discord_client(discord_client& old) 생성자).
    // 캐싱된 포인터를 계속 쓰면 use-after-free로 세그폴트가 나므로, 안정적인 shard ID(숫자)만
    // 저장하고 쓸 때마다 bot.get_shard(shardId)로 현재 객체를 다시 조회한다.
    uint32_t shardId;
    VideoDbInfo currentPlay;
    std::vector<VideoDbInfo> videoLists;
    dpp::snowflake voiceChannelId;
    std::atomic<bool> skipRequested;
    std::atomic<bool> shouldStop;
    std::atomic<bool> repeatCurrent;
    std::thread audioThread;
    std::mutex listMutex;

    // 음성 연결이 끊긴 걸 감지한 뒤 마지막으로 자동 재접속을 시도한 시각.
    // PlayAudioThread 자기 자신만 읽고 쓰므로 atomic이 아니어도 안전하다.
    std::chrono::steady_clock::time_point lastReconnectAttempt;

    // 아래 필드들은 guildInfosMutex를 잡은 상태에서만 읽고 쓴다.
    dpp::snowflake lastTextChannelId = dpp::snowflake(0);  // 마지막으로 명령어를 받은 텍스트 채널 (안내 메시지용)
    bool emptyTracking = false;                            // 봇 말고 아무도 없는 상태가 지속되는지
    std::chrono::steady_clock::time_point emptySince;      // 그 상태가 시작된 시각
    bool leavePending = false;                             // 자동 퇴장 처리 중 (중복 실행 방지)

    GuildInfo()
        : shardId(0), voiceChannelId(0), skipRequested(false), shouldStop(false), repeatCurrent(false),
          lastReconnectAttempt(std::chrono::steady_clock::now()) {}

    GuildInfo(uint32_t shardId, dpp::snowflake voiceChannelId)
        : shardId(shardId), voiceChannelId(voiceChannelId), skipRequested(false), shouldStop(false), repeatCurrent(false),
          lastReconnectAttempt(std::chrono::steady_clock::now()) {}

    // 복사 생성자/대입 삭제 (스레드는 복사할 수 없음)
    GuildInfo(const GuildInfo&) = delete;
    GuildInfo& operator=(const GuildInfo&) = delete;
    GuildInfo& operator=(GuildInfo&&) = delete;

    // 이동 생성자: 모든 필드를 명시적으로 옮긴다 (currentPlay 포함 - 레거시에서 이 부분이 빠져있었음)
    GuildInfo(GuildInfo&& other) noexcept
        : shardId(other.shardId),
          currentPlay(std::move(other.currentPlay)),
          videoLists(std::move(other.videoLists)),
          voiceChannelId(other.voiceChannelId),
          skipRequested(other.skipRequested.load()),
          shouldStop(other.shouldStop.load()),
          repeatCurrent(other.repeatCurrent.load()),
          audioThread(std::move(other.audioThread)),
          lastReconnectAttempt(std::chrono::steady_clock::now()) {}

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
    void HandleRepeatCommand(const dpp::message_create_t& event);
    void HandleCleanCommand(const std::string& args, const dpp::message_create_t& event);

    // 검색어/URL을 서버 /process로 보내고, 응답(단일 곡 또는 플레이리스트)을 재생 큐에 추가한다.
    void RequestAndEnqueue(const std::string& query, const dpp::message_create_t& event, bool insertFront);

    // guildId에 대한 GuildInfo가 없으면 만든다. guildInfosMutex를 잠근 상태에서 호출해야 한다.
    GuildInfo& EnsureGuildInfo(dpp::snowflake guildId, uint32_t shardId);

    void PlayAudioThread(dpp::snowflake guildId);
    void StartAudioThread(dpp::snowflake guildId);
    void StopAudioThread(dpp::snowflake guildId);

    // 오디오 스레드 정지 -> 음성 연결 해제 -> GuildInfo 삭제. !leave와 자동 퇴장이 공유한다.
    // 스레드 join으로 블로킹될 수 있으니 dpp 이벤트/타이머 스레드에서 직접 부르지 않는다.
    void LeaveVoice(dpp::snowflake guildId, uint32_t shardId);

    // ---- 음성 채널 인원 추적 ----
    // dpp의 guild->voice_members는 이벤트 스레드가 락 없이 수정하므로 다른 스레드에서 순회하면
    // 크래시할 수 있다. 그래서 on_voice_state_update 이벤트로 우리 쪽 자료구조를 따로 유지하고
    // (자체 뮤텍스로 보호), 타이머는 그것만 읽는다.
    struct VoiceMemberInfo {
        dpp::snowflake channelId;
        bool isBot = false;
    };
    struct VoiceObservation {
        bool known = false;       // 이 길드의 음성 상태를 알고 있는지 (스냅샷 완료 여부)
        bool botPresent = false;  // 봇 자신이 어떤 음성 채널에 있는지
        int humans = 0;           // 봇이 있는 채널의 봇 아닌 유저 수
    };
    void OnVoiceStateUpdate(const dpp::voice_state_update_t& event);
    void EnsureVoiceSeeded(dpp::snowflake guildId);
    VoiceObservation ObserveVoice(dpp::snowflake guildId);
    void ResetVoiceTracking(dpp::snowflake guildId);
    void CheckVoiceChannels();

    void SendBotMessage(const dpp::snowflake& msgChannelId, const std::string& message);
    void DeleteMessage(const dpp::snowflake& msgId, const dpp::snowflake& msgChannelId);
    void SendListMessage(dpp::snowflake channelId, dpp::snowflake guildId);

    dpp::cluster bot;
    std::map<dpp::snowflake, GuildInfo> guildInfos;
    std::mutex guildInfosMutex;

    // 채널별 마지막 봇 메시지 ID (다음 메시지를 보낼 때 이전 것을 지우기 위함)
    std::map<dpp::snowflake, dpp::snowflake> lastBotMessages;
    std::mutex lastBotMessagesMutex;

    // guildInfosMutex와 voiceMutex는 동시에 잡지 않는다 (락 순서 문제 방지).
    std::mutex voiceMutex;
    std::map<dpp::snowflake, std::map<dpp::snowflake, VoiceMemberInfo>> voiceOccupancy;  // guild -> (user -> 상태)
    std::set<dpp::snowflake> seededVoiceGuilds;
};
