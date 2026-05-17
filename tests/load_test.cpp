#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <Poco/DateTime.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/Statement.h>
#include <Poco/Exception.h>
#include <Poco/StreamCopier.h>
#include <Poco/JWT/Signer.h>
#include <Poco/JWT/Token.h>
#include <Poco/Timestamp.h>

#include <sodium.h>

#include <RGT/Devkit/General.h>
#include <RGT/Devkit/JWTPayload.h>
#include <RGT/Devkit/Types.h>
#include <RGT/Devkit/TestTools/Client.h>
#include <RGT/Devkit/TestTools/ConnectionRegistry.h>

namespace RGT::Main::Tests
{

namespace
{

struct HttpResult
{
    int statusCode = 0;
    std::string body;
    std::chrono::microseconds latency{};
};

struct LatencyStats
{
    mutable std::mutex mutex;
    std::vector<double> latenciesMs;
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> httpOk{0};
    std::atomic<uint64_t> businessOk{0};

    void record(const HttpResult & result, bool countBusinessOk)
    {
        ++requests;
        if (result.statusCode == static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK)) {
            ++httpOk;
        }

        const double ms = std::chrono::duration<double, std::milli>(result.latency).count();
        {
            std::lock_guard lock(mutex);
            latenciesMs.push_back(ms);
        }

        if (countBusinessOk and result.statusCode == static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK))
        {
            try
            {
                Poco::JSON::Parser parser;
                Poco::JSON::Object::Ptr obj = parser.parse(result.body).extract<Poco::JSON::Object::Ptr>();
                if (obj->has("status") and obj->getValue<std::string>("status") == "OK")
                {
                    if (not obj->has("message") or obj->getValue<std::string>("message") == "OK") {
                        ++businessOk;
                    }
                }
            }
            catch (...)
            {
            }
        }
    }

    void recordCoordinates(const HttpResult & result)
    {
        ++requests;
        if (result.statusCode == static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK)) {
            ++httpOk;
            try
            {
                Poco::JSON::Parser parser;
                Poco::JSON::Object::Ptr obj = parser.parse(result.body).extract<Poco::JSON::Object::Ptr>();
                if (not obj.isNull()) {
                    ++businessOk;
                }
            }
            catch (...)
            {
            }
        }

        const double ms = std::chrono::duration<double, std::milli>(result.latency).count();
        {
            std::lock_guard lock(mutex);
            latenciesMs.push_back(ms);
        }
    }

    void printSummary(const char * sectionTitle) const
    {
        std::vector<double> sorted;
        {
            std::lock_guard lock(mutex);
            sorted = latenciesMs;
        }
        std::sort(sorted.begin(), sorted.end());

        const uint64_t total = requests.load();
        const uint64_t okHttp = httpOk.load();
        const uint64_t okBiz = businessOk.load();

        std::cerr << std::format("\n  {}\n", sectionTitle);
        std::cerr << std::format("  Всего запросов:        {}\n", total);
        std::cerr << std::format("  Успешных ответов:      {} ({:.1f}%)\n",
            okHttp,
            total ? 100.0 * okHttp / total : 0.0);
        std::cerr << std::format("  Корректных операций:   {} ({:.1f}%)\n",
            okBiz,
            total ? 100.0 * okBiz / total : 0.0);

        if (sorted.empty()) {
            std::cerr << "  Время отклика:         нет данных\n";
            return;
        }

        const auto percentile = [&sorted](double p) -> double
        {
            if (sorted.empty()) {
                return 0.0;
            }
            const size_t idx = static_cast<size_t>(std::ceil(p * sorted.size())) - 1;
            return sorted[std::min(idx, sorted.size() - 1)];
        };

        double sum = 0.0;
        for (double v : sorted) {
            sum += v;
        }

        std::cerr << "  Время отклика (мс):\n";
        std::cerr << std::format("    минимум   {:>8.2f}\n", sorted.front());
        std::cerr << std::format("    среднее   {:>8.2f}\n", sum / sorted.size());
        std::cerr << std::format("    медиана   {:>8.2f}\n", percentile(0.50));
        std::cerr << std::format("    p95       {:>8.2f}\n", percentile(0.95));
        std::cerr << std::format("    p99       {:>8.2f}\n", percentile(0.99));
        std::cerr << std::format("    максимум  {:>8.2f}\n", sorted.back());
    }
};

void printReportHeader(uint32_t participants, uint32_t durationSec, uint32_t judgesCount)
{
    std::cerr << "\n";
    std::cerr << "================================================================\n";
    std::cerr << "  НАГРУЗОЧНОЕ ТЕСТИРОВАНИЕ — REGATTA TRACKER\n";
    std::cerr << "================================================================\n";
    std::cerr << "  Сценарий: гонка в реальном времени\n";
    std::cerr << std::format("    • участников (яхтсменов): {}\n", participants);
    std::cerr << std::format("    • судей (тренер):           {}\n", judgesCount);
    std::cerr << std::format("    • длительность гонки:       {} с\n", durationSec);
    std::cerr << "    • частота отправки GPS:     1 координата / с на участника\n";
    std::cerr << "    • опрос координат тренером: 1 запрос / с\n";
    std::cerr << "----------------------------------------------------------------\n";
}

void printReportFooter(uint64_t raceId, bool passed)
{
    std::cerr << "----------------------------------------------------------------\n";
    std::cerr << std::format("  Гонка №{} завершена\n", raceId);
    std::cerr << std::format("  ИТОГ НАГРУЗОЧНОГО ТЕСТА: {}\n", passed ? "УСПЕШНО" : "ЕСТЬ ОТКЛОНЕНИЯ");
    std::cerr << "================================================================\n\n";
}

void configureHttpSession(Poco::Net::HTTPClientSession & session)
{
    const Poco::Timespan timeout(30, 0);
    session.setTimeout(timeout);
    session.setKeepAliveTimeout(timeout);
}

HttpResult doHttpRaw
(
    const std::string & method,
    const std::string & uri,
    const std::string & accessToken,
    const std::optional<std::string> & jsonBody
)
{
    const auto started = std::chrono::steady_clock::now();

    Poco::Net::HTTPClientSession session("127.0.0.1", 80);
    configureHttpSession(session);
    Poco::Net::HTTPRequest request;
    request.setMethod(method);
    request.setURI(uri);
    request.set("X-Fingerprint", RGT::Devkit::TestTools::default_fingerprint);
    request.set("User-Agent", RGT::Devkit::TestTools::default_user_agent);
    request.setCredentials("Bearer", accessToken);

    if (jsonBody.has_value()) {
        request.setContentType("application/json");
        request.setContentLength(static_cast<std::streamsize>(jsonBody->size()));
    }

    std::ostream & os = session.sendRequest(request);
    if (jsonBody.has_value()) {
        os << *jsonBody;
    }

    Poco::Net::HTTPResponse response;
    std::istream & is = session.receiveResponse(response);
    std::string body;
    Poco::StreamCopier::copyToString(is, body);

    const auto finished = std::chrono::steady_clock::now();

    return {
        .statusCode = static_cast<int>(response.getStatus()),
        .body = std::move(body),
        .latency = std::chrono::duration_cast<std::chrono::microseconds>(finished - started),
    };
}

std::string currentTimeIsoUtc()
{
    return Poco::DateTimeFormatter::format(
        Poco::DateTime(),
        Poco::DateTimeFormat::ISO8601_FRAC_FORMAT);
}

void trimEnvValue(std::string & value)
{
    while (not value.empty() and std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (not value.empty() and std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    if (value.size() >= 2
        and ((value.front() == '"' and value.back() == '"') or (value.front() == '\'' and value.back() == '\'')))
    {
        value = value.substr(1, value.size() - 2);
    }
}

bool isTruthyEnv(const std::string & value)
{
    std::string normalized = value;
    trimEnvValue(normalized);
    for (char & c : normalized) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return normalized == "1" or normalized == "true" or normalized == "yes" or normalized == "on";
}

bool isLoadTestEnabled()
{
    if (std::optional<std::string> run = RGT::Devkit::getEnv("LOAD_TEST_RUN"); run.has_value()) {
        return isTruthyEnv(*run);
    }
    return false;
}

uint32_t loadTestParticipantsCount()
{
    if (std::optional<std::string> raw = RGT::Devkit::getEnv("LOAD_TEST_PARTICIPANTS"); raw.has_value()) {
        return static_cast<uint32_t>(std::stoul(*raw));
    }
    return 499;
}

uint32_t loadTestDurationSec()
{
    if (std::optional<std::string> raw = RGT::Devkit::getEnv("LOAD_TEST_DURATION_SEC"); raw.has_value()) {
        return static_cast<uint32_t>(std::stoul(*raw));
    }
    return 180;
}

uint32_t loadTestUploadThreads()
{
    if (std::optional<std::string> raw = RGT::Devkit::getEnv("LOAD_TEST_UPLOAD_THREADS"); raw.has_value()) {
        return static_cast<uint32_t>(std::stoul(*raw));
    }
    return 64;
}

std::string hashPasswordLikeAuth(const std::string & password)
{
    static const bool sodiumReady = []() -> bool
    {
        if (sodium_init() < 0) {
            throw std::runtime_error("sodium_init failed");
        }
        return true;
    }();
    (void)sodiumReady;

    char hashed[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(
            hashed,
            password.c_str(),
            password.size(),
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE)
        != 0)
    {
        throw std::runtime_error("password hashing failed");
    }
    return std::string(hashed);
}

struct LoadTestAccount
{
    uint32_t index = 0;
    uint64_t userId = 0;
    std::string login;
    std::string accessToken;
};

uint32_t accessTokenValiditySec()
{
    if (std::optional<std::string> raw = RGT::Devkit::getEnv("ACCESS_TOKEN_VALIDITY_SEC"); raw.has_value()) {
        return static_cast<uint32_t>(std::stoul(*raw));
    }
    return 900;
}

std::string mintAccessToken(uint64_t userId, RGT::Devkit::UserRole role)
{
    std::optional<std::string> secretKey = RGT::Devkit::getEnv("SECRET_KEY");
    if (not secretKey.has_value() or secretKey->empty()) {
        throw std::runtime_error("SECRET_KEY is required to mint JWT for load test");
    }

    const auto exp = std::chrono::duration_cast<std::chrono::seconds>(
        (
            std::chrono::system_clock::now()
            + std::chrono::seconds(accessTokenValiditySec())
        ).time_since_epoch());

    RGT::Devkit::JWTPayload payload{
        .sub = RGT::Devkit::mapUintToUserId(userId),
        .role = role,
        .exp = exp,
    };

    Poco::JWT::Token token;
    token.setSubject(std::to_string(RGT::Devkit::mapUserIdToUint(payload.sub)));
    token.payload().set("role", std::string(RGT::Devkit::mapUserRoleToString(payload.role)));
    token.setExpiration(static_cast<Poco::Timestamp::TimeVal>(
        std::chrono::duration_cast<std::chrono::microseconds>(payload.exp).count()));

    Poco::JWT::Signer signer(*secretKey);
    return signer.sign(token, Poco::JWT::Signer::ALGO_HS256);
}

void assignAccessTokens(std::vector<LoadTestAccount> & accounts)
{
    for (LoadTestAccount & account : accounts) {
        account.accessToken = mintAccessToken(account.userId, RGT::Devkit::UserRole::Participant);
    }
}

[[noreturn]] void rethrowWithContext(const char * step, const std::exception & e)
{
    throw std::runtime_error(std::format("{}: {}", step, e.what()));
}

void rethrowPocoWithContext(const char * step)
{
    try
    {
        throw;
    }
    catch (const Poco::Exception & e)
    {
        throw std::runtime_error(std::format("{}: {}", step, e.displayText()));
    }
    catch (const std::exception & e)
    {
        rethrowWithContext(step, e);
    }
}

std::vector<LoadTestAccount> bulkInsertParticipants
(
    const std::string & loginRoot,
    uint32_t participantsCount
)
{
    std::string passwordHash = hashPasswordLikeAuth(RGT::Devkit::TestTools::default_password);
    std::string role = "participant";

    Poco::Data::Session session = RGT::Devkit::TestTools::ConnectionRegistry::instance().getPsqlPool().get();

    std::vector<LoadTestAccount> accounts;
    accounts.reserve(participantsCount);

    try
    {
        for (uint32_t i = 0; i < participantsCount; ++i)
        {
            std::string name = std::format("S{}", i);
            std::string surname = "Boat";
            std::string login = std::format("{}p{:03}", loginRoot, i);
            uint64_t userId = 0;

            session << "INSERT INTO users (name, surname, role, login, password_hash)"
                         " VALUES ($1, $2, $3, $4, $5) RETURNING id",
                Poco::Data::Keywords::use(name),
                Poco::Data::Keywords::use(surname),
                Poco::Data::Keywords::use(role),
                Poco::Data::Keywords::use(login),
                Poco::Data::Keywords::use(passwordHash),
                Poco::Data::Keywords::into(userId),
                Poco::Data::Keywords::now;

            accounts.push_back(LoadTestAccount{.index = i, .userId = userId, .login = std::move(login)});
        }
    }
    catch (...)
    {
        rethrowPocoWithContext("регистрация участников в PostgreSQL");
    }

    return accounts;
}

void deleteParticipantsByLoginPrefix(const std::string & loginRoot)
{
    std::string pattern = loginRoot + "p%";
    Poco::Data::Session session = RGT::Devkit::TestTools::ConnectionRegistry::instance().getPsqlPool().get();
    session << "DELETE FROM users WHERE login LIKE $1",
        Poco::Data::Keywords::use(pattern),
        Poco::Data::Keywords::now;
}

std::string bearerTokenFromUser(RGT::Devkit::TestTools::User & user);

void createRaceWithParticipants
(
    RGT::Devkit::TestTools::User & judge,
    std::span<const uint64_t> participantIds,
    uint64_t & raceIdOut
)
{
    Poco::JSON::Object jsonBody;
    Poco::JSON::Array::Ptr participantsArr = new Poco::JSON::Array;
    for (uint64_t id : participantIds) {
        participantsArr->add(static_cast<Poco::Int64>(id));
    }
    jsonBody.set("participants", participantsArr);

    Poco::JSON::Array::Ptr judgesArr = new Poco::JSON::Array;
    judgesArr->add(static_cast<Poco::Int64>(judge.getId()));
    jsonBody.set("judges", judgesArr);

    std::ostringstream bodyStream;
    jsonBody.stringify(bodyStream);
    const std::string body = bodyStream.str();

    const HttpResult result = doHttpRaw(
        Poco::Net::HTTPRequest::HTTP_POST,
        "/management/create_race",
        bearerTokenFromUser(judge),
        body);

    // getRequestBlank sets Authorization as "Bearer <token>" - extract properly
    ASSERT_EQ(result.statusCode, static_cast<int>(Poco::Net::HTTPResponse::HTTP_CREATED))
        << result.body;

    Poco::JSON::Parser parser;
    Poco::JSON::Object::Ptr root = parser.parse(result.body).extract<Poco::JSON::Object::Ptr>();
    ASSERT_TRUE(root->has("id"));
    raceIdOut = root->get("id").convert<uint64_t>();
    ASSERT_GT(raceIdOut, 0u) << result.body;
}

std::string bearerTokenFromUser(RGT::Devkit::TestTools::User & user)
{
    Poco::Net::HTTPRequest blank = user.getRequestBlank();
    std::string scheme;
    std::string token;
    blank.getCredentials(scheme, token);
    return token;
}

void postManagementRaceId(RGT::Devkit::TestTools::User & judge, const std::string & uri, uint64_t raceId)
{
    Poco::JSON::Object body;
    body.set("race_id", static_cast<Poco::UInt64>(raceId));
    std::ostringstream oss;
    body.stringify(oss);

    const HttpResult result = doHttpRaw(
        Poco::Net::HTTPRequest::HTTP_POST,
        uri,
        bearerTokenFromUser(judge),
        oss.str());

    ASSERT_EQ(result.statusCode, static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK)) << uri << " " << result.body;
}

struct ParticipantHandle
{
    uint32_t index = 0;
    uint64_t userId = 0;
    std::string accessToken;
};

void uploadOnce(ParticipantHandle participant, uint32_t tick, LatencyStats & stats)
{
    Poco::JSON::Object jsonBody;
    jsonBody.set("time", currentTimeIsoUtc());
    jsonBody.set("longitude", 37.6 + 0.0001 * static_cast<double>((participant.index + tick) % 500));
    jsonBody.set("latitude", 55.7 + 0.0001 * static_cast<double>((participant.index * 3 + tick) % 500));

    std::ostringstream oss;
    jsonBody.stringify(oss);

    const HttpResult result = doHttpRaw(
        Poco::Net::HTTPRequest::HTTP_POST,
        "/receiver/upload",
        participant.accessToken,
        oss.str());

    stats.record(result, true);
}

void uploadWorkerLoop
(
    std::vector<ParticipantHandle> assigned,
    std::atomic<bool> & stopFlag,
    LatencyStats & stats
)
{
    uint32_t tick = 0;
    while (not stopFlag.load(std::memory_order_relaxed))
    {
        const auto tickStart = std::chrono::steady_clock::now();

        for (const ParticipantHandle & participant : assigned) {
            uploadOnce(participant, tick, stats);
        }

        const auto tickEnd = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart);
        const auto sleepFor = std::chrono::seconds(1) - elapsed;
        if (sleepFor.count() > 0) {
            std::this_thread::sleep_for(sleepFor);
        }

        ++tick;
    }
}

void trainerObserverLoop
(
    const std::string & trainerToken,
    uint64_t raceId,
    std::atomic<bool> & stopFlag,
    LatencyStats & stats
)
{
    const std::string uri = std::format("/observer/coordinates/{}", raceId);

    while (not stopFlag.load(std::memory_order_relaxed))
    {
        const auto tickStart = std::chrono::steady_clock::now();

        const HttpResult result = doHttpRaw(
            Poco::Net::HTTPRequest::HTTP_GET,
            uri,
            trainerToken,
            std::nullopt);

        stats.recordCoordinates(result);

        const auto tickEnd = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart);
        const auto sleepFor = std::chrono::seconds(1) - elapsed;
        if (sleepFor.count() > 0) {
            std::this_thread::sleep_for(sleepFor);
        }
    }
}

} // namespace

/// Нагрузочный сценарий: 1 тренер + N яхтсменов (по умолчанию 499).
/// Во время гонки каждый яхтсмен раз в секунду шлёт координаты, тренер раз в секунду запрашивает /observer/coordinates/{raceId}.
/// Длительность: LOAD_TEST_DURATION_SEC (по умолчанию 180). N: LOAD_TEST_PARTICIPANTS.
TEST(IntegrationRace, trainer_499_participants_sustained_load)
{
    RGT::Devkit::readDotEnv();

    if (not isLoadTestEnabled())
    {
        GTEST_SKIP() << "Set LOAD_TEST_RUN=1 in tests/.env (or regatta-tracker-main/.env for Docker). "
                        "When using docker compose, recreate the integration-tests container after changing .env.";
    }

    const uint32_t participantsCount = loadTestParticipantsCount();
    const uint32_t durationSec = loadTestDurationSec();
    const uint32_t uploadThreads = std::max(1u, std::min(loadTestUploadThreads(), participantsCount));

    ASSERT_GE(participantsCount, 3u);
    ASSERT_GE(durationSec, 10u);

    const auto stampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto stamp = static_cast<std::make_unsigned_t<decltype(stampMs)>>(stampMs);
    const std::string loginRoot = std::format("load{}", stamp);

    printReportHeader(participantsCount, durationSec, 1);

    std::cerr << "  Подготовка тестовых данных...\n";

    const auto setupStarted = std::chrono::steady_clock::now();

    std::vector<LoadTestAccount> accounts;
    try
    {
        accounts = bulkInsertParticipants(loginRoot, participantsCount);
        assignAccessTokens(accounts);
    }
    catch (const std::exception & e)
    {
        FAIL() << e.what();
    }

    RGT::Devkit::TestTools::User trainer(
        "LoadTrainer",
        "Judge",
        loginRoot + "judge",
        RGT::Devkit::TestTools::Role::Judge);
    const std::string trainerToken = bearerTokenFromUser(trainer);

    const auto setupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - setupStarted).count();
    std::cerr << std::format("  Участники зарегистрированы, сессии выданы ({} мс)\n", setupMs);

    std::vector<uint64_t> participantIds;
    participantIds.reserve(participantsCount);
    std::vector<ParticipantHandle> handles;
    handles.reserve(participantsCount);
    for (const LoadTestAccount & account : accounts)
    {
        participantIds.push_back(account.userId);
        handles.push_back(ParticipantHandle{
            .index = account.index,
            .userId = account.userId,
            .accessToken = account.accessToken,
        });
    }

    uint64_t raceId = 0;
    const auto createStarted = std::chrono::steady_clock::now();
    createRaceWithParticipants(trainer, participantIds, raceId);
    const auto createMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - createStarted).count();
    std::cerr << std::format("  Гонка создана (№{}, {} мс)\n", raceId, createMs);

    postManagementRaceId(trainer, "/management/start_race", raceId);
    std::cerr << std::format("  Гонка №{} стартовала — фаза нагрузки ({} с)\n", raceId, durationSec);
    std::cerr.flush();

    std::atomic<bool> stopFlag{false};
    LatencyStats uploadStats;
    LatencyStats observerStats;

    std::thread trainerThread(trainerObserverLoop, trainerToken, raceId, std::ref(stopFlag), std::ref(observerStats));

    std::vector<std::vector<ParticipantHandle>> uploadAssignments(uploadThreads);
    for (const ParticipantHandle & handle : handles) {
        uploadAssignments[handle.index % uploadThreads].push_back(handle);
    }

    std::vector<std::thread> uploadPool;
    uploadPool.reserve(uploadThreads);
    for (uint32_t t = 0; t < uploadThreads; ++t)
    {
        uploadPool.emplace_back(
            uploadWorkerLoop,
            std::move(uploadAssignments[t]),
            std::ref(stopFlag),
            std::ref(uploadStats));
    }

    std::this_thread::sleep_for(std::chrono::seconds(durationSec));
    stopFlag.store(true, std::memory_order_relaxed);

    trainerThread.join();
    for (std::thread & th : uploadPool) {
        th.join();
    }

    const uint64_t expectedUploads = static_cast<uint64_t>(participantsCount) * durationSec;
    const uint64_t expectedObserverRequests = durationSec;

    std::cerr << "\n  РЕЗУЛЬТАТЫ НАГРУЗКИ\n";
    std::cerr << std::format("  Ожидалось запросов upload:   {}\n", expectedUploads);
    std::cerr << std::format("  Ожидалось запросов тренера: {}\n", expectedObserverRequests);

    uploadStats.printSummary("Приём координат (POST /receiver/upload)");
    observerStats.printSummary("Просмотр трека тренером (GET /observer/coordinates)");

    postManagementRaceId(trainer, "/management/end_race", raceId);

    deleteParticipantsByLoginPrefix(loginRoot);

    const bool uploadOk = uploadStats.httpOk.load() > uploadStats.requests.load() / 2;
    const bool observerOk = observerStats.httpOk.load() > observerStats.requests.load() / 2;
    printReportFooter(raceId, uploadOk and observerOk);

    EXPECT_GT(uploadStats.httpOk.load(), uploadStats.requests.load() / 2)
        << "слишком много ошибок при приёме координат";
    EXPECT_GT(observerStats.httpOk.load(), observerStats.requests.load() / 2)
        << "слишком много ошибок при запросе координат тренером";
}

} // namespace RGT::Main::Tests
