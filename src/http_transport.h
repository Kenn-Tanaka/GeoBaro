#pragma once
#include <string>
#include <vector>
#include <functional>
#include <windows.h>
#include <wininet.h>

class HttpTransport {
public:
    static std::string GetString(
        const std::wstring& url,
        const std::function<bool()>& should_cancel = nullptr
    );
    static std::vector<unsigned char> GetBinary(
        const std::wstring& url,
        const std::function<bool()>& should_cancel = nullptr
    );
    static std::vector<std::string> GetStrings(
        const std::vector<std::wstring>& urls,
        const std::function<bool()>& should_cancel = nullptr
    );
    static std::vector<std::vector<unsigned char>> GetBinaries(
        const std::vector<std::wstring>& urls,
        const std::function<bool()>& should_cancel = nullptr
    );

private:
    static HINTERNET CreateSession();
    static std::vector<unsigned char> FetchWithSession(
        HINTERNET hInternet,
        const std::wstring& url,
        const std::function<bool()>& should_cancel = nullptr
    );
};
