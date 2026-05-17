#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <regex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <Poco/Data/Data.h>
#include <Poco/Data/Session.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>

#include <RGT/Devkit/General.h>
#include <RGT/Devkit/TestTools/Client.h>
#include <RGT/Devkit/TestTools/ConnectionRegistry.h>

#ifndef GPX_FILES_DIR
    #error GPX_FILES_DIR must be defined (path to tests/gpx-files)
#endif

namespace RGT::Main::Tests
{

namespace
{

struct GpxPoint
{
    std::string timeIso;
    double latitude{};
    double longitude{};
};

/// GPX 1.1: один сегмент между открытием trkpt и первым тегом <time> внутри точки.
std::vector<GpxPoint> parseGpxFile(const std::string & path)
{
    std::ifstream file(path);
    if (not file) {
        throw std::runtime_error(std::format("Cannot open GPX file: {}", path));
    }
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    static const std::regex blockRe(
        R"gpx(<trkpt\s+lat="([^"]+)"\s+lon="([^"]+)">[\s\S]*?<time>([^<]+)</time>)gpx",
        std::regex::ECMAScript);

    std::vector<GpxPoint> points;
    for (std::sregex_iterator it(content.begin(), content.end(), blockRe), end; it != end; ++it)
    {
        GpxPoint p;
        p.latitude = std::stod((*it)[1].str());
        p.longitude = std::stod((*it)[2].str());
        p.timeIso = (*it)[3].str();
        points.push_back(std::move(p));
    }

    if (points.empty()) {
        //throw std::runtime_error(std::format("No track points parsed from {}", path));
    }
    return points;
}

std::string doHttpJson
(
    RGT::Devkit::TestTools::User & user,
    const std::string & method,
    const std::string & uri,
    const Poco::JSON::Object & jsonBody
)
{
    Poco::Net::HTTPClientSession session("127.0.0.1", 80);
    Poco::Net::HTTPRequest request = user.getRequestBlank();
    request.setMethod(method);
    request.setURI(uri);

    std::ostringstream bodyStream;
    jsonBody.stringify(bodyStream);
    const std::string body = bodyStream.str();
    request.setContentLength(body.size());
    request.setContentType("application/json");

    std::ostream & os = session.sendRequest(request);
    os << body;

    Poco::Net::HTTPResponse response;
    std::istream & is = session.receiveResponse(response);
    std::string responseBody;
    Poco::StreamCopier::copyToString(is, responseBody);

    return std::string(std::to_string(static_cast<int>(response.getStatus()))) + "\n" + responseBody;
}

void assertUserHasRole(uint64_t userId, const std::string & expectedRole)
{
    Poco::Data::Session session = RGT::Devkit::TestTools::ConnectionRegistry::instance().getPsqlPool().get();
    std::string role;
    session
        << "SELECT role::text FROM users WHERE id = $1",
        Poco::Data::Keywords::use(userId),
        Poco::Data::Keywords::into(role),
        Poco::Data::Keywords::now;
    ASSERT_EQ(role, expectedRole)
        << "user id " << userId << " must have role '" << expectedRole << "', got '" << role << "'";
}

void createRace
(
    RGT::Devkit::TestTools::User & judge,
    std::span<RGT::Devkit::TestTools::User * const> participants,
    uint64_t & raceIdOut
)
{
    Poco::JSON::Object jsonBody;
    Poco::JSON::Array::Ptr participantsIds = new Poco::JSON::Array;
    for (RGT::Devkit::TestTools::User * const p : participants) {
        participantsIds->add(p->getId());
    }
    jsonBody.set("participants", participantsIds);

    Poco::JSON::Array::Ptr judgesIds = new Poco::JSON::Array;
    judgesIds->add(judge.getId());
    jsonBody.set("judges", judgesIds);

    Poco::Net::HTTPClientSession session("127.0.0.1", 80);
    Poco::Net::HTTPRequest request = judge.getRequestBlank();
    request.setMethod("POST");
    request.setURI("/management/create_race");

    std::ostringstream bodyStream;
    jsonBody.stringify(bodyStream);
    const std::string body = bodyStream.str();
    request.setContentLength(body.size());
    request.setContentType("application/json");

    std::ostream & os = session.sendRequest(request);
    os << body;

    Poco::Net::HTTPResponse response;
    std::istream & is = session.receiveResponse(response);
    std::string stringResponse;
    Poco::StreamCopier::copyToString(is, stringResponse);

    ASSERT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_CREATED)
        << "nginx->management вернул не 201: проверь, что контейнеры nginx и management запущены, "
           "слушается :80 и в логах management нет падений при create_race. Тело ответа: "
        << stringResponse;

    Poco::JSON::Parser parser;
    Poco::JSON::Object::Ptr result = parser.parse(stringResponse).extract<Poco::JSON::Object::Ptr>();
    ASSERT_TRUE(result->has("id"));
    raceIdOut = result->get("id").convert<uint64_t>();
    ASSERT_GT(raceIdOut, 0u) << "create_race must return a real race id, not 0. Body: " << stringResponse;
}

void postManagementOk(RGT::Devkit::TestTools::User & judge, const std::string & uri, uint64_t raceId)
{
    Poco::JSON::Object jsonBody;
    jsonBody.set("race_id", raceId);

    const std::string raw = doHttpJson(judge, Poco::Net::HTTPRequest::HTTP_POST, uri, jsonBody);
    const size_t newline = raw.find('\n');
    ASSERT_NE(newline, std::string::npos);
    const int status = std::stoi(raw.substr(0, newline));
    const std::string jsonStr = raw.substr(newline + 1);

    ASSERT_EQ(status, static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK))
        << uri << ": ожидался HTTP 200. Тело ответа: " << jsonStr;

    Poco::JSON::Parser parser;
    Poco::JSON::Object::Ptr result = parser.parse(jsonStr).extract<Poco::JSON::Object::Ptr>();
    EXPECT_EQ(result->getValue<std::string>("status"), "OK");
}

void uploadCoordinate
(
    RGT::Devkit::TestTools::User & participant,
    const std::string & timeIso,
    double longitude,
    double latitude
)
{
    Poco::JSON::Object jsonBody;
    jsonBody.set("time", timeIso);
    jsonBody.set("longitude", longitude);
    jsonBody.set("latitude", latitude);

    const std::string raw = doHttpJson(
        participant,
        Poco::Net::HTTPRequest::HTTP_POST,
        "/receiver/upload",
        jsonBody);
    const size_t newline = raw.find('\n');
    ASSERT_NE(newline, std::string::npos);
    const int status = std::stoi(raw.substr(0, newline));
    const std::string jsonStr = raw.substr(newline + 1);

    EXPECT_EQ(status, static_cast<int>(Poco::Net::HTTPResponse::HTTP_OK));

    Poco::JSON::Parser parser;
    Poco::JSON::Object::Ptr result = parser.parse(jsonStr).extract<Poco::JSON::Object::Ptr>();
    EXPECT_EQ(result->getValue<std::string>("status"), "OK");
    const std::string message = result->getValue<std::string>("message");
    EXPECT_EQ(message, "OK") << "coordinates must be persisted while the race is in progress";
}

bool waitRaceFinished(uint64_t raceId, std::chrono::milliseconds totalTimeout)
{
    const auto deadline = std::chrono::steady_clock::now() + totalTimeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        Poco::Data::Session session = RGT::Devkit::TestTools::ConnectionRegistry::instance().getPsqlPool().get();
        std::string statusText;
        uint64_t raceIdMutable = raceId;
        try
        {
            session << "SELECT status::text FROM races WHERE id = $1",
                Poco::Data::Keywords::use(raceIdMutable),
                Poco::Data::Keywords::into(statusText),
                Poco::Data::Keywords::now;
        }
        catch (...)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        if (statusText == "finished") {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}

void runFiveParticipantsGpxFinishScenario
(
    RGT::Devkit::TestTools::User & trainer,
    std::array<RGT::Devkit::TestTools::User *, 5> participants
)
{
    uint64_t raceId = 0;
    createRace(trainer, participants, raceId);

    postManagementOk(trainer, "/management/start_race", raceId);

    for (size_t i = 0; i < participants.size(); ++i)
    {
        const std::string path = std::format("{}/regatta_boat_{}.gpx", GPX_FILES_DIR, i + 1);
        const std::vector<GpxPoint> track = parseGpxFile(path);
        for (const GpxPoint & pt : track) {
            uploadCoordinate(*participants[i], pt.timeIso, pt.longitude, pt.latitude);
        }
    }

    postManagementOk(trainer, "/management/end_race", raceId);

    ASSERT_TRUE(waitRaceFinished(raceId, std::chrono::minutes(2)))
        << "race-postprocessor should set races.status to finished";

    Poco::Data::Session verifySession = RGT::Devkit::TestTools::ConnectionRegistry::instance().getPsqlPool().get();
    std::string finalStatus;
    uint64_t raceIdForSql = raceId;
    verifySession
        << "SELECT status::text FROM races WHERE id = $1",
        Poco::Data::Keywords::use(raceIdForSql),
        Poco::Data::Keywords::into(finalStatus),
        Poco::Data::Keywords::now;
    EXPECT_EQ(finalStatus, "finished");

    ASSERT_GT(raceIdForSql, 0u);
    std::this_thread::sleep_for(std::chrono::minutes(1));

    verifySession
        << "DELETE FROM races WHERE id = $1",
        Poco::Data::Keywords::use(raceIdForSql),
        Poco::Data::Keywords::now;
}

} // namespace

TEST(IntegrationRace, trainer_five_participants_gpx_finish)
{
    RGT::Devkit::readDotEnv();

    // Логин: только буквы латиницы и цифры, первый символ — буква (RegisterHandler::validateLogin).
    const auto stampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto stamp = static_cast<std::make_unsigned_t<decltype(stampMs)>>(stampMs);
    const std::string loginRoot = std::format("racex{}", stamp);

    RGT::Devkit::TestTools::User trainer
    (
        "Trainer",
        "Integration",
        loginRoot + "judge",
        RGT::Devkit::TestTools::Role::Judge
    );

    RGT::Devkit::TestTools::User p1("P1", "Boat", loginRoot + "p1", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p2("P2", "Boat", loginRoot + "p2", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p3("P3", "Boat", loginRoot + "p3", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p4("P4", "Boat", loginRoot + "p4", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p5("P5", "Boat", loginRoot + "p5", RGT::Devkit::TestTools::Role::Participant);

    std::array<RGT::Devkit::TestTools::User *, 5> participants = {&p1, &p2, &p3, &p4, &p5};
    runFiveParticipantsGpxFinishScenario(trainer, participants);
}

TEST(IntegrationRace, five_participants_gpx_finish_with_existing_antonio123)
{
    RGT::Devkit::readDotEnv();

    std::optional<std::string> existingLogin = RGT::Devkit::getEnv("INTEGRATION_EXISTING_LOGIN");
    std::optional<std::string> existingPassword = RGT::Devkit::getEnv("INTEGRATION_EXISTING_PASSWORD");
    ASSERT_TRUE(existingLogin.has_value() and not existingLogin->empty());
    ASSERT_TRUE(existingPassword.has_value() and not existingPassword->empty());

    const auto stampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto stamp = static_cast<std::make_unsigned_t<decltype(stampMs)>>(stampMs);
    const std::string loginRoot = std::format("racex{}", stamp);

    RGT::Devkit::TestTools::User trainer
    (
        "Trainer",
        "Integration",
        loginRoot + "judge",
        RGT::Devkit::TestTools::Role::Judge
    );

    RGT::Devkit::TestTools::User antonio = RGT::Devkit::TestTools::User::fromExisting(*existingLogin, *existingPassword);
    assertUserHasRole(antonio.getId(), "participant");

    RGT::Devkit::TestTools::User p2("P2", "Boat", loginRoot + "p2", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p3("P3", "Boat", loginRoot + "p3", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p4("P4", "Boat", loginRoot + "p4", RGT::Devkit::TestTools::Role::Participant);
    RGT::Devkit::TestTools::User p5("P5", "Boat", loginRoot + "p5", RGT::Devkit::TestTools::Role::Participant);

    std::array<RGT::Devkit::TestTools::User *, 5> participants = {&antonio, &p2, &p3, &p4, &p5};
    runFiveParticipantsGpxFinishScenario(trainer, participants);
}

} // namespace RGT::Main::Tests
