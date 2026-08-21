#include "OpeningFetcher.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <array>
#include <vector>
#include <algorithm>
#include <cctype>
#include <libchess/position.hpp>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace cta {
namespace {

std::string url_encode_query_value(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return encoded;
}

std::string trim_setting_value(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string load_configured_api_token(const std::filesystem::path& cache_path) {
    const std::array<std::filesystem::path, 2> settings_paths = {
        cache_path.parent_path() / "ChessTubeAnalyzer.ini",
        cache_path.parent_path() / "settings.ini",
    };

    for (const auto& settings_path : settings_paths) {
        std::ifstream settings(settings_path);
        if (!settings) continue;

        std::string line;
        while (std::getline(settings, line)) {
            const size_t separator = line.find('=');
            if (separator == std::string::npos) continue;
            if (trim_setting_value(line.substr(0, separator)) != "lichessToken") continue;
            const std::string token = trim_setting_value(line.substr(separator + 1));
            if (!token.empty()) return token;
        }
    }
    return {};
}

void set_winhttp_error(LichessOpening& result, const char* operation) {
#ifdef _WIN32
    result.winhttp_error = GetLastError();
    std::ostringstream oss;
    oss << operation << " failed";
    if (result.winhttp_error != 0) {
        oss << " (WinHTTP error " << result.winhttp_error << ")";
    }
    result.error = oss.str();
#else
    result.error = operation;
#endif
}

#ifdef _WIN32
void configure_winhttp_timeouts(HINTERNET handle) {
    // Metadata is optional application enrichment, but a stalled request must
    // never hold the extraction pipeline indefinitely.
    constexpr int timeout_ms = 5000;
    WinHttpSetTimeouts(handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
}

struct WinHttpTextResponse {
    DWORD status = 0;
    DWORD error = ERROR_SUCCESS;
    std::string body;
};

WinHttpTextResponse fetch_https_text(
    const wchar_t* host,
    const std::string& path,
    const wchar_t* accept_type,
    const std::string& token) {
    WinHttpTextResponse response;
    HINTERNET session = WinHttpOpen(
        L"ChessTubeAnalyzer-Extract/1.0 (local extraction pipeline)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        response.error = GetLastError();
        return response;
    }
    configure_winhttp_timeouts(session);
    DWORD connect_retries = 1;
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_RETRIES,
                     &connect_retries, sizeof(connect_retries));
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP |
                          WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION,
                     &decompression, sizeof(decompression));
#endif

    HINTERNET connection = WinHttpConnect(
        session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        response.error = GetLastError();
        WinHttpCloseHandle(session);
        return response;
    }

    const int wide_length = MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wide_path(wide_length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                        wide_path.data(), wide_length);

    LPCWSTR accept_types[] = {accept_type, nullptr};
    HINTERNET request = WinHttpOpenRequest(
        connection, L"GET", wide_path.c_str(), nullptr, WINHTTP_NO_REFERER,
        accept_types, WINHTTP_FLAG_SECURE);
    if (!request) {
        response.error = GetLastError();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }
    configure_winhttp_timeouts(request);

    std::wstring headers;
    if (!token.empty()) {
        const std::string auth = "Authorization: Bearer " + token + "\r\n";
        const int header_length = MultiByteToWideChar(
            CP_UTF8, 0, auth.c_str(), -1, nullptr, 0);
        headers.resize(header_length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, auth.c_str(), -1, headers.data(), header_length);
        headers.resize(static_cast<size_t>(header_length - 1));
    }

    if (!WinHttpSendRequest(
            request,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        response.error = GetLastError();
    } else {
        DWORD status_size = sizeof(response.status);
        if (!WinHttpQueryHeaders(
                request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &status_size,
                WINHTTP_NO_HEADER_INDEX)) {
            response.error = GetLastError();
        } else if (response.status == 200) {
            while (true) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    response.error = GetLastError();
                    break;
                }
                if (available == 0) break;
                std::vector<char> buffer(available);
                DWORD downloaded = 0;
                if (!WinHttpReadData(request, buffer.data(), available, &downloaded)) {
                    response.error = GetLastError();
                    break;
                }
                response.body.append(buffer.data(), downloaded);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}
#endif

LichessGameMetadata parse_master_game_headers(
    const std::string& pgn,
    const std::string& game_id) {
    LichessGameMetadata metadata;
    metadata.id = game_id;

    std::istringstream stream(pgn);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 5 || line.front() != '[') continue;

        const size_t separator = line.find(" \"");
        const size_t value_end = line.rfind("\"]");
        if (separator == std::string::npos || value_end == std::string::npos ||
            value_end <= separator + 2) {
            continue;
        }

        const std::string key = line.substr(1, separator - 1);
        const std::string value = line.substr(separator + 2, value_end - separator - 2);
        if (key == "Event") metadata.event = value;
        else if (key == "Site") metadata.site = value;
        else if (key == "Date") metadata.date = value;
        else if (key == "Round") metadata.round = value;
        else if (key == "White") metadata.white = value;
        else if (key == "Black") metadata.black = value;
        else if (key == "Result") metadata.result = value;
        else if (key == "WhiteElo") metadata.white_elo = value;
        else if (key == "BlackElo") metadata.black_elo = value;
        else if (key == "ECO") metadata.eco = value;
        else if (key == "Opening") metadata.opening = value;
    }

    metadata.found = !metadata.date.empty() && !metadata.white.empty() &&
                     !metadata.black.empty() && !metadata.result.empty();
    return metadata;
}

LichessGameMetadata parse_master_game_summary(const nlohmann::json& game) {
    LichessGameMetadata metadata;
    metadata.id = game.value("id", "");
    const std::string winner = game.contains("winner") && game["winner"].is_string()
        ? game["winner"].get<std::string>() : "";
    metadata.result = winner == "white" ? "1-0"
        : winner == "black" ? "0-1" : "1/2-1/2";
    if (game.contains("white") && game["white"].is_object()) {
        metadata.white = game["white"].value("name", "");
        if (game["white"].contains("rating") && game["white"]["rating"].is_number()) {
            metadata.white_elo = std::to_string(game["white"].value("rating", 0));
        }
    }
    if (game.contains("black") && game["black"].is_object()) {
        metadata.black = game["black"].value("name", "");
        if (game["black"].contains("rating") && game["black"]["rating"].is_number()) {
            metadata.black_elo = std::to_string(game["black"].value("rating", 0));
        }
    }
    return metadata;
}

std::array<char, 64> expand_fen_board(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int square = 56;
    for (const char character : fen) {
        if (character == ' ') break;
        if (character == '/') square -= 16;
        else if (character >= '1' && character <= '8') square += character - '0';
        else board[square++] = character;
    }
    return board;
}

std::string normalize_san(std::string san) {
    san.erase(std::remove(san.begin(), san.end(), '+'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '#'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '!'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '?'), san.end());
    const size_t en_passant = san.find("e.p.");
    if (en_passant != std::string::npos) san.erase(en_passant, 4);
    if (san == "0-0") return "O-O";
    if (san == "0-0-0") return "O-O-O";
    return san;
}

std::string build_san_for_move(
    const libchess::Position& position,
    const libchess::Move& move,
    std::string uci) {
    const int from = static_cast<int>(static_cast<unsigned int>(move.from()));
    const int to = static_cast<int>(static_cast<unsigned int>(move.to()));
    const std::array<char, 64> board = expand_fen_board(position.get_fen());
    const char piece = board[from];
    const char target = board[to];
    const bool pawn = piece == 'P' || piece == 'p';
    const bool capture = target != ' ' ||
        (pawn && from % 8 != to % 8 && target == ' ');

    if (move.type() == libchess::MoveType::ksc) return "O-O";
    if (move.type() == libchess::MoveType::qsc) return "O-O-O";

    std::string san;
    if (!pawn) {
        san += static_cast<char>(std::toupper(static_cast<unsigned char>(piece)));
        bool file_conflict = false;
        bool rank_conflict = false;
        bool disambiguate = false;
        for (const auto& alternative : position.legal_moves()) {
            const int alternative_from = static_cast<int>(
                static_cast<unsigned int>(alternative.from()));
            const int alternative_to = static_cast<int>(
                static_cast<unsigned int>(alternative.to()));
            if (alternative_from != from && alternative_to == to &&
                board[alternative_from] == piece) {
                disambiguate = true;
                file_conflict |= alternative_from % 8 == from % 8;
                rank_conflict |= alternative_from / 8 == from / 8;
            }
        }
        if (disambiguate) {
            if (!file_conflict) san += static_cast<char>('a' + from % 8);
            else if (!rank_conflict) san += static_cast<char>('1' + from / 8);
            else {
                san += static_cast<char>('a' + from % 8);
                san += static_cast<char>('1' + from / 8);
            }
        }
    } else if (capture) {
        san += static_cast<char>('a' + from % 8);
    }

    if (capture) san += 'x';
    san += static_cast<char>('a' + to % 8);
    san += static_cast<char>('1' + to / 8);
    if (uci.size() >= 5) {
        san += '=';
        san += static_cast<char>(std::toupper(static_cast<unsigned char>(uci[4])));
    }

    libchess::Position next = position;
    next.makemove(move);
    if (next.is_checkmate()) san += '#';
    else if (next.in_check()) san += '+';
    return san;
}

bool apply_pgn_move(libchess::Position& position, const std::string& token) {
    try {
        position.makemove(position.parse_move(token));
        return true;
    } catch (...) {
    }

    std::string normalized = normalize_san(token);
    if (normalized == "O-O" || normalized == "O-O-O") {
        const bool white_to_move = position.get_fen().find(" w ") != std::string::npos;
        if (normalized == "O-O") normalized = white_to_move ? "e1g1" : "e8g8";
        else normalized = white_to_move ? "e1c1" : "e8c8";
    }

    try {
        position.makemove(position.parse_move(normalized));
        return true;
    } catch (...) {
    }

    const std::string wanted_san = normalize_san(token);
    for (const auto& legal_move : position.legal_moves()) {
        std::string uci = static_cast<std::string>(legal_move);
        if (uci == "e1h1") uci = "e1g1";
        else if (uci == "e1a1") uci = "e1c1";
        else if (uci == "e8h8") uci = "e8g8";
        else if (uci == "e8a8") uci = "e8c8";
        if (normalize_san(build_san_for_move(position, legal_move, uci)) == wanted_san) {
            position.makemove(legal_move);
            return true;
        }
    }
    return false;
}

std::vector<std::string> parse_master_game_fens(const std::string& pgn) {
    std::vector<std::string> fens;
    libchess::Position position(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    fens.push_back(position.get_fen());

    std::string movetext;
    bool in_header = false;
    int comment_depth = 0;
    int variation_depth = 0;
    for (const char character : pgn) {
        if (character == '[' && comment_depth == 0) {
            in_header = true;
            continue;
        }
        if (in_header && character == ']') {
            in_header = false;
            continue;
        }
        if (in_header) continue;
        if (character == '{') {
            ++comment_depth;
            continue;
        }
        if (character == '}' && comment_depth > 0) {
            --comment_depth;
            continue;
        }
        if (comment_depth > 0) continue;
        if (character == '(') {
            ++variation_depth;
            continue;
        }
        if (character == ')' && variation_depth > 0) {
            --variation_depth;
            continue;
        }
        if (variation_depth == 0) movetext.push_back(character);
    }

    std::istringstream tokens(movetext);
    std::string token;
    while (tokens >> token) {
        while (!token.empty() &&
               ((token.front() >= '0' && token.front() <= '9') || token.front() == '.')) {
            token.erase(token.begin());
        }
        if (token.empty() || token == "*" || token == "1-0" || token == "0-1" ||
            token == "1/2-1/2" || token.front() == '$') {
            continue;
        }

        if (!apply_pgn_move(position, token)) break;
        fens.push_back(position.get_fen());
    }
    return fens;
}

bool parse_explorer_response(
    const std::string& response_body,
    nlohmann::json& response) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(response_body);
        if (parsed.is_object()) {
            response = parsed;
            return true;
        }
    } catch (...) {
        // The masters endpoint may stream newline-delimited JSON rather than
        // returning one JSON document. Try each complete line below.
    }

    bool found = false;
    nlohmann::json latest_response;
    nlohmann::json named_opening;
    bool found_named_opening = false;
    std::istringstream lines(response_body);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        try {
            const nlohmann::json parsed = nlohmann::json::parse(line);
            if (!parsed.is_object()) continue;
            if (parsed.contains("white") || parsed.contains("moves") ||
                parsed.contains("opening") || parsed.contains("topGames")) {
                latest_response = parsed;
                found = true;
                if (parsed.contains("opening") && parsed["opening"].is_object() &&
                    !parsed["opening"].value("eco", "").empty() &&
                    !parsed["opening"].value("name", "").empty()) {
                    named_opening = parsed["opening"];
                    found_named_opening = true;
                }
            }
        } catch (...) {
            // Ignore incomplete or non-JSON lines and keep looking for a
            // complete explorer response.
        }
    }
    if (found) {
        response = latest_response;
        // Later streamed updates can omit the opening object even though an
        // earlier update identified the named position.
        if (found_named_opening &&
            (!response.contains("opening") || response["opening"].is_null())) {
            response["opening"] = named_opening;
        }
    }
    return found;
}

bool contains_fen_sequence(
    const std::vector<std::string>& game_fens,
    const std::vector<std::string>& observed_fens) {
    if (observed_fens.empty() || game_fens.size() < observed_fens.size()) {
        return false;
    }

    // The halfmove and fullmove counters are bookkeeping fields, not board
    // identity. They can differ when the video begins from a position whose
    // imported PGN has a different history, while the first four FEN fields
    // still prove that the same game state was observed.
    const auto position_key = [](const std::string& fen) {
        std::istringstream fields(fen);
        std::string key;
        std::string field;
        for (int index = 0; index < 4 && fields >> field; ++index) {
            if (!key.empty()) key.push_back(' ');
            key += field;
        }
        return key;
    };

    std::vector<std::string> game_position_keys;
    std::vector<std::string> observed_position_keys;
    game_position_keys.reserve(game_fens.size());
    observed_position_keys.reserve(observed_fens.size());
    for (const std::string& fen : game_fens) {
        game_position_keys.push_back(position_key(fen));
    }
    for (const std::string& fen : observed_fens) {
        observed_position_keys.push_back(position_key(fen));
    }

    return std::search(
               game_position_keys.begin(), game_position_keys.end(),
               observed_position_keys.begin(), observed_position_keys.end()) !=
           game_position_keys.end();
}

} // namespace

OpeningFetcher::OpeningFetcher() : stop_(false), unique_game_reached_(false), done_(false) {
    auto cache_path = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer" / "openings_cache.json";
#ifdef _WIN32
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr) {
        cache_path = std::filesystem::path(appdata) / "ChessTubeAnalyzer" / "openings_cache.json";
        free(appdata);
    }
#endif
    cache_path_ = cache_path.string();
    api_token_ = load_configured_api_token(cache_path);
    if (std::filesystem::exists(cache_path_)) {
        try {
            std::ifstream ifs(cache_path_);
            nlohmann::json j;
            ifs >> j;
            const nlohmann::json& position_entries =
                j.contains("positions") && j["positions"].is_object()
                    ? j["positions"] : j;
            if (j.contains("master_games") && j["master_games"].is_object()) {
                for (const auto& game : j["master_games"].items()) {
                    if (!game.value().is_array()) continue;
                    master_game_fens_[game.key()] =
                        game.value().get<std::vector<std::string>>();
                }
            }
            for (const auto& el : position_entries.items()) {
                long long tg = el.value().value("total_games", 0LL);
                if (tg < 0) continue; // Skip corrupted entries from previous 32-bit overflows
                
                std::vector<std::string> top_games;
                if (el.value().contains("top_games")) {
                    for (const auto& g : el.value()["top_games"]) {
                        top_games.push_back(g.get<std::string>());
                    }
                } else if (el.value().contains("game_info")) {
                    std::string gi = el.value().value("game_info", "");
                    if (!gi.empty()) top_games.push_back(gi);
                }

                LichessOpening cached{el.value().value("eco", ""), el.value().value("name", ""),
                                      "", tg, 200, 0,
                                      el.value().value("found", false), true,
                                      top_games,
                                      el.value().value("top_game_ids", std::vector<std::string>{})};
                if (el.value().contains("game_metadata")) {
                    const auto& game = el.value()["game_metadata"];
                    cached.game_metadata.found = game.value("found", false);
                    cached.game_metadata.id = game.value("id", "");
                    cached.game_metadata.event = game.value("event", "");
                    cached.game_metadata.site = game.value("site", "");
                    cached.game_metadata.date = game.value("date", "");
                    cached.game_metadata.round = game.value("round", "");
                    cached.game_metadata.white = game.value("white", "");
                    cached.game_metadata.black = game.value("black", "");
                    cached.game_metadata.result = game.value("result", "");
                    cached.game_metadata.white_elo = game.value("white_elo", "");
                    cached.game_metadata.black_elo = game.value("black_elo", "");
                    cached.game_metadata.eco = game.value("eco", "");
                    cached.game_metadata.opening = game.value("opening", "");
                    cached.game_metadata.http_status = game.value("http_status", 0);
                    cached.game_metadata.error = game.value("error", "");
                }
                // A cached position from before master-game IDs/metadata were
                // persisted cannot resolve authoritative headers. Re-query it
                // instead of silently accepting an opening-only cache entry.
                if (tg == 0 || cached.game_metadata.found || !cached.top_game_ids.empty()) {
                    cache_[el.key()] = std::move(cached);
                }
            }
        } catch(...) {}
    }

    thread_ = std::thread(&OpeningFetcher::worker_thread, this);
}

OpeningFetcher::~OpeningFetcher() {
    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }

    try {
        nlohmann::json j;
        nlohmann::json positions = nlohmann::json::object();
        for (const auto& [fen, info] : cache_) {
            if (info.api_success) {
                positions[fen] = {
                    {"eco", info.eco}, {"name", info.name},
                    {"total_games", info.total_games}, {"found", info.found},
                    {"top_games", info.top_games},
                    {"top_game_ids", info.top_game_ids},
                    {"game_metadata", {
                        {"found", info.game_metadata.found},
                        {"id", info.game_metadata.id},
                        {"event", info.game_metadata.event},
                        {"site", info.game_metadata.site},
                        {"date", info.game_metadata.date},
                        {"round", info.game_metadata.round},
                        {"white", info.game_metadata.white},
                        {"black", info.game_metadata.black},
                        {"result", info.game_metadata.result},
                        {"white_elo", info.game_metadata.white_elo},
                        {"black_elo", info.game_metadata.black_elo},
                        {"eco", info.game_metadata.eco},
                        {"opening", info.game_metadata.opening},
                        {"http_status", info.game_metadata.http_status},
                        {"error", info.game_metadata.error}
                    }}
                };
            }
        }
        j["version"] = 2;
        j["positions"] = std::move(positions);
        nlohmann::json master_games = nlohmann::json::object();
        for (const auto& [id, fens] : master_game_fens_) {
            master_games[id] = fens;
        }
        j["master_games"] = std::move(master_games);
        std::filesystem::create_directories(std::filesystem::path(cache_path_).parent_path());
        std::ofstream ofs(cache_path_);
        if (!ofs) {
            throw std::runtime_error("could not open cache file for writing");
        }
        ofs << j.dump(4);
        if (!ofs) {
            throw std::runtime_error("could not write cache file");
        }
    } catch (const std::exception& error) {
        std::cerr << "Could not persist Lichess opening cache: " << error.what() << "\n";
    }
}

void OpeningFetcher::set_api_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    api_token_ = token;
}

bool OpeningFetcher::test_connection(std::string& out_error) {
    LichessOpening result = fetch_from_lichess("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    if (result.http_status == 401 || result.http_status == 403) {
        out_error = "Lichess API returned HTTP " + std::to_string(result.http_status) + 
                    " Unauthorized. Please configure a valid API token in Settings -> Advanced.";
        return false;
    }
    return true;
}

void OpeningFetcher::enqueue_fen(const std::string& fen) {
    if (unique_game_reached_ || done_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(fen);
    cv_.notify_one();
}

LichessOpening OpeningFetcher::get_opening(const std::string& fen) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(fen);
    if (it != cache_.end()) {
        return it->second;
    }
    LichessOpening missing;
    missing.total_games = -1LL; // Use -1 to distinguish "not in cache" from "fetched but failed"
    return missing;
}

void OpeningFetcher::wait_until_done() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return unique_game_reached_ || (queue_.empty() && !active_request_);
        });
    }

    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    done_ = true;
}

bool OpeningFetcher::wait_until_done_for(std::chrono::milliseconds timeout) {
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        completed = cv_.wait_for(lock, timeout, [this]() {
            return unique_game_reached_ || (queue_.empty() && !active_request_);
        });
    }

    stop_ = true;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    done_ = true;
    return completed;
}

std::string OpeningFetcher::metadata_resolution_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_resolution_error_;
}

LichessOpening OpeningFetcher::fetch_from_lichess(
    const std::string& fen,
    const std::string& play) {
    LichessOpening result;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = api_token_;
    }

#ifdef _WIN32
    HINTERNET hSession = WinHttpOpen(L"ChessTubeAnalyzer-Extract/1.0 (local extraction pipeline)", 
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, 
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    configure_winhttp_timeouts(hSession);
    DWORD connect_retries = 1;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_RETRIES,
                     &connect_retries, sizeof(connect_retries));

    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secure_protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols, sizeof(secure_protocols));

#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD dwDecompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hSession, WINHTTP_OPTION_DECOMPRESSION, &dwDecompression, sizeof(dwDecompression));
#endif

    HINTERNET hConnect = WinHttpConnect(hSession, L"explorer.lichess.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        set_winhttp_error(result, "WinHttpConnect");
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Tournament and over-the-board games are in the masters database. The
    // general Lichess database contains rated online games and cannot supply
    // the event, round, site, or result headers for a broadcast game.
    std::string path = "/masters?variant=standard&topGames=15&fen=" +
        url_encode_query_value(fen);
    if (!play.empty()) {
        path += "&play=" + url_encode_query_value(play);
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.length()), nullptr, 0);
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.length()), &wpath[0], wlen);

    LPCWSTR acceptTypes[] = {
        L"application/x-ndjson", L"application/json", NULL
    };
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(), 
                                            NULL, WINHTTP_NO_REFERER, 
                                            acceptTypes, 
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        set_winhttp_error(result, "WinHttpOpenRequest");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }
    configure_winhttp_timeouts(hRequest);

    std::wstring headers = L"";
    if (!token.empty()) {
        std::string auth_header = "Authorization: Bearer " + token + "\r\n";
        int wlen_hdr = MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, nullptr, 0);
        headers.resize(wlen_hdr, 0);
        MultiByteToWideChar(CP_UTF8, 0, auth_header.c_str(), -1, &headers[0], wlen_hdr);
        headers.resize(wlen_hdr - 1); // remove the embedded null terminator included by -1
    }

    if (WinHttpSendRequest(hRequest, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), 
                           headers.empty() ? 0 : static_cast<DWORD>(-1), 
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        
        DWORD dwStatusCode = 0;
        DWORD dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, 
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                            WINHTTP_HEADER_NAME_BY_INDEX, 
                            &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        result.http_status = static_cast<int>(dwStatusCode);

        if (dwStatusCode == 429) {
            result.api_success = false;
            result.total_games = -2LL; // Signal rate limit
            result.error = "rate limited by Lichess";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        if (dwStatusCode == 404) {
            result.api_success = true;
            result.total_games = 0; // Out of book
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        if (dwStatusCode == 200) {
            DWORD size = 0;
            DWORD downloaded = 0;
            std::string response_body;

            do {
                size = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &size)) {
                    set_winhttp_error(result, "WinHttpQueryDataAvailable");
                    break;
                }
                if (size == 0) break;

                char* buffer = new char[size];
                if (!WinHttpReadData(hRequest, (LPVOID)buffer, size, &downloaded)) {
                    set_winhttp_error(result, "WinHttpReadData");
                    delete[] buffer;
                    break;
                }
                response_body.append(buffer, downloaded);
                delete[] buffer;

            } while (size > 0);

            nlohmann::json j;
            if (parse_explorer_response(response_body, j)) {
                if (j.contains("white") || j.contains("moves") ||
                    j.contains("opening")) {
                    result.api_success = true;
                    if (j.contains("opening") && !j["opening"].is_null()) {
                        result.eco = j["opening"].value("eco", "");
                        result.name = j["opening"].value("name", "");
                        result.found = true;
                    }
                    long long white = j.value("white", 0LL);
                    long long draws = j.value("draws", 0LL);
                    long long black = j.value("black", 0LL);
                    result.total_games = white + draws + black;
                    
                    const std::array<const char*, 2> game_list_keys = {
                        "topGames", "recentGames"
                    };
                    bool metadata_candidate_attempted = false;
                    for (const char* game_list_key : game_list_keys) {
                        if (!j.contains(game_list_key) ||
                            !j[game_list_key].is_array()) {
                            continue;
                        }
                        for (const auto& game : j[game_list_key]) {
                            const std::string game_id = game.value("id", "");
                            if (!game_id.empty()) result.top_game_ids.push_back(game_id);
                            std::string white_name = "Unknown";
                            if (game.contains("white") && game["white"].is_object()) white_name = game["white"].value("name", "Unknown");
                            std::string black_name = "Unknown";
                            if (game.contains("black") && game["black"].is_object()) black_name = game["black"].value("name", "Unknown");
                            std::string year = game.contains("year") && game["year"].is_number() ? std::to_string(game.value("year", 0)) : "";
                            
                            if (!year.empty() && year != "0") result.top_games.push_back(white_name + " vs " + black_name + " (" + year + ")");
                            else result.top_games.push_back(white_name + " vs " + black_name);

                            // Once the explorer identifies a single master
                            // game, retrieve its PGN. That is the authoritative
                            // source for date, round, site, and final result.
                            // A unique Explorer result can still repeat the
                            // same game in topGames and recentGames; retrying
                            // every entry multiplies several HTTP requests and
                            // can stall extraction when the service is slow.
                            if (result.total_games <= 1 &&
                                !metadata_candidate_attempted) {
                                const LichessGameMetadata summary = parse_master_game_summary(game);
                                if (!summary.id.empty()) {
                                    metadata_candidate_attempted = true;
                                    result.game_metadata = fetch_master_game(summary.id);
                                }
                            }
                        }
                    }
                } else {
                    result.error = "unexpected JSON response";
                }
            } else {
                result.error = "failed to parse JSON response";
            }
        } else {
            std::ostringstream oss;
            oss << "HTTP " << dwStatusCode;
            result.error = oss.str();
        }
    } else {
        set_winhttp_error(result, "WinHTTP request");
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
#endif
    return result;
}

LichessGameMetadata OpeningFetcher::fetch_master_game(const std::string& game_id) {
    LichessGameMetadata result;
    result.id = game_id;
#ifdef _WIN32
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        token = api_token_;
    }

    WinHttpTextResponse response = fetch_https_text(
        L"explorer.lichess.org",
        "/masters/pgn/" + url_encode_query_value(game_id),
        L"application/x-chess-pgn", token);
    if (response.body.empty()) {
        // The master database endpoint is authoritative, but older or
        // proxied Lichess deployments may not serve that route. The public
        // export endpoint returns the same game's PGN and is a safe fallback
        // because the game id came from Lichess Explorer itself.
        const WinHttpTextResponse fallback = fetch_https_text(
            L"lichess.org",
            "/game/export/" + url_encode_query_value(game_id),
            L"application/x-chess-pgn", token);
        if (!fallback.body.empty() || fallback.status != 0 || response.status == 0) {
            response = fallback;
        }
    }

    result.http_status = static_cast<int>(response.status);
    if (!response.body.empty()) {
        LichessGameMetadata parsed = parse_master_game_headers(response.body, game_id);
        parsed.http_status = result.http_status;

        // The masters PGN endpoint does not consistently include ECO and
        // Opening tags. Ask the game-export endpoint for those optional
        // headers, while retaining the masters response as the authoritative
        // source for the game identity and replay.
        if (parsed.eco.empty() || parsed.opening.empty()) {
            const WinHttpTextResponse supplemental = fetch_https_text(
                L"lichess.org",
                "/game/export/" + url_encode_query_value(game_id) +
                    "?opening=1&clocks=0&evals=0",
                L"application/x-chess-pgn", token);
            if (!supplemental.body.empty()) {
                const LichessGameMetadata supplemental_metadata =
                    parse_master_game_headers(supplemental.body, game_id);
                if (parsed.event.empty()) parsed.event = supplemental_metadata.event;
                if (parsed.site.empty()) parsed.site = supplemental_metadata.site;
                if (parsed.date.empty()) parsed.date = supplemental_metadata.date;
                if (parsed.round.empty()) parsed.round = supplemental_metadata.round;
                if (parsed.white.empty()) parsed.white = supplemental_metadata.white;
                if (parsed.black.empty()) parsed.black = supplemental_metadata.black;
                if (parsed.result.empty()) parsed.result = supplemental_metadata.result;
                if (parsed.white_elo.empty()) parsed.white_elo = supplemental_metadata.white_elo;
                if (parsed.black_elo.empty()) parsed.black_elo = supplemental_metadata.black_elo;
                if (parsed.eco.empty()) parsed.eco = supplemental_metadata.eco;
                if (parsed.opening.empty()) parsed.opening = supplemental_metadata.opening;
                parsed.found = !parsed.date.empty() && !parsed.white.empty() &&
                    !parsed.black.empty() && !parsed.result.empty();
            }
        }
        result = std::move(parsed);
        master_game_fens_[game_id] = parse_master_game_fens(response.body);
    } else if (response.status != 200) {
        result.error = response.status == 0 && response.error != ERROR_SUCCESS
            ? "WinHTTP error " + std::to_string(response.error)
            : "HTTP " + std::to_string(response.status);
    } else {
        result.error = "empty PGN response";
    }
#endif
    return result;
}

void OpeningFetcher::resolve_game_metadata(
    const std::vector<std::string>& observed_fens,
    const std::vector<std::string>& observed_moves) {
    if (observed_fens.empty()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        metadata_resolution_error_.clear();

        // Game identity belongs to a verified move/FEN sequence, not to an
        // individual position.  Opening positions are commonly shared by
        // unrelated games, so metadata retained from an earlier lookup must
        // not become a fallback for this sequence if resolution fails.
        for (const std::string& fen : observed_fens) {
            auto it = cache_.find(fen);
            if (it != cache_.end()) it->second.game_metadata = {};
        }
    }

    // Older opening-cache entries may contain candidate game ids but no ECO
    // or opening tags. Replay the extracted move prefix through Explorer so
    // it can resolve a named opening even when the final deep FEN is not an
    // exact named-position lookup.
    LichessOpening opening_metadata;
    for (const std::string& fen : observed_fens) {
        opening_metadata = get_opening(fen);
        if (!opening_metadata.eco.empty() && !opening_metadata.name.empty()) break;
    }
    if (opening_metadata.eco.empty() || opening_metadata.name.empty()) {
        if (observed_moves.size() >= observed_fens.size() - 1) {
            bool found_named_opening = false;
            std::string play;
            for (size_t index = 0; index + 1 < observed_fens.size(); ++index) {
                if (index >= observed_moves.size()) break;
                if (!play.empty()) play.push_back(',');
                play += observed_moves[index];
                const LichessOpening refreshed = fetch_from_lichess(
                    observed_fens.front(), play);
                if (!refreshed.api_success) {
                    // A failed request cannot become useful by adding more
                    // moves, and continuing would multiply timeout/rate-limit
                    // delays for every observed ply.
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cache_[observed_fens[index + 1]] = refreshed;
                }
                if (!refreshed.eco.empty() && !refreshed.name.empty()) {
                    opening_metadata = refreshed;
                    found_named_opening = true;
                } else if (found_named_opening) {
                    break;
                }
            }
        }
    }

    constexpr size_t kMaxMetadataCandidates = 8;
    std::vector<std::string> candidate_ids;
    std::unordered_map<std::string, size_t> candidate_frequency;
    std::unordered_map<std::string, size_t> candidate_first_seen;
    size_t candidate_order = 0;
    // Candidates from the deepest observed position are the most selective,
    // so inspect those before broad opening-position candidates.
    for (auto fen_it = observed_fens.rbegin(); fen_it != observed_fens.rend(); ++fen_it) {
        const LichessOpening info = get_opening(*fen_it);
        for (const std::string& id : info.top_game_ids) {
            ++candidate_frequency[id];
            candidate_first_seen.try_emplace(id, candidate_order++);
        }
    }

    candidate_ids.reserve(candidate_frequency.size());
    for (const auto& [id, frequency] : candidate_frequency) {
        (void)frequency;
        candidate_ids.push_back(id);
    }
    std::stable_sort(candidate_ids.begin(), candidate_ids.end(),
        [&](const std::string& left, const std::string& right) {
            if (candidate_first_seen.at(left) != candidate_first_seen.at(right)) {
                return candidate_first_seen.at(left) < candidate_first_seen.at(right);
            }
            return candidate_frequency.at(left) > candidate_frequency.at(right);
        });
    if (candidate_ids.size() > kMaxMetadataCandidates) {
        candidate_ids.resize(kMaxMetadataCandidates);
    }

    // A truncated video may never reach a position with one master-game hit.
    // Compare the observed FEN sequence with candidate master PGNs so
    // metadata can still be recovered without knowing fixture names or
    // reading an answer key. A contiguous match is required; metadata alone
    // is never sufficient because multiple games can share opening positions.
    struct MatchingCandidate {
        LichessGameMetadata metadata;
        size_t first_seen = 0;
        size_t frequency = 0;
    };
    std::vector<MatchingCandidate> matching_candidates;
    std::string last_candidate_error;
    for (const std::string& id : candidate_ids) {
        LichessGameMetadata metadata;
        std::vector<std::string> game_fens;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            game_fens = master_game_fens_[id];
        }
        if (game_fens.empty()) {
            metadata = fetch_master_game(id);
            std::lock_guard<std::mutex> lock(mutex_);
            game_fens = master_game_fens_[id];
        } else {
            for (const std::string& fen : observed_fens) {
                const LichessOpening info = get_opening(fen);
                if (info.game_metadata.found && info.game_metadata.id == id) {
                    metadata = info.game_metadata;
                    break;
                }
            }
            if (!metadata.found) metadata.id = id;
        }

        if (!metadata.found) {
            last_candidate_error = id + ": " +
                (metadata.error.empty() ? "Lichess PGN headers were not parsed" : metadata.error) +
                " (HTTP " + std::to_string(metadata.http_status) + ")";
            continue;
        }
        if (game_fens.empty()) {
            last_candidate_error = id + ": Lichess PGN contained no replayable moves";
            continue;
        }
        if (!contains_fen_sequence(game_fens, observed_fens)) {
            last_candidate_error = id + ": Lichess PGN did not reproduce the extracted FEN sequence";
            continue;
        }

        if (metadata.eco.empty()) metadata.eco = opening_metadata.eco;
        if (metadata.opening.empty()) metadata.opening = opening_metadata.name;
        matching_candidates.push_back({
            std::move(metadata),
            candidate_first_seen.at(id),
            candidate_frequency.at(id),
        });
    }

    if (!matching_candidates.empty()) {
        // Several master games can share a truncated opening prefix. When
        // their replayed positions are equally valid, prefer the most recent
        // authoritative record; Explorer's top-game order is popularity-based
        // and otherwise tends to select an older famous game.
        const auto date_key = [](const std::string& date) {
            std::string key;
            for (const char character : date) {
                if (character >= '0' && character <= '9') key.push_back(character);
            }
            return key;
        };
        std::stable_sort(matching_candidates.begin(), matching_candidates.end(),
            [&](const MatchingCandidate& left, const MatchingCandidate& right) {
                const std::string left_date = date_key(left.metadata.date);
                const std::string right_date = date_key(right.metadata.date);
                if (left_date != right_date) return left_date > right_date;
                if (left.first_seen != right.first_seen) {
                    return left.first_seen < right.first_seen;
                }
                return left.frequency > right.frequency;
            });

        const LichessGameMetadata& metadata = matching_candidates.front().metadata;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& fen : observed_fens) {
            auto it = cache_.find(fen);
            if (it != cache_.end()) it->second.game_metadata = metadata;
        }
        metadata_resolution_error_.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    metadata_resolution_error_ = candidate_ids.empty()
        ? "Lichess Explorer returned no candidate game IDs"
        : "No Lichess candidate matched the extracted FEN sequence; last candidate: " +
          last_candidate_error;
}

void OpeningFetcher::worker_thread() {
    while (true) {
        std::string fen;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !queue_.empty() || stop_; });
            
            if (unique_game_reached_ || (stop_ && queue_.empty())) {
                cv_.notify_all();
                break;
            }
            
            fen = queue_.front();
            queue_.pop();
            
            // Check cache first to avoid redundant API calls
            if (cache_.find(fen) != cache_.end()) {
                if (cache_[fen].total_games == 0 ||
                    (cache_[fen].total_games == 1 && cache_[fen].game_metadata.found)) {
                    unique_game_reached_ = true;
                    cv_.notify_all();
                    break;
                }
                continue; // Instant lookup, no network delay needed
            }

            active_request_ = true;
        }
        
        LichessOpening info = fetch_from_lichess(fen);
        if (info.total_games == -2LL) {
            // Do not sleep for the server's rate-limit window. A metadata
            // lookup cannot satisfy the caller while rate-limited, and the
            // caller must receive a prompt, explicit failure instead of a
            // minute-long hang.
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[fen] = info;
            active_request_ = false;
            unique_game_reached_ = true;
            cv_.notify_all();
            break;
        }
        if (stop_) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_request_ = false;
            cv_.notify_all();
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[fen] = info;
            active_request_ = false;
            cv_.notify_all();
        }
        if (info.api_success && info.total_games == 0) {
            unique_game_reached_ = true;
            cv_.notify_all();
            break;
        }
        if (info.api_success && info.total_games == 1 && info.game_metadata.found) {
            unique_game_reached_ = true;
            cv_.notify_all();
            break;
        }
        // Lichess rate limits to 1 per second generally; buffer to stay safe.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
}

} // namespace cta
