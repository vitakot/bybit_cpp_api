#include "vk/bybit/bybit.h"
#include "vk/bybit/v2/bybit_futures_rest_client_v2.h"
#include "vk/utils/json_utils.h"
#include "vk/utils/log_utils.h"
#include "vk/bybit/v2/bybit_ws_stream_manager_v2.h"
#include "vk/utils/utils.h"
#include <memory>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <thread>
#include <spdlog/spdlog.h>
#include <future>

using namespace vk::bybit;
using namespace vk::bybit::v2;
using namespace std::chrono_literals;

constexpr int HISTORY_LENGTH_IN_S = 86400; // 1 day

void logFunction(const vk::LogSeverity severity, const std::string& errmsg) {
    switch (severity) {
    case vk::LogSeverity::Info:
        spdlog::info(errmsg);
        break;
    case vk::LogSeverity::Warning:
        spdlog::warn(errmsg);
        break;
    case vk::LogSeverity::Critical:
        spdlog::critical(errmsg);
        break;
    case vk::LogSeverity::Error:
        spdlog::error(errmsg);
        break;
    case vk::LogSeverity::Debug:
        spdlog::debug(errmsg);
        break;
    case vk::LogSeverity::Trace:
        spdlog::trace(errmsg);
        break;
    }
}

std::pair<std::string, std::string> readCredentials() {
    std::filesystem::path pathToCfg{"PATH_TO_CFG_FILE"};
    std::ifstream ifs(pathToCfg.string());

    if (!ifs.is_open()) {
        std::cerr << "Couldn't open config file: " + pathToCfg.string();
        return {};
    }

    try {
        std::string apiKey;
        std::string apiSecret;

        nlohmann::json json = nlohmann::json::parse(ifs);
        vk::readValue<std::string>(json, "ApiKey", apiKey);
        vk::readValue<std::string>(json, "ApiSecret", apiSecret);

        std::pair retVal(apiKey, apiSecret);
        return retVal;
    }
    catch (const std::exception& e) {
        std::cerr << e.what();
        ifs.close();
    }

    return {};
}

bool checkCandles(const std::vector<Candle>& candles, const CandleInterval interval) {
    const auto secs = Bybit::numberOfMsForCandleInterval(interval) / 1000;

    for (auto i = 0; i < candles.size() - 1; i++) {
        if (const auto timeDiff = candles[i + 1].m_openTime - candles[i].m_openTime; timeDiff != secs) {
            return false;
        }
    }

    return true;
}

void testHistory() {
    try {
        const auto [fst, snd] = readCredentials();
        const auto restClient = std::make_unique<futures::RESTClient>(fst, snd);
        const auto from = std::chrono::seconds(std::time(nullptr)).count() - HISTORY_LENGTH_IN_S;
        const auto to = from + 4 * 60 * 60;

        if (const auto candles = restClient->getHistoricalPrices("BTCUSDT", CandleInterval::_1, from, to); checkCandles(candles, CandleInterval::_1)) {
            logFunction(vk::LogSeverity::Info, "Candles OK");
        } else {
            logFunction(vk::LogSeverity::Error, "Candles Not OK");
        }
    }
    catch (std::exception& e) {
        logFunction(vk::LogSeverity::Critical, e.what());
    }
}

void testWebsockets() {
    const std::shared_ptr wsManager = std::make_unique<futures::WSStreamManager>();
    wsManager->setLoggerCallback(&logFunction);

    wsManager->subscribeInstrumentInfoStream("BTCUSDT");

    while (true) {
        {
            if (const auto ret = wsManager->readInstrumentInfo("BTCUSDT")) {
                std::cout << "BTC price: " << ret->m_lastPrice << std::endl;
            }
            else {
                std::cout << "Error" << std::endl;
            }
        }
        {
            if (const auto ret = wsManager->readInstrumentInfo("ETHUSDT")) {
                std::cout << "ETH price: " << ret->m_ask1Price << std::endl;
            }
            else {
                std::cout << "Error" << std::endl;
            }
        }
        std::this_thread::sleep_for(1000ms);
    }
}

void measureRestResponses() {
    const auto [fst, snd] = readCredentials();
    const auto restClient = std::make_shared<futures::RESTClient>(fst, snd);

    using std::chrono::high_resolution_clock;
    using std::chrono::duration_cast;
    using std::chrono::duration;
    using std::chrono::milliseconds;

    double overallTime = 0.0;
    int numPass = 0;

    while (true) {
        try {
            auto t1 = high_resolution_clock::now();
            auto pr = restClient->getWalletBalance("USDT");
            auto t2 = high_resolution_clock::now();

            duration<double, std::milli> ms_double = t2 - t1;
            logFunction(vk::LogSeverity::Info,
                        fmt::format("Get Wallet Balance request time: {} ms", ms_double.count()));
            overallTime += ms_double.count();

            t1 = high_resolution_clock::now();
            auto ex = restClient->getSymbols(true);
            t2 = high_resolution_clock::now();

            ms_double = t2 - t1;
            logFunction(vk::LogSeverity::Info, fmt::format("Get symbols request time: {} ms", ms_double.count()));
            overallTime += ms_double.count();

            t1 = high_resolution_clock::now();
            const auto account = restClient->getPositionInfo("BTCUSDT");
            t2 = high_resolution_clock::now();

            ms_double = t2 - t1;
            logFunction(vk::LogSeverity::Info,
                        fmt::format("Get position info request time: {} ms\n", ms_double.count()));
            overallTime += ms_double.count();
            numPass++;

            double timePerResponse = overallTime / (numPass * 3);
            logFunction(vk::LogSeverity::Info, fmt::format("Average time per response: {} ms\n", timePerResponse));
        }
        catch (std::exception& e) {
            logFunction(vk::LogSeverity::Warning, fmt::format("Exception: {}", e.what()));
        }

        std::this_thread::sleep_for(2s);
    }
}

double round_to(const double value, const double precision = 1.0) {
    return std::round(value / precision) * precision;
}

void replaceAll(std::string& s, const std::string& search, const std::string& replace) {
    for (size_t pos = 0;; pos += replace.length()) {
        pos = s.find(search, pos);

        if (pos == std::string::npos)
            break;

        s.erase(pos, search.length());
        s.insert(pos, replace);
    }
}

void positions() {
    const auto [fst, snd] = readCredentials();
    const auto restClient = std::make_shared<futures::RESTClient>(fst, snd);

    try {
        for (const auto& position : restClient->getPositionInfo()) {
            if (position.m_size != 0) {
                const auto ts = vk::getMsTimestamp(vk::currentTime()).count();
                Order order;
                order.m_symbol = position.m_symbol;

                if (position.m_side == Side::Buy) {
                    order.m_side = Side::Sell;
                }
                else {
                    order.m_side = Side::Buy;
                }

                order.m_orderType = OrderType::Market;
                order.m_qty = position.m_size;
                order.m_timeInForce = TimeInForce::GoodTillCancel;
                order.m_orderLinkId = std::to_string(ts);
                order.m_positionIdx = position.m_positionIdx;
                restClient->sendOrder(order);
            }
        }
    }
    catch (std::exception& e) {
        logFunction(vk::LogSeverity::Warning, fmt::format("Exception: {}", e.what()));
    }
}


void fundingRates() {
    const auto [fst, snd] = readCredentials();
    const auto restClient = std::make_shared<futures::RESTClient>(fst, snd);
    const auto fr = restClient->getLastFundingRate("BTCUSDT");
}

void testOrders() {
    const auto [fst, snd] = readCredentials();
    const auto restClient = std::make_shared<futures::RESTClient>(fst, snd);

    try {
        const auto ts = vk::getMsTimestamp(vk::currentTime()).count();

        double lotAmount = 0.1;
        constexpr int amount = -25;

        Order order;
        order.m_symbol = "DOTUSDT";
        order.m_side = Side::Buy;
        order.m_orderType = OrderType::Market;
        order.m_qty = lotAmount * std::abs(amount);
        order.m_timeInForce = TimeInForce::GoodTillCancel;
        order.m_orderLinkId = std::to_string(ts);

        auto orderResponse = restClient->sendOrder(order);

        int attemptNo = 0;

        while (orderResponse.m_orderStatus != OrderStatus::Active &&
            orderResponse.m_orderStatus != OrderStatus::Filled) {
            const auto activeOrder = restClient->queryActiveOrder(orderResponse.m_symbol, orderResponse.m_orderId,
                                                                  orderResponse.m_orderLinkId);

            if (activeOrder) {
                orderResponse.m_orderStatus = activeOrder->m_orderStatus;
                orderResponse.m_lastExecPrice = activeOrder->m_lastExecPrice;
                orderResponse.m_cumExecQty = activeOrder->m_cumExecQty;
            }

            attemptNo++;
            auto ordRespString = orderResponse.toJson().dump();

            if (constexpr int maxAttempts = 10; attemptNo == maxAttempts) {
                spdlog::error(
                    "Cannot send order to server, reason: order was not filled, order response: {}, active order: {}",
                    orderResponse.toJson().dump(), activeOrder->toJson().dump());
                return;
            }

            std::this_thread::sleep_for(500ms);
        }
    }
    catch (std::exception& e) {
        logFunction(vk::LogSeverity::Warning, fmt::format("Exception: {}", e.what()));
    }
}

void setPositionMode() {
    const auto [fst, snd] = readCredentials();
    const auto restClient = std::make_shared<futures::RESTClient>(fst, snd);
    restClient->setPositionMode("", "USDT", PositionMode::MergedSingle);
}

int main() {
    // measureRestResponses();
    // testWebsockets();
    // setPositionMode();
    // positions();
    // fundingRates();
    // testOrders();
    testHistory();
    return getchar();
}
