#pragma once
#include <string>
#include <fstream>
#include <dpp/json.h>
#include <filesystem>

class JsonReader {
public:
    static std::string GetDiscordToken() {
        return GetToken("discord_token");
    }

private:
    static nlohmann::json& GetConfig() {
        static nlohmann::json data;
        static bool loaded = false;

        if (!loaded) {
            auto path = std::filesystem::current_path() / "config.json";
            std::ifstream file(path);
            if (!file.is_open()) {
                throw std::runtime_error("config.json not found");
            }
            file >> data;
            loaded = true;
        }
        return data;
    }

    static std::string GetToken(const std::string& key) {
        auto& data = GetConfig();
        if (data.contains(key)) {
            return data[key].get<std::string>();
        }
        return "";
    }
};
