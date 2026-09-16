#include "FileLoader.h"
#include <iostream>
#include <filesystem>
#include <cctype>
#include <cstdio>

namespace fs = std::filesystem;

namespace {
    constexpr const char* SERVER_BASE_URL = "http://127.0.0.1:8080";
}

std::string FileLoader::UrlEncode(const std::string& value) {
    std::string encoded;
    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

void FileLoader::RequestServer(dpp::cluster& bot, const std::string& path,
                                std::function<void(bool, nlohmann::json, std::string)> callback) {
    std::string url = std::string(SERVER_BASE_URL) + path;

    bot.request(url, dpp::m_get, [callback](const dpp::http_request_completion_t& response) {
        if (response.status < 200 || response.status >= 300) {
            callback(false, nlohmann::json(), "서버 응답 실패 (status " + std::to_string(response.status) + ")");
            return;
        }
        if (response.body.empty()) {
            callback(false, nlohmann::json(), "서버 응답이 비어있습니다.");
            return;
        }

        try {
            callback(true, nlohmann::json::parse(response.body), "");
        } catch (const std::exception& e) {
            callback(false, nlohmann::json(), std::string("JSON 파싱 실패: ") + e.what());
        }
    });
}

VideoDbInfo FileLoader::ParseVideoDbInfo(const nlohmann::json& item) {
    VideoDbInfo info;
    info.success = true;
    info.id = item.value("id", "");
    info.title = item.value("title", "");
    info.channelId = item.value("channelId", "");
    info.channelTitle = item.value("channelTitle", "");
    info.thumbnail = item.value("thumbnail", "");
    info.duration = item.value("duration", 0);

    if (item.contains("localPath")) {
        info.filePath = item["localPath"].get<std::string>();
    } else if (item.contains("local_path")) {
        info.filePath = item["local_path"].get<std::string>();
    }
    return info;
}

FILE* FileLoader::GetAudioStream(const std::string& path) {
    fs::path p(path);
    if (!fs::exists(p)) {
        std::cerr << "[FileLoader] 파일을 찾을 수 없음: " << path << std::endl;
        return nullptr;
    }

#ifdef _WIN32
    return _wfopen(p.wstring().c_str(), L"rb");
#else
    return fopen(p.c_str(), "rb");
#endif
}

void FileLoader::CloseAudioStream(FILE* stream, const std::string& /*path*/) {
    if (stream) {
        fclose(stream);
    }
}

void FileLoader::Cleanup(const std::string& /*path*/) {
    // 서버가 db/ 디렉토리에서 파일을 영구 관리하므로 클라이언트에서는 삭제하지 않는다.
}
