#include "http_transport.h"

HINTERNET HttpTransport::CreateSession() {
    HINTERNET hInternet = InternetOpenW(
        L"GeoBaro/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        NULL,
        NULL,
        0
    );
    if (!hInternet) {
        return NULL;
    }

    // 接続・送受信タイムアウトを設定 (接続5秒 / 送受信8秒)
    DWORD connect_timeout_ms = 5000;
    DWORD io_timeout_ms = 8000;
    InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &connect_timeout_ms, sizeof(connect_timeout_ms));
    InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &io_timeout_ms, sizeof(io_timeout_ms));
    InternetSetOptionW(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &io_timeout_ms, sizeof(io_timeout_ms));
    return hInternet;
}

std::vector<unsigned char> HttpTransport::FetchWithSession(
    HINTERNET hInternet,
    const std::wstring& url,
    const std::function<bool()>& should_cancel
) {
    std::vector<unsigned char> result;
    if (!hInternet || (should_cancel && should_cancel())) {
        return result;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (url.rfind(L"https://", 0) == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    HINTERNET hFile = InternetOpenUrlW(
        hInternet,
        url.c_str(),
        NULL,
        0,
        flags,
        0
    );

    if (hFile) {
        DWORD statusCode = 0;
        DWORD statusLen = sizeof(statusCode);
        bool ok = HttpQueryInfoW(
            hFile,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &statusCode,
            &statusLen,
            NULL
        );

        if (ok && statusCode >= 200 && statusCode < 300) {
            DWORD contentLength = 0;
            DWORD contentLengthLen = sizeof(contentLength);
            bool hasContentLength = HttpQueryInfoW(
                hFile,
                HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                &contentLength,
                &contentLengthLen,
                NULL
            ) == TRUE;
            if (hasContentLength && contentLength > 0 && contentLength < (64u * 1024u * 1024u)) {
                result.reserve(contentLength);
            }

            unsigned char buffer[4096];
            bool read_ok = true;
            for (;;) {
                if (should_cancel && should_cancel()) {
                    read_ok = false;
                    break;
                }
                DWORD bytesRead = 0;
                if (!InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead)) {
                    read_ok = false;
                    break;
                }
                if (bytesRead == 0) {
                    break;
                }
                result.insert(result.end(), buffer, buffer + bytesRead);
            }

            if (!read_ok || (hasContentLength && result.size() != static_cast<size_t>(contentLength))) {
                result.clear();
            }
        }
        InternetCloseHandle(hFile);
    }

    return result;
}

std::string HttpTransport::GetString(
    const std::wstring& url,
    const std::function<bool()>& should_cancel
) {
    if (should_cancel && should_cancel()) {
        return "";
    }
    HINTERNET hInternet = CreateSession();
    if (!hInternet) {
        return "";
    }
    std::vector<unsigned char> bytes = FetchWithSession(hInternet, url, should_cancel);
    InternetCloseHandle(hInternet);
    if (bytes.empty()) {
        return "";
    }
    return std::string(reinterpret_cast<char*>(bytes.data()), bytes.size());
}

std::vector<unsigned char> HttpTransport::GetBinary(
    const std::wstring& url,
    const std::function<bool()>& should_cancel
) {
    if (should_cancel && should_cancel()) {
        return {};
    }
    HINTERNET hInternet = CreateSession();
    if (!hInternet) {
        return {};
    }
    std::vector<unsigned char> bytes = FetchWithSession(hInternet, url, should_cancel);
    InternetCloseHandle(hInternet);
    return bytes;
}

std::vector<std::string> HttpTransport::GetStrings(
    const std::vector<std::wstring>& urls,
    const std::function<bool()>& should_cancel
) {
    std::vector<std::string> results(urls.size());
    if (urls.empty() || (should_cancel && should_cancel())) {
        return results;
    }
    HINTERNET hInternet = CreateSession();
    if (!hInternet) {
        return results;
    }

    for (size_t i = 0; i < urls.size(); ++i) {
        if (should_cancel && should_cancel()) {
            break;
        }
        std::vector<unsigned char> bytes = FetchWithSession(hInternet, urls[i], should_cancel);
        if (!bytes.empty()) {
            results[i].assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }

    // 一部チャンク取得に失敗した場合は未取得分のみ1回リトライ
    for (size_t i = 0; i < urls.size(); ++i) {
        if (!results[i].empty()) continue;
        if (should_cancel && should_cancel()) {
            break;
        }
        Sleep(120);
        if (should_cancel && should_cancel()) {
            break;
        }
        std::vector<unsigned char> bytes = FetchWithSession(hInternet, urls[i], should_cancel);
        if (!bytes.empty()) {
            results[i].assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }

    InternetCloseHandle(hInternet);
    return results;
}

std::vector<std::vector<unsigned char>> HttpTransport::GetBinaries(
    const std::vector<std::wstring>& urls,
    const std::function<bool()>& should_cancel
) {
    std::vector<std::vector<unsigned char>> results(urls.size());
    if (urls.empty() || (should_cancel && should_cancel())) {
        return results;
    }
    HINTERNET hInternet = CreateSession();
    if (!hInternet) {
        return results;
    }

    for (size_t i = 0; i < urls.size(); ++i) {
        if (should_cancel && should_cancel()) {
            break;
        }
        results[i] = FetchWithSession(hInternet, urls[i], should_cancel);
    }

    // 一部タイルの取得に失敗した場合は未取得分のみ1回リトライ
    for (size_t i = 0; i < urls.size(); ++i) {
        if (!results[i].empty()) continue;
        if (should_cancel && should_cancel()) {
            break;
        }
        Sleep(80);
        if (should_cancel && should_cancel()) {
            break;
        }
        results[i] = FetchWithSession(hInternet, urls[i], should_cancel);
    }

    InternetCloseHandle(hInternet);
    return results;
}
