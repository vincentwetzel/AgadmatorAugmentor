#pragma once

#include "OpeningFetcher.h"
#include "ChessVideoExtractor.h"
#include <QObject>
#include <QString>
#include <atomic>
#include <vector>
#include <string>
#include <functional>

namespace cta {

struct LichessSyncResults {
    std::string finalEco;
    std::string finalOpeningName;
    std::vector<std::string> videoOpeningNames;
    LichessGameMetadata gameMetadata;
};

class LichessSyncHelper : public QObject {
    Q_OBJECT

public:
    explicit LichessSyncHelper(QObject* parent = nullptr);
    ~LichessSyncHelper() override = default;

    std::function<void(const std::string&)> getFenCallback();
    
    void waitAndProcess(const GameData& gameData, std::atomic<bool>* cancelFlag);
    const LichessSyncResults& getResults() const { return results_; }

signals:
    void logMessage(const QString& message);

private:
    OpeningFetcher opening_fetcher_;
    LichessSyncResults results_;
};

} // namespace cta
