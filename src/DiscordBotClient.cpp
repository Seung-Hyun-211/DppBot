#include "DiscordBotClient.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <random>
#include <cctype>

constexpr size_t FRAME_SIZE = 11520;         // 60ms 분량 (48000Hz * 2ch * 2byte * 0.06s)
constexpr uint64_t SILENCE_DURATION_MS = 20;

// 프레임(60ms 분량)을 실제 재생 속도보다 조금 빠르게 보내서 버퍼가 마르지 않게 한다.
// 프레임마다 (60 - 58)ms 만큼 dpp 내부 버퍼에 오디오가 선행해서 쌓이고, 곡이 끝난 뒤에는
// 그 누적량만큼 기다려서 남은 오디오가 다 재생된 뒤 다음 곡으로 넘어간다.
// 전송 간격을 바꾸면 선행량도 자동으로 맞춰지도록 상수 하나에서 파생시킨다.
constexpr uint32_t FRAME_DURATION_MS = 60;
constexpr uint32_t SEND_INTERVAL_MS = 58;
constexpr uint32_t LEAD_PER_FRAME_MS = FRAME_DURATION_MS - SEND_INTERVAL_MS;

// 봇 말고 아무도 없는 상태가 이 시간 이상 지속되면 음성 채널에서 나간다.
// 잠깐 튕겼다 돌아오는 경우를 넘기기 위한 유예 시간이다.
constexpr std::chrono::seconds EMPTY_CHANNEL_GRACE(10);
// 인원 확인 주기. 유예 시간보다 충분히 짧아야 실제 퇴장 시점이 유예 시간에 가깝다.
constexpr uint64_t VOICE_CHECK_INTERVAL_SEC = 2;

DiscordBotClient::DiscordBotClient(const std::string& token)
    : bot(token, dpp::i_all_intents)
{
    bot.on_log(dpp::utility::cout_logger());

    bot.on_ready([this](const dpp::ready_t&) {
        std::cout << "Bot logged in successfully!\n";
    });

    bot.on_message_create([this](const dpp::message_create_t& event) {
        if (event.msg.author.is_bot()) return;
        if (event.msg.content.rfind("!", 0) == 0) {
            HandleCommand(event.msg.content.substr(1), event);
        }
    });

    bot.on_voice_state_update([this](const dpp::voice_state_update_t& event) {
        OnVoiceStateUpdate(event);
    });

    // on_ready는 재연결 때 다시 호출될 수 있어서 타이머가 중복 등록되지 않도록 생성자에서 한 번만 등록한다.
    bot.start_timer([this](dpp::timer) { CheckVoiceChannels(); }, VOICE_CHECK_INTERVAL_SEC);
}

void DiscordBotClient::run() {
    bot.start(dpp::st_wait);
}

void DiscordBotClient::HandleCommand(const std::string& command, const dpp::message_create_t& event) {
    std::string lowerCommand = command;
    std::transform(lowerCommand.begin(), lowerCommand.end(), lowerCommand.begin(),
        [](unsigned char c) { return std::tolower(c); });

    auto argsAfterFirstSpace = [&]() -> std::string {
        size_t spacePos = command.find(' ');
        return (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
    };

    // 자동 퇴장 안내 메시지를 보낼 채널을 기억해둔다.
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        auto it = guildInfos.find(event.msg.guild_id);
        if (it != guildInfos.end()) {
            it->second.lastTextChannelId = event.msg.channel_id;
        }
    }

    if (lowerCommand == "join" || lowerCommand == "j") {
        HandleJoinCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "leave" || lowerCommand == "l") {
        HandleLeaveCommand(event);
        SendBotMessage(event.msg.channel_id, "잘자용");
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand.rfind("play ", 0) == 0 || lowerCommand.rfind("p ", 0) == 0) {
        HandlePlayCommand(argsAfterFirstSpace(), event);
    } else if (lowerCommand.rfind("first ", 0) == 0 || lowerCommand.rfind("f ", 0) == 0) {
        HandleFirstPlayCommand(argsAfterFirstSpace(), event);
    } else if (lowerCommand == "skip" || lowerCommand == "s") {
        HandleSkipCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "omakase" || lowerCommand.rfind("omakase ", 0) == 0) {
        HandleOmakaseCommand(argsAfterFirstSpace(), event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "list") {
        HandleListCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "random") {
        HandleRandomCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "remove" || lowerCommand.rfind("remove ", 0) == 0) {
        HandleRemoveCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "delete" || lowerCommand.rfind("delete ", 0) == 0) {
        std::string args = argsAfterFirstSpace();
        std::transform(args.begin(), args.end(), args.begin(), [](unsigned char c) { return std::tolower(c); });
        bool shouldSkip = args.find('s') != std::string::npos;
        HandleDeleteCommand(event, shouldSkip);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "repeat") {
        HandleRepeatCommand(event);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    } else if (lowerCommand == "clean" || lowerCommand.rfind("clean ", 0) == 0) {
        HandleCleanCommand(argsAfterFirstSpace(), event);
    }
}

// ================= 서버 호출 =================

GuildInfo& DiscordBotClient::EnsureGuildInfo(dpp::snowflake guildId, uint32_t shardId) {
    auto it = guildInfos.find(guildId);
    if (it != guildInfos.end()) {
        return it->second;
    }

    dpp::discord_client* shard = bot.get_shard(shardId);
    dpp::voiceconn* vconn = shard ? shard->get_voice(guildId) : nullptr;
    dpp::snowflake voiceChannelId = (vconn && vconn->channel_id != dpp::snowflake(0)) ? vconn->channel_id : dpp::snowflake(0);
    auto result = guildInfos.emplace(guildId, GuildInfo(shardId, voiceChannelId));
    return result.first->second;
}

void DiscordBotClient::HandlePlayCommand(const std::string& args, const dpp::message_create_t& event) {
    if (args.empty()) {
        event.reply("?");
        return;
    }
    RequestAndEnqueue(args, event, false);
}

void DiscordBotClient::HandleFirstPlayCommand(const std::string& args, const dpp::message_create_t& event) {
    if (args.empty()) {
        SendBotMessage(event.msg.channel_id, "검색어를 입력해주세요.");
        return;
    }
    RequestAndEnqueue(args, event, true);
}

// 검색어/URL을 서버 /process로 보내고, 응답(단일 곡 또는 플레이리스트)을 재생 큐에 추가한다.
// 서버 호출은 dpp::cluster::request()를 통한 비동기 HTTP라서 디스코드 이벤트 스레드를 막지 않는다.
void DiscordBotClient::RequestAndEnqueue(const std::string& query, const dpp::message_create_t& event, bool insertFront) {
    bool needJoinFirst = false;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        needJoinFirst = guildInfos.find(event.msg.guild_id) == guildInfos.end();
    }
    if (needJoinFirst) {
        HandleJoinCommand(event);
    }

    bot.message_add_reaction(event.msg, "🔃");

    dpp::snowflake guildId = event.msg.guild_id;
    uint32_t shardId = event.from()->shard_id;
    std::string path = "/process?q=" + FileLoader::UrlEncode(query);

    FileLoader::RequestServer(bot, path, [this, event, guildId, shardId, insertFront](bool ok, nlohmann::json json, std::string err) {
        auto markFailed = [this, event]() {
            bot.message_delete_own_reaction(event.msg, "🔃", [this, event](const dpp::confirmation_callback_t& cb) {
                if (!cb.is_error()) bot.message_add_reaction(event.msg, "❌");
            });
        };

        if (!ok) {
            std::cerr << "[RequestAndEnqueue] " << err << std::endl;
            markFailed();
            return;
        }
        if (!json.value("success", false)) {
            markFailed();
            SendBotMessage(event.msg.channel_id, "오류: " + json.value("error", std::string("Unknown error")));
            return;
        }

        std::vector<VideoDbInfo> items;
        std::string summary;

        if (json.contains("list") && json["list"].is_array()) {
            for (const auto& item : json["list"]) {
                VideoDbInfo info = FileLoader::ParseVideoDbInfo(item);
                if (!info.filePath.empty()) items.push_back(info);
            }
            int total = json.value("total", static_cast<int>(items.size()));
            int successCount = json.value("success_count", static_cast<int>(items.size()));
            int failedCount = json.value("failed_count", 0);
            summary = "플레이리스트 " + std::to_string(items.size()) + "개 추가됨 (전체: " +
                      std::to_string(total) + ", 성공: " + std::to_string(successCount) +
                      ", 실패: " + std::to_string(failedCount) + ")";
        } else {
            VideoDbInfo info = FileLoader::ParseVideoDbInfo(json.value("metadata", nlohmann::json::object()));
            info.filePath = json.value("local_path", "");
            if (info.filePath.empty()) {
                markFailed();
                return;
            }
            items.push_back(info);
            summary = info.title;
        }

        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            GuildInfo& guildInfo = EnsureGuildInfo(guildId, shardId);
            std::lock_guard<std::mutex> listLock(guildInfo.listMutex);
            if (insertFront) {
                guildInfo.videoLists.insert(guildInfo.videoLists.begin(), items.begin(), items.end());
            } else {
                guildInfo.videoLists.insert(guildInfo.videoLists.end(), items.begin(), items.end());
            }
        }

        bot.message_delete_own_reaction(event.msg, "🔃", [this, event](const dpp::confirmation_callback_t& cb) {
            if (!cb.is_error()) bot.message_add_reaction(event.msg, "✅");
        });

        std::string newMessage = "***" + event.msg.author.global_name + "*** - " + summary + (insertFront ? " (먼저 재생)" : "");
        SendBotMessage(event.msg.channel_id, newMessage);
        DeleteMessage(event.msg.id, event.msg.channel_id);
    });
}

void DiscordBotClient::HandleOmakaseCommand(const std::string& args, const dpp::message_create_t& event) {
    int count = 30;
    if (!args.empty()) {
        try {
            count = std::stoi(args);
            if (count <= 0) count = 30;
        } catch (...) {
            count = 30;
        }
    }

    bool needJoinFirst = false;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        needJoinFirst = guildInfos.find(event.msg.guild_id) == guildInfos.end();
    }
    if (needJoinFirst) {
        HandleJoinCommand(event);
    }

    bot.message_add_reaction(event.msg, "🔃");

    dpp::snowflake guildId = event.msg.guild_id;
    uint32_t shardId = event.from()->shard_id;
    std::string path = "/getrandom?count=" + std::to_string(count);

    FileLoader::RequestServer(bot, path, [this, event, guildId, shardId](bool ok, nlohmann::json json, std::string err) {
        bool hasItems = ok && json.value("success", false) && json.contains("items") && json["items"].is_array();
        if (!hasItems) {
            std::string errMsg = ok ? json.value("error", std::string("응답에 items가 없습니다.")) : err;
            std::cerr << "[HandleOmakaseCommand] " << errMsg << std::endl;
            bot.message_delete_own_reaction(event.msg, "🔃", [this, event](const dpp::confirmation_callback_t& cb) {
                if (!cb.is_error()) bot.message_add_reaction(event.msg, "❌");
            });
            SendBotMessage(event.msg.channel_id, "오류: " + errMsg);
            return;
        }

        int addedCount = 0;
        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            GuildInfo& guildInfo = EnsureGuildInfo(guildId, shardId);
            std::lock_guard<std::mutex> listLock(guildInfo.listMutex);
            for (const auto& item : json["items"]) {
                VideoDbInfo info = FileLoader::ParseVideoDbInfo(item);
                if (!info.filePath.empty()) {
                    guildInfo.videoLists.push_back(info);
                    addedCount++;
                }
            }
        }

        bot.message_delete_own_reaction(event.msg, "🔃", [this, event](const dpp::confirmation_callback_t& cb) {
            if (!cb.is_error()) bot.message_add_reaction(event.msg, "✅");
        });

        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            auto it = guildInfos.find(guildId);
            if (it != guildInfos.end() && !it->second.audioThread.joinable()) {
                StartAudioThread(guildId);
            }
        }

        std::cout << "[HandleOmakaseCommand] Added " << addedCount << " items for guild " << guildId << std::endl;
        SendListMessage(event.msg.channel_id, guildId);
    });
}

void DiscordBotClient::HandleDeleteCommand(const dpp::message_create_t& event, bool shouldSkip) {
    dpp::snowflake guildId = event.msg.guild_id;
    std::string currentVideoId;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        auto it = guildInfos.find(guildId);
        if (it == guildInfos.end()) {
            SendBotMessage(event.msg.channel_id, "채널 스레드가 존재하지 않음.");
            return;
        }
        currentVideoId = it->second.currentPlay.id;
    }

    if (currentVideoId.empty()) {
        SendBotMessage(event.msg.channel_id, "현재 재생 중인 노래가 없습니다.");
        return;
    }

    std::string path = "/deactivate?id=" + FileLoader::UrlEncode(currentVideoId);
    FileLoader::RequestServer(bot, path, [this, event, shouldSkip](bool ok, nlohmann::json json, std::string err) {
        if (!ok || !json.value("success", false)) {
            SendBotMessage(event.msg.channel_id, "오류: " + (ok ? json.value("error", std::string("Unknown error")) : err));
            return;
        }
        if (shouldSkip) {
            HandleSkipCommand(event);
        }
        SendBotMessage(event.msg.channel_id, "추천 비활성화됨");
    });
}

// ================= 재생 큐 =================

void DiscordBotClient::HandleSkipCommand(const dpp::message_create_t& event) {
    std::lock_guard<std::mutex> lock(guildInfosMutex);
    auto it = guildInfos.find(event.msg.guild_id);
    if (it != guildInfos.end()) {
        it->second.skipRequested = true;
    }
    SendBotMessage(event.msg.channel_id, "넘김");
}

void DiscordBotClient::HandleRepeatCommand(const dpp::message_create_t& event) {
    std::lock_guard<std::mutex> lock(guildInfosMutex);
    auto it = guildInfos.find(event.msg.guild_id);
    if (it == guildInfos.end()) {
        SendBotMessage(event.msg.channel_id, "채널 스레드가 존재하지 않음.");
        return;
    }

    bool nowRepeating = !it->second.repeatCurrent.load();
    it->second.repeatCurrent = nowRepeating;
    SendBotMessage(event.msg.channel_id, nowRepeating ? "반복 재생 켜짐" : "반복 재생 꺼짐");
}

void DiscordBotClient::HandleRandomCommand(const dpp::message_create_t& event) {
    std::lock_guard<std::mutex> lock(guildInfosMutex);
    auto it = guildInfos.find(event.msg.guild_id);
    if (it == guildInfos.end()) {
        SendBotMessage(event.msg.channel_id, "재생 목록이 없음");
        return;
    }

    GuildInfo& guildInfo = it->second;
    std::lock_guard<std::mutex> listLock(guildInfo.listMutex);
    if (guildInfo.videoLists.empty()) {
        SendBotMessage(event.msg.channel_id, "섞을 노래가 없음");
        return;
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(guildInfo.videoLists.begin(), guildInfo.videoLists.end(), g);
    SendBotMessage(event.msg.channel_id, "랜덤 재생");
}

void DiscordBotClient::HandleRemoveCommand(const dpp::message_create_t& event) {
    dpp::snowflake guildId = event.msg.guild_id;

    std::string command = event.msg.content.substr(1);
    size_t spacePos = command.find(' ');
    if (spacePos == std::string::npos) {
        SendBotMessage(event.msg.channel_id, "인덱스를 입력해주세요. 예: !remove 1");
        return;
    }

    int index = 0;
    try {
        index = std::stoi(command.substr(spacePos + 1));
    } catch (...) {
        SendBotMessage(event.msg.channel_id, "유효하지 않은 인덱스입니다.");
        return;
    }
    if (index <= 0) {
        SendBotMessage(event.msg.channel_id, "인덱스는 1 이상이어야 합니다.");
        return;
    }

    GuildInfo* guildInfo = nullptr;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        auto it = guildInfos.find(guildId);
        if (it == guildInfos.end()) {
            SendBotMessage(event.msg.channel_id, "채널 스레드가 존재하지 않음.");
            return;
        }
        guildInfo = &it->second;
    }

    std::lock_guard<std::mutex> listLock(guildInfo->listMutex);
    if (index <= static_cast<int>(guildInfo->videoLists.size())) {
        std::string title = guildInfo->videoLists[index - 1].title;
        guildInfo->videoLists.erase(guildInfo->videoLists.begin() + index - 1);
        SendBotMessage(event.msg.channel_id, "[ " + title + " ] 가 목록에서 제거되었습니다.");
    } else {
        SendBotMessage(event.msg.channel_id, "유효하지 않은 인덱스");
    }
}

void DiscordBotClient::SendListMessage(dpp::snowflake channelId, dpp::snowflake guildId) {
    std::lock_guard<std::mutex> lock(guildInfosMutex);
    auto it = guildInfos.find(guildId);
    if (it == guildInfos.end()) {
        SendBotMessage(channelId, "재생 목록이 없습니다.");
        return;
    }

    GuildInfo& guildInfo = it->second;
    std::string message = "현재\n```\n";

    std::lock_guard<std::mutex> listLock(guildInfo.listMutex);
    int startIndex = 0;
    std::string currentTitle;

    if (!guildInfo.currentPlay.title.empty()) {
        currentTitle = guildInfo.currentPlay.title;
    } else if (!guildInfo.videoLists.empty()) {
        currentTitle = guildInfo.videoLists[0].title;
        startIndex = 1;
    } else {
        currentTitle = "(재생 중인 노래 없음)";
    }

    message += currentTitle + "\n```다음\n```\n";

    int listSize = static_cast<int>(guildInfo.videoLists.size());
    int showCount = std::min(5, listSize - startIndex);
    for (int i = 0; i < showCount; i++) {
        message += std::to_string(i + 1) + " : " + guildInfo.videoLists[startIndex + i].title + "\n";
    }
    message += "Count : " + std::to_string(listSize) + "\n```";

    SendBotMessage(channelId, message);
}

void DiscordBotClient::HandleListCommand(const dpp::message_create_t& event) {
    SendListMessage(event.msg.channel_id, event.msg.guild_id);
}

// ================= 음성 연결 =================

void DiscordBotClient::HandleJoinCommand(const dpp::message_create_t& event) {
    dpp::guild* g = dpp::find_guild(event.msg.guild_id);
    if (!g) {
        SendBotMessage(event.msg.channel_id, "ERROR");
        return;
    }
    if (g->voice_members.find(event.msg.author.id) == g->voice_members.end()) {
        SendBotMessage(event.msg.channel_id, "음성채널 입장후 사용가능");
        return;
    }

    // shard 포인터는 게이트웨이 재연결 때 파괴/재생성되므로 들고 있지 않고, ID만 기억해서
    // 쓸 때마다 bot.get_shard()로 현재 객체를 조회한다.
    const uint32_t shardId = event.from()->shard_id;

    bool hadExistingConnection = false;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        hadExistingConnection = guildInfos.find(event.msg.guild_id) != guildInfos.end();
    }

    if (hadExistingConnection) {
        // 오디오 스레드를 먼저 정지한다 - voiceclient를 아직 참조하고 있을 수 있으므로,
        // 스레드를 멈추기 전에 연결을 끊으면 use-after-free 위험이 있다.
        StopAudioThread(event.msg.guild_id);

        // 캐싱된 값 대신 지금 시점의 shard/voiceconn을 다시 조회한다 (dpp가 그 사이
        // 내부적으로 재연결하면서 이전 객체를 없앴을 수 있음).
        dpp::discord_client* shard = bot.get_shard(shardId);
        dpp::voiceconn* existingVconn = shard ? shard->get_voice(event.msg.guild_id) : nullptr;
        if (existingVconn && existingVconn->voiceclient && existingVconn->voiceclient->is_ready()) {
            existingVconn->voiceclient->stop_audio();
            existingVconn->disconnect();
        }

        std::lock_guard<std::mutex> lock(guildInfosMutex);
        guildInfos.erase(event.msg.guild_id);
    }

    // 인원 스냅샷을 새로 뜨도록 초기화 (이전 세션 동안 놓친 이벤트가 있어도 다시 맞춰진다).
    ResetVoiceTracking(event.msg.guild_id);

    if (dpp::discord_client* shard = bot.get_shard(shardId)) {
        shard->disconnect_voice(event.msg.guild_id);
    }

    if (g->connect_member_voice(bot, event.msg.author.id)) {
        dpp::discord_client* shard = bot.get_shard(shardId);
        dpp::voiceconn* vconn = shard ? shard->get_voice(event.msg.guild_id) : nullptr;
        if (vconn) {
            dpp::snowflake voiceChannelId = (vconn->channel_id != dpp::snowflake(0)) ? vconn->channel_id : dpp::snowflake(0);
            {
                std::lock_guard<std::mutex> lock(guildInfosMutex);
                guildInfos.erase(event.msg.guild_id);
                auto created = guildInfos.emplace(event.msg.guild_id, GuildInfo(shardId, voiceChannelId));
                created.first->second.lastTextChannelId = event.msg.channel_id;
            }
            StartAudioThread(event.msg.guild_id);
        }
        SendBotMessage(event.msg.channel_id, "ㅎㅇ");
    } else {
        SendBotMessage(event.msg.channel_id, "음성 채널 연결 실패");
    }
}

void DiscordBotClient::HandleLeaveCommand(const dpp::message_create_t& event) {
    LeaveVoice(event.msg.guild_id, event.from()->shard_id);
}

void DiscordBotClient::LeaveVoice(dpp::snowflake guildId, uint32_t shardId) {
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        auto it = guildInfos.find(guildId);
        if (it != guildInfos.end()) {
            it->second.shouldStop = true;
        }
    }

    // 스레드를 먼저 종료한 뒤 음성 연결을 끊는다.
    try {
        StopAudioThread(guildId);
    } catch (const std::exception& e) {
        std::cerr << "[LeaveVoice] StopAudioThread 예외: " << e.what() << std::endl;
    }

    try {
        if (dpp::discord_client* shard = bot.get_shard(shardId)) {
            shard->disconnect_voice(guildId);
        }
    } catch (const std::exception& e) {
        std::cerr << "[LeaveVoice] disconnect_voice 예외: " << e.what() << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        guildInfos.erase(guildId);
    }
    ResetVoiceTracking(guildId);
}

// ================= 음성 채널 인원 추적 / 자동 퇴장 =================

void DiscordBotClient::OnVoiceStateUpdate(const dpp::voice_state_update_t& event) {
    const dpp::voicestate& st = event.state;
    if (st.guild_id == dpp::snowflake(0) || st.user_id == dpp::snowflake(0)) return;

    const bool left = (st.channel_id == dpp::snowflake(0));
    bool isBot = false;
    if (!left) {
        dpp::user* u = dpp::find_user(st.user_id);
        isBot = (u != nullptr) && u->is_bot();   // 캐시에 없으면 사람으로 간주 (보수적)
    }

    std::lock_guard<std::mutex> lock(voiceMutex);
    if (left) {
        auto it = voiceOccupancy.find(st.guild_id);
        if (it != voiceOccupancy.end()) it->second.erase(st.user_id);
    } else {
        VoiceMemberInfo info;
        info.channelId = st.channel_id;
        info.isBot = isBot;
        voiceOccupancy[st.guild_id][st.user_id] = info;
    }
}

// 이미 채널에 있던 사람들은 이벤트가 오지 않으므로 dpp 캐시에서 한 번만 스냅샷을 뜬다.
// dpp가 voice_members를 락 없이 수정하기 때문에 이 접근은 길드당 세션마다 한 번으로 제한한다.
void DiscordBotClient::EnsureVoiceSeeded(dpp::snowflake guildId) {
    {
        std::lock_guard<std::mutex> lock(voiceMutex);
        if (seededVoiceGuilds.count(guildId)) return;
    }

    dpp::guild* g = dpp::find_guild(guildId);
    if (!g) return;  // 길드가 아직 캐시에 없으면 판단을 미룬다 (섣불리 "비었다"고 결론내리지 않음)
    std::map<dpp::snowflake, dpp::voicestate> snapshot = g->voice_members;

    std::vector<std::pair<dpp::snowflake, VoiceMemberInfo>> entries;
    for (const auto& entry : snapshot) {
        if (entry.second.channel_id == dpp::snowflake(0)) continue;
        dpp::user* u = dpp::find_user(entry.first);
        VoiceMemberInfo info;
        info.channelId = entry.second.channel_id;
        info.isBot = (u != nullptr) && u->is_bot();
        entries.emplace_back(entry.first, info);
    }

    std::lock_guard<std::mutex> lock(voiceMutex);
    auto& members = voiceOccupancy[guildId];
    for (const auto& e : entries) {
        members.emplace(e.first, e.second);  // 이미 이벤트로 들어온 값이 더 최신이므로 덮어쓰지 않는다
    }
    seededVoiceGuilds.insert(guildId);
}

DiscordBotClient::VoiceObservation DiscordBotClient::ObserveVoice(dpp::snowflake guildId) {
    VoiceObservation obs;
    const dpp::snowflake botId = bot.me.id;

    std::lock_guard<std::mutex> lock(voiceMutex);
    if (botId == dpp::snowflake(0) || !seededVoiceGuilds.count(guildId)) return obs;
    obs.known = true;

    auto guildIt = voiceOccupancy.find(guildId);
    if (guildIt == voiceOccupancy.end()) return obs;
    const auto& members = guildIt->second;

    auto botIt = members.find(botId);
    if (botIt == members.end()) return obs;
    obs.botPresent = true;

    for (const auto& entry : members) {
        if (entry.first == botId || entry.second.isBot) continue;
        if (entry.second.channelId != botIt->second.channelId) continue;
        obs.humans++;
    }
    return obs;
}

void DiscordBotClient::ResetVoiceTracking(dpp::snowflake guildId) {
    std::lock_guard<std::mutex> lock(voiceMutex);
    seededVoiceGuilds.erase(guildId);
    voiceOccupancy.erase(guildId);
}

// 타이머(dpp 타이머 스레드)에서 주기적으로 호출된다. 오래 블로킹하면 안 되므로
// 실제 퇴장 처리(스레드 join 등)는 별도 워커 스레드로 넘긴다.
void DiscordBotClient::CheckVoiceChannels() {
    struct Target {
        dpp::snowflake guildId;
        uint32_t shardId;
    };

    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        for (const auto& entry : guildInfos) {
            if (!entry.second.leavePending) {
                targets.push_back({entry.first, entry.second.shardId});
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const Target& target : targets) {
        EnsureVoiceSeeded(target.guildId);
        const VoiceObservation obs = ObserveVoice(target.guildId);

        dpp::snowflake textChannelId(0);
        bool shouldLeave = false;
        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            auto it = guildInfos.find(target.guildId);
            if (it == guildInfos.end() || it->second.leavePending) continue;
            GuildInfo& info = it->second;

            // 상태를 알 수 없거나(스냅샷 전), 봇 자신이 아직 안 보이거나(접속 직후), 사람이 있으면 초기화.
            if (!obs.known || !obs.botPresent || obs.humans > 0) {
                info.emptyTracking = false;
                continue;
            }

            if (!info.emptyTracking) {
                info.emptyTracking = true;
                info.emptySince = now;
                continue;
            }

            if (now - info.emptySince >= EMPTY_CHANNEL_GRACE) {
                info.leavePending = true;
                textChannelId = info.lastTextChannelId;
                shouldLeave = true;
            }
        }

        if (shouldLeave) {
            std::cout << "[CheckVoiceChannels] 봇 외에 아무도 없어서 퇴장 (guild " << target.guildId << ")" << std::endl;
            const dpp::snowflake guildId = target.guildId;
            const uint32_t shardId = target.shardId;
            std::thread([this, guildId, shardId, textChannelId]() {
                LeaveVoice(guildId, shardId);
                if (textChannelId != dpp::snowflake(0)) {
                    SendBotMessage(textChannelId, "음성 채널에 아무도 없어서 나갈게요.");
                }
            }).detach();
        }
    }
}

// ================= 오디오 재생 스레드 =================

void DiscordBotClient::PlayAudioThread(dpp::snowflake guildId) {
    std::cout << "[PlayAudioThread] Started for guild " << guildId << std::endl;

    while (true) {
        GuildInfo* guildInfo = nullptr;
        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            auto it = guildInfos.find(guildId);
            if (it == guildInfos.end() || it->second.shouldStop.load()) {
                break;
            }
            guildInfo = &it->second;
        }

        // shard 객체는 게이트웨이 재연결(resume) 때 파괴/재생성되므로 매번 ID로 다시 조회한다.
        const uint32_t shardId = guildInfo->shardId;
        dpp::discord_client* shard = bot.get_shard(shardId);
        dpp::voiceconn* vconn = shard ? shard->get_voice(guildId) : nullptr;
        dpp::discord_voice_client* v = (vconn && vconn->voiceclient) ? vconn->voiceclient.get() : nullptr;

        if (!vconn || !v || !v->is_ready()) {
            // 음성 연결이 끊겼는데 자동으로 복구가 안 되는 경우(게이트웨이 재연결 여파 등)를
            // 대비해, 5초 이상 끊긴 상태가 지속되면 마지막으로 접속해있던 채널로 재접속을
            // 시도한다. 그전엔 정상적인 초기 연결 과정으로 오해해 재시도하지 않는다.
            auto now = std::chrono::steady_clock::now();
            if (shard && guildInfo->voiceChannelId != dpp::snowflake(0) &&
                now - guildInfo->lastReconnectAttempt > std::chrono::seconds(5)) {
                guildInfo->lastReconnectAttempt = now;
                std::cout << "[PlayAudioThread] 음성 연결 끊김 감지, 채널 " << guildInfo->voiceChannelId
                          << " 재접속 시도 (guild " << guildId << ")" << std::endl;
                shard->connect_voice(guildId, guildInfo->voiceChannelId);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        bool hasVideo = false;
        std::string filePathToPlay;
        {
            std::lock_guard<std::mutex> lock(guildInfosMutex);
            auto it = guildInfos.find(guildId);
            if (it == guildInfos.end() || it->second.shouldStop.load()) {
                break;
            }
            guildInfo = &it->second;

            if (guildInfo->currentPlay.filePath.empty()) {
                std::lock_guard<std::mutex> listLock(guildInfo->listMutex);
                if (!guildInfo->videoLists.empty()) {
                    guildInfo->currentPlay = guildInfo->videoLists.front();
                    guildInfo->videoLists.erase(guildInfo->videoLists.begin());
                    hasVideo = true;
                }
            } else {
                hasVideo = true;
            }
            if (hasVideo) {
                filePathToPlay = guildInfo->currentPlay.filePath;
            }
        }

        if (hasVideo && !filePathToPlay.empty()) {
            std::cout << "[PlayAudioThread] Playing: " << filePathToPlay << std::endl;

            FILE* pipe = FileLoader::GetAudioStream(filePathToPlay);
            bool finishedNaturally = false;
            if (pipe) {
                std::vector<uint8_t> buffer(FRAME_SIZE);
                uint32_t waitTime = 0;

                while (true) {
                    if (guildInfo->shouldStop.load()) break;

                    if (guildInfo->skipRequested.load()) {
                        std::lock_guard<std::mutex> lock(guildInfosMutex);
                        auto it = guildInfos.find(guildId);
                        if (it != guildInfos.end()) it->second.skipRequested = false;
                        break;
                    }

                    size_t bytesRead = fread(buffer.data(), 1, buffer.size(), pipe);
                    if (bytesRead == 0) {
                        finishedNaturally = true;
                        break;
                    }
                    if (bytesRead % 2 != 0) bytesRead--;

                    // 매 프레임마다 다시 조회한다. dpp가 내부적으로 음성 재연결(voiceconn 교체)을
                    // 하는 동안 바깥에서 캐싱해둔 포인터를 계속 쓰면 use-after-free로 세그폴트가
                    // 난다 - 실제로 "Starting a full reconnection" 로그 직후 크래시가 발생했었음.
                    dpp::discord_client* liveShard = bot.get_shard(shardId);
                    dpp::voiceconn* liveVconn = liveShard ? liveShard->get_voice(guildId) : nullptr;
                    dpp::discord_voice_client* liveV = (liveVconn && liveVconn->voiceclient) ? liveVconn->voiceclient.get() : nullptr;
                    if (!liveV || !liveVconn || !liveV->is_ready()) break;

                    try {
                        liveV->send_audio_raw(reinterpret_cast<uint16_t*>(buffer.data()), bytesRead);
                    } catch (const std::exception& e) {
                        std::cerr << "[PlayAudioThread] send_audio_raw 예외: " << e.what() << std::endl;
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(SEND_INTERVAL_MS));
                    waitTime += LEAD_PER_FRAME_MS;
                }

                // 남은 오디오가 다 재생되도록 기다린다. 퇴장/정지 중이면 연결을 곧 끊을 거라 필요 없고,
                // 이 대기가 StopAudioThread의 join을 곡 후반일수록 몇 초씩 막았다.
                if (!guildInfo->shouldStop.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(waitTime));
                }
                FileLoader::CloseAudioStream(pipe, filePathToPlay);
                FileLoader::Cleanup(filePathToPlay);
            } else {
                std::cerr << "[PlayAudioThread] 파일 열기 실패: " << filePathToPlay << std::endl;
            }

            // 자연 종료(끝까지 다 재생)이고 반복 모드면 currentPlay를 그대로 둬서 다음 바퀴에 같은 파일을 다시 연다.
            // 스킵/정지/에러로 끝난 경우는 반복 여부와 무관하게 항상 다음 곡으로 넘어간다.
            bool shouldRepeat = finishedNaturally && guildInfo->repeatCurrent.load();
            if (!shouldRepeat) {
                std::lock_guard<std::mutex> lock(guildInfosMutex);
                auto it = guildInfos.find(guildId);
                if (it != guildInfos.end()) {
                    it->second.currentPlay = VideoDbInfo();
                }
            }
        } else {
            if (v && vconn && v->is_ready()) {
                try {
                    v->send_silence(SILENCE_DURATION_MS);
                } catch (const std::exception& e) {
                    std::cerr << "[PlayAudioThread] send_silence 예외: " << e.what() << std::endl;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SILENCE_DURATION_MS));
        }
    }

    std::cout << "[PlayAudioThread] Exited for guild " << guildId << std::endl;
}

void DiscordBotClient::StartAudioThread(dpp::snowflake guildId) {
    std::lock_guard<std::mutex> lock(guildInfosMutex);
    auto it = guildInfos.find(guildId);
    if (it == guildInfos.end() || it->second.audioThread.joinable()) {
        return;
    }
    it->second.shouldStop = false;
    it->second.audioThread = std::thread(&DiscordBotClient::PlayAudioThread, this, guildId);
}

void DiscordBotClient::StopAudioThread(dpp::snowflake guildId) {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(guildInfosMutex);
        auto it = guildInfos.find(guildId);
        if (it != guildInfos.end()) {
            it->second.shouldStop = true;
            if (it->second.audioThread.joinable()) {
                threadToJoin = std::move(it->second.audioThread);
            }
        }
    }
    if (threadToJoin.joinable()) {
        threadToJoin.join();
    }
}

// ================= 메시지 유틸 =================

// 현재 채널의 최근 메시지를 count개 지운다 (요청한 !clean 메시지 자신도 포함해서 지워짐).
// count는 최대 10개까지만 허용 (그 이상이면 삭제하지 않고 안내 메시지만 보냄).
// Discord API 제약: 2주보다 오래된 메시지는 벌크 삭제할 수 없다.
void DiscordBotClient::HandleCleanCommand(const std::string& args, const dpp::message_create_t& event) {
    int count = 10;
    if (!args.empty()) {
        try {
            count = std::stoi(args);
        } catch (...) {
            count = 10;
        }
    }
    if (count <= 0) count = 10;

    dpp::snowflake channelId = event.msg.channel_id;

    if (count > 10) {
        SendBotMessage(channelId, "10개 이상은 삭제할 수 없습니다.");
        return;
    }

    bot.messages_get(channelId, 0, 0, 0, static_cast<uint64_t>(count),
        [this, channelId](const dpp::confirmation_callback_t& callback) {
            if (callback.is_error()) {
                std::cerr << "[HandleCleanCommand] messages_get 실패: " << callback.get_error().message << std::endl;
                SendBotMessage(channelId, "메시지를 가져오는 데 실패했습니다 (권한 부족일 수 있음).");
                return;
            }

            auto messages = std::get<dpp::message_map>(callback.value);
            if (messages.empty()) {
                return;
            }

            std::vector<dpp::snowflake> ids;
            ids.reserve(messages.size());
            for (const auto& entry : messages) {
                ids.push_back(entry.first);
            }

            for (size_t offset = 0; offset < ids.size(); offset += 100) {
                size_t end = std::min(offset + static_cast<size_t>(100), ids.size());
                std::vector<dpp::snowflake> chunk(ids.begin() + offset, ids.begin() + end);

                if (chunk.size() == 1) {
                    bot.message_delete(chunk[0], channelId, [](const dpp::confirmation_callback_t&) {});
                } else {
                    bot.message_delete_bulk(chunk, channelId, [channelId](const dpp::confirmation_callback_t& cb) {
                        if (cb.is_error()) {
                            // 2주보다 오래된 메시지가 섞여있으면 여기서 실패할 수 있음 - 로그만 남김
                            std::cerr << "[HandleCleanCommand] bulk delete 실패 (channel " << channelId
                                      << "): " << cb.get_error().message << std::endl;
                        }
                    });
                }
            }

            std::cout << "[HandleCleanCommand] " << ids.size() << "개 메시지 삭제 요청 (channel " << channelId << ")" << std::endl;
        }
    );
}

void DiscordBotClient::SendBotMessage(const dpp::snowflake& msgChannelId, const std::string& message) {
    dpp::snowflake lastMessageId = 0;
    {
        std::lock_guard<std::mutex> lock(lastBotMessagesMutex);
        auto it = lastBotMessages.find(msgChannelId);
        if (it != lastBotMessages.end()) {
            lastMessageId = it->second;
        }
    }

    if (lastMessageId != 0) {
        bot.message_delete(lastMessageId, msgChannelId, [](const dpp::confirmation_callback_t&) {});
    }

    bot.message_create(dpp::message(msgChannelId, message),
        [this, msgChannelId](const dpp::confirmation_callback_t& callback) {
            if (!callback.is_error()) {
                dpp::message msg = std::get<dpp::message>(callback.value);
                std::lock_guard<std::mutex> lock(lastBotMessagesMutex);
                lastBotMessages[msgChannelId] = msg.id;
            }
        }
    );
}

void DiscordBotClient::DeleteMessage(const dpp::snowflake& msgId, const dpp::snowflake& msgChannelId) {
    bot.message_delete(msgId, msgChannelId, [](const dpp::confirmation_callback_t&) {});
}
