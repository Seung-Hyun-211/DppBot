#pragma once
#include <string>
#include <cstdio>
#include <functional>
#include <dpp/dpp.h>

struct VideoDbInfo {
    std::string id;
    std::string title;
    std::string channelId;
    std::string channelTitle;
    std::string thumbnail;
    std::string filePath;
    int duration = 0;
    std::string error;
    bool success = false;
};

// Go-Local 서버(127.0.0.1:8080)와의 통신 + 로컬 PCM 파일 접근을 담당한다.
// 서버 호출은 전부 dpp::cluster::request()를 통한 비동기 HTTP로 처리하며,
// 셸을 거치는 프로세스 실행(popen 등)은 쓰지 않는다.
class FileLoader {
public:
    // path는 "/process?q=..." 형태의 경로+쿼리. 서버 베이스 URL은 내부에서 붙인다.
    // 콜백은 dpp 내부 워커 스레드에서 호출되므로, 콜백 안에서 guildInfosMutex 등을
    // 짧게만 잠그고 블로킹 작업을 하지 않아야 한다.
    static void RequestServer(dpp::cluster& bot, const std::string& path,
                               std::function<void(bool ok, nlohmann::json body, std::string error)> callback);

    // 쿼리 문자열을 URL 인코딩한다 (공백은 '+').
    static std::string UrlEncode(const std::string& value);

    // 서버 응답의 아이템(단일 metadata 객체 또는 list/items 배열의 원소)을 VideoDbInfo로 변환한다.
    // localPath/local_path 키가 없으면 filePath는 비워둔다.
    static VideoDbInfo ParseVideoDbInfo(const nlohmann::json& item);

    // .pcm 파일을 열어 읽기용 FILE*을 반환한다. 없으면 nullptr.
    static FILE* GetAudioStream(const std::string& path);
    static void CloseAudioStream(FILE* stream, const std::string& path);

    // 서버가 파일을 영구 저장소(db/)에서 관리하므로 클라이언트는 정리하지 않는다.
    static void Cleanup(const std::string& path);
};
