#ifdef _WIN32
#pragma warning(disable: 4819)
#endif
#include "JsonReader.h"
#include "DiscordBotClient.h"
#include <iostream>
#ifdef _WIN32
#include <Windows.h>
#endif

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        std::string token = JsonReader::GetDiscordToken();
        if (token.empty()) {
            std::cerr << "Discord token not found." << std::endl;
            return 1;
        }

        DiscordBotClient client(token);
        client.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cerr << "Unknown error occurred." << std::endl;
        return 1;
    }

    return 0;
}
