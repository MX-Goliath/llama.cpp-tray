#include <QAction>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>

namespace {

const QSet<QString> kActiveStatuses = {"loaded", "loading", "sleeping"};
constexpr auto kLoadingIndicatorTimeout = std::chrono::seconds(120);
const QString kDefaultCustomArgs = QStringLiteral("--listen localhost:8082 --watch-config");

struct ModelInfo {
    QString modelId{};
    QString title{};
    QString status{};
    QString details{};
};

struct AppSettings {
    QString host{};
    QString configPath{};
    QString customArgs{};
};

struct RequestResult {
    int statusCode = -1;
    QByteArray body{};
    QString error{};
};

struct RefreshResult {
    QList<ModelInfo> loadedModels{};
    QList<ModelInfo> availableModels{};
    QString error{};
};

struct LoadResult {
    QString modelId{};
    QString error{};
};

struct ActionResult {
    QString error{};
};

QString displayName(const QString &modelId)
{
    QString normalized = modelId;
    normalized.replace('\\', '/');
    while (normalized.endsWith('/')) {
        normalized.chop(1);
    }

    const int slash = normalized.lastIndexOf('/');
    if (slash >= 0 && slash + 1 < normalized.size()) {
        return normalized.mid(slash + 1);
    }

    const QFileInfo info(modelId);
    return info.fileName().isEmpty() ? modelId : info.fileName();
}

QString normalizeBaseUrl(const QString &host)
{
    QString cleaned = host.trimmed();
    if (cleaned.isEmpty()) {
        cleaned = QStringLiteral("127.0.0.1:8082");
    }
    if (!cleaned.contains(QStringLiteral("://"))) {
        cleaned.prepend(QStringLiteral("http://"));
    }
    while (cleaned.endsWith('/')) {
        cleaned.chop(1);
    }
    return cleaned;
}

QString defaultConfigPath()
{
    return QDir::home().filePath(QStringLiteral(".models/config.yaml"));
}

class Translations {
public:
    static Translations &instance()
    {
        static Translations translations;
        return translations;
    }

    void load(const QString &language)
    {
        const QString normalized = language.trimmed().isEmpty() ? QStringLiteral("en") : language.trimmed().toLower();
        if (loadLanguage(normalized)) {
            return;
        }

        const QString baseLanguage = normalized.section('-', 0, 0).section('_', 0, 0);
        if (baseLanguage != normalized && loadLanguage(baseLanguage)) {
            return;
        }

        loadLanguage(QStringLiteral("en"));
    }

    [[nodiscard]] QString text(const QString &key) const
    {
        return m_values.value(key, key);
    }

private:
    bool loadLanguage(const QString &language)
    {
        const QString fileName = language + QStringLiteral(".json");
        const QStringList paths = {
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("translations/") + fileName),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../translations/") + fileName),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../translations/") + fileName),
            QStringLiteral(":/translations/") + fileName,
        };

        for (const QString &path : paths) {
            if (loadFile(path)) {
                return true;
            }
        }

        return false;
    }

    bool loadFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return false;
        }

        QHash<QString, QString> loaded;
        const QJsonObject object = document.object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.value().isString()) {
                loaded.insert(it.key(), it.value().toString());
            }
        }

        m_values = std::move(loaded);
        return true;
    }

    QHash<QString, QString> m_values;
};

QString t(const char *key)
{
    return Translations::instance().text(QString::fromUtf8(key));
}

class LlamaApiError final : public std::runtime_error {
public:
    explicit LlamaApiError(const QString &message)
        : std::runtime_error(message.toStdString()), m_message(message)
    {
    }

    [[nodiscard]] const QString &message() const
    {
        return m_message;
    }

private:
    QString m_message;
};

class BlockingHttpClient {
public:
    RequestResult request(const QString &method,
                          const QUrl &url,
                          const QList<int> &expectedCodes,
                          const std::optional<QByteArray> &jsonBody = std::nullopt,
                          int timeoutMs = 30000)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(url);
        request.setTransferTimeout(timeoutMs);

        QNetworkReply *reply = nullptr;
        if (method == QStringLiteral("GET")) {
            reply = manager.get(request);
        } else if (method == QStringLiteral("POST")) {
            if (jsonBody.has_value()) {
                request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
                reply = manager.post(request, *jsonBody);
            } else {
                reply = manager.post(request, QByteArray{});
            }
        } else {
            return RequestResult{.error = QStringLiteral("Unsupported HTTP method: %1").arg(method)};
        }

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, reply, [reply]() {
            if (reply->isRunning()) {
                reply->abort();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();

        RequestResult result;
        result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();

        if (reply->error() != QNetworkReply::NoError && reply->error() != QNetworkReply::OperationCanceledError) {
            result.error = reply->errorString();
        } else if (reply->error() == QNetworkReply::OperationCanceledError && timer.isActive()) {
            result.error = reply->errorString();
        } else if (!timer.isActive() && result.statusCode <= 0) {
            result.error = QStringLiteral("Request timed out");
        }

        reply->deleteLater();

        if (!result.error.isEmpty()) {
            return result;
        }

        if (!expectedCodes.contains(result.statusCode)) {
            QString body = QString::fromUtf8(result.body).trimmed();
            if (body.size() > 300) {
                body = body.left(297) + QStringLiteral("...");
            }
            result.error = QStringLiteral("%1 %2 -> HTTP %3: %4")
                               .arg(method, url.path(QUrl::FullyEncoded))
                               .arg(result.statusCode)
                               .arg(body);
        }

        return result;
    }
};

class LlamaApi {
public:
    explicit LlamaApi(QString baseUrl)
        : m_baseUrl(std::move(baseUrl))
    {
    }

    [[nodiscard]] QString baseUrl() const
    {
        return m_baseUrl;
    }

    void setBaseUrl(const QString &baseUrl)
    {
        m_baseUrl = baseUrl;
    }

    QList<ModelInfo> getLoadedModels()
    {
        QStringList errors;

        try {
            return getModelsLlamaSwapRunning();
        } catch (const LlamaApiError &error) {
            errors.push_back(error.message());
        }

        try {
            return getModelsRouterMode();
        } catch (const LlamaApiError &error) {
            errors.push_back(error.message());
        }

        try {
            return getModelsSingleMode();
        } catch (const LlamaApiError &error) {
            errors.push_back(error.message());
        }

        try {
            return getModelsFromProps();
        } catch (const LlamaApiError &error) {
            errors.push_back(error.message());
        }

        throw LlamaApiError(errors.join(QStringLiteral("; ")));
    }

    QList<ModelInfo> getAvailableModels()
    {
        const QJsonDocument payload = getJson(QStringLiteral("/v1/models"));
        if (!payload.isObject() || !payload.object().value(QStringLiteral("data")).isArray()) {
            throw LlamaApiError(QStringLiteral("GET /v1/models returned unexpected payload"));
        }

        QList<ModelInfo> models;
        const auto items = payload.object().value(QStringLiteral("data")).toArray();
        for (const QJsonValue &itemValue : items) {
            if (!itemValue.isObject()) {
                continue;
            }
            const QJsonObject item = itemValue.toObject();
            const QString modelId = item.value(QStringLiteral("id")).toString(QStringLiteral("unknown-model"));
            const QString title = displayName(modelId);

            QStringList details;
            if (title != modelId) {
                details.push_back(modelId);
            }

            const QString name = item.value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty() && name != title) {
                details.push_back(name);
            }

            const QString description = item.value(QStringLiteral("description")).toString().trimmed();
            if (!description.isEmpty()) {
                details.push_back(description);
            }

            models.push_back(ModelInfo{
                .modelId = modelId,
                .title = title,
                .status = QStringLiteral("available"),
                .details = details.join(QStringLiteral(" | ")),
            });
        }
        return models;
    }

    void loadModel(const QString &modelId)
    {
        const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(modelId));
        const QList<QPair<QString, QString>> attempts = {
            {QStringLiteral("GET"), QStringLiteral("/upstream/%1/health").arg(encoded)},
            {QStringLiteral("GET"), QStringLiteral("/upstream/%1/props").arg(encoded)},
            {QStringLiteral("GET"), QStringLiteral("/upstream/%1/v1/models").arg(encoded)},
            {QStringLiteral("GET"), QStringLiteral("/upstream/%1/").arg(encoded)},
        };

        QStringList errors;
        for (const auto &[method, path] : attempts) {
            const RequestResult response = request(method, path, {200, 204, 404, 405, 502, 503});
            if (!response.error.isEmpty()) {
                errors.push_back(response.error);
                continue;
            }
            if (response.statusCode == 200 || response.statusCode == 204) {
                return;
            }
            errors.push_back(QStringLiteral("%1 %2 -> HTTP %3").arg(method, path).arg(response.statusCode));
        }

        throw LlamaApiError(QStringLiteral("Failed to load model through llama-swap. Details: %1")
                                .arg(errors.join(QStringLiteral("; "))));
    }

    void unloadModel(const QString &modelId)
    {
        const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(modelId));
        const QList<QPair<QString, std::optional<QByteArray>>> attempts = {
            {QStringLiteral("/api/models/unload/%1").arg(encoded), std::nullopt},
            {QStringLiteral("/models/unload"), toJsonBody(modelId)},
            {QStringLiteral("/v1/models/unload"), toJsonBody(modelId)},
            {QStringLiteral("/unload"), toJsonBody(modelId)},
        };

        QStringList errors;
        for (const auto &[path, payload] : attempts) {
            const RequestResult response = request(QStringLiteral("POST"), path, {200, 202, 204, 404, 405}, payload);
            if (!response.error.isEmpty()) {
                errors.push_back(response.error);
                continue;
            }

            if (response.statusCode == 404 || response.statusCode == 405) {
                errors.push_back(QStringLiteral("POST %1 -> HTTP %2").arg(path).arg(response.statusCode));
                continue;
            }

            if (response.statusCode == 204 || response.body.trimmed().isEmpty()) {
                return;
            }

            const QJsonDocument json = QJsonDocument::fromJson(response.body);
            if (!json.isObject()) {
                return;
            }

            const QJsonObject object = json.object();
            if (object.value(QStringLiteral("success")).toBool()) {
                return;
            }

            if (object.value(QStringLiteral("error")).isObject()) {
                const QJsonObject errorObject = object.value(QStringLiteral("error")).toObject();
                const QString message = errorObject.value(QStringLiteral("message")).toString(QString::fromUtf8(response.body));
                throw LlamaApiError(QStringLiteral("POST %1 failed: %2").arg(path, message));
            }

            return;
        }

        throw LlamaApiError(
            QStringLiteral("Unload endpoint is unavailable. This usually means llama-server is not running in router mode. Details: %1")
                .arg(errors.join(QStringLiteral("; "))));
    }

private:
    [[nodiscard]] QByteArray toJsonBody(const QString &modelId) const
    {
        const QJsonObject payload{{QStringLiteral("model"), modelId}};
        return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    }

    RequestResult request(const QString &method,
                          const QString &path,
                          const QList<int> &expectedCodes,
                          const std::optional<QByteArray> &jsonBody = std::nullopt)
    {
        BlockingHttpClient client;
        return client.request(method, QUrl(m_baseUrl + path), expectedCodes, jsonBody);
    }

    QJsonDocument getJson(const QString &path)
    {
        const RequestResult response = request(QStringLiteral("GET"), path, {200});
        if (!response.error.isEmpty()) {
            throw LlamaApiError(response.error);
        }

        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(response.body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            throw LlamaApiError(QStringLiteral("GET %1 returned invalid JSON").arg(path));
        }
        return json;
    }

    QList<ModelInfo> getModelsLlamaSwapRunning()
    {
        const RequestResult response = request(QStringLiteral("GET"), QStringLiteral("/running"), {200});
        if (!response.error.isEmpty()) {
            throw LlamaApiError(response.error);
        }

        const QByteArray body = response.body.trimmed();
        if (body.isEmpty()) {
            return {};
        }

        QJsonParseError parseError;
        const QJsonDocument json = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            return parseRunningText(QString::fromUtf8(body));
        }

        return parseRunningPayload(json.isObject() ? QJsonValue(json.object()) : QJsonValue(json.array()));
    }

    QList<ModelInfo> parseRunningPayload(const QJsonValue &payload)
    {
        if (payload.isObject()) {
            const QJsonObject object = payload.toObject();
            if (object.value(QStringLiteral("running")).isArray()) {
                return parseRunningPayload(object.value(QStringLiteral("running")));
            }
            if (object.value(QStringLiteral("data")).isArray()) {
                return parseRunningPayload(object.value(QStringLiteral("data")));
            }
            if (object.value(QStringLiteral("models")).isArray()) {
                return parseRunningPayload(object.value(QStringLiteral("models")));
            }
        }

        if (!payload.isArray()) {
            throw LlamaApiError(QStringLiteral("GET /running returned unexpected payload"));
        }

        QList<ModelInfo> models;
        const QJsonArray array = payload.toArray();
        for (const QJsonValue &itemValue : array) {
            if (itemValue.isString()) {
                const QString modelId = itemValue.toString().trimmed();
                if (!modelId.isEmpty()) {
                    models.push_back(ModelInfo{.modelId = modelId, .title = displayName(modelId), .status = QStringLiteral("ready")});
                }
                continue;
            }

            if (!itemValue.isObject()) {
                continue;
            }

            const QJsonObject item = itemValue.toObject();
            const QString modelId = firstNonEmpty({
                item.value(QStringLiteral("id")).toString(),
                item.value(QStringLiteral("model")).toString(),
                item.value(QStringLiteral("model_id")).toString(),
                item.value(QStringLiteral("name")).toString(),
            });

            if (modelId.isEmpty()) {
                continue;
            }

            const QString status = firstNonEmpty({
                                        item.value(QStringLiteral("state")).toString(),
                                        item.value(QStringLiteral("status")).toString(),
                                        QStringLiteral("ready"),
                                    })
                                       .trimmed()
                                       .toLower();
            if (status == QStringLiteral("shutdown") || status == QStringLiteral("stopped") || status == QStringLiteral("unknown")) {
                continue;
            }

            QStringList details;
            const QString title = displayName(modelId);
            if (title != modelId) {
                details.push_back(modelId);
            }

            const QString description = item.value(QStringLiteral("description")).toString().trimmed();
            if (!description.isEmpty()) {
                details.push_back(description);
            }

            const QString peerId = firstNonEmpty({
                item.value(QStringLiteral("peerID")).toString(),
                item.value(QStringLiteral("peerId")).toString(),
            });
            if (!peerId.isEmpty()) {
                details.push_back(QStringLiteral("peer:%1").arg(peerId));
            }

            models.push_back(ModelInfo{
                .modelId = modelId,
                .title = title,
                .status = status,
                .details = details.join(QStringLiteral(" | ")),
            });
        }

        return models;
    }

    QList<ModelInfo> parseRunningText(const QString &text)
    {
        QList<ModelInfo> models;
        const auto lines = text.split('\n');
        for (const QString &line : lines) {
            QString cleaned = line.trimmed();
            while (cleaned.endsWith(',')) {
                cleaned.chop(1);
            }
            while (cleaned.startsWith('-')) {
                cleaned.remove(0, 1);
                cleaned = cleaned.trimmed();
            }
            if (cleaned.isEmpty()) {
                continue;
            }
            models.push_back(ModelInfo{
                .modelId = cleaned,
                .title = displayName(cleaned),
                .status = QStringLiteral("ready"),
            });
        }
        return models;
    }

    QList<ModelInfo> getModelsRouterMode()
    {
        const QJsonDocument payload = getJson(QStringLiteral("/models"));
        if (!payload.isObject() || !payload.object().value(QStringLiteral("data")).isArray()) {
            throw LlamaApiError(QStringLiteral("GET /models returned unexpected payload"));
        }

        QList<ModelInfo> models;
        for (const QJsonValue &itemValue : payload.object().value(QStringLiteral("data")).toArray()) {
            if (!itemValue.isObject()) {
                continue;
            }

            const QJsonObject item = itemValue.toObject();
            const QString status = item.value(QStringLiteral("status")).toObject().value(QStringLiteral("value")).toString(QStringLiteral("unknown"));
            if (!kActiveStatuses.contains(status)) {
                continue;
            }

            const QString modelId = firstNonEmpty({
                item.value(QStringLiteral("id")).toString(),
                item.value(QStringLiteral("path")).toString(),
                QStringLiteral("unknown-model"),
            });
            const QString title = displayName(modelId);

            QStringList details;
            if (title != modelId) {
                details.push_back(modelId);
            }
            const QString path = item.value(QStringLiteral("path")).toString();
            if (!path.isEmpty()) {
                details.push_back(path);
            }
            if (item.contains(QStringLiteral("in_cache"))) {
                details.push_back(item.value(QStringLiteral("in_cache")).toBool() ? QStringLiteral("cache")
                                                                                  : QStringLiteral("local"));
            }

            models.push_back(ModelInfo{
                .modelId = modelId,
                .title = title,
                .status = status,
                .details = details.join(QStringLiteral(" | ")),
            });
        }
        return models;
    }

    QList<ModelInfo> getModelsSingleMode()
    {
        const QJsonDocument payload = getJson(QStringLiteral("/v1/models"));
        if (!payload.isObject() || !payload.object().value(QStringLiteral("data")).isArray()) {
            throw LlamaApiError(QStringLiteral("GET /v1/models returned unexpected payload"));
        }

        QList<ModelInfo> models;
        for (const QJsonValue &itemValue : payload.object().value(QStringLiteral("data")).toArray()) {
            if (!itemValue.isObject()) {
                continue;
            }
            const QJsonObject item = itemValue.toObject();
            const QString modelId = item.value(QStringLiteral("id")).toString(QStringLiteral("unknown-model"));
            const QString title = displayName(modelId);

            QStringList details;
            if (title != modelId) {
                details.push_back(modelId);
            }

            if (item.value(QStringLiteral("meta")).isObject()) {
                const QJsonObject meta = item.value(QStringLiteral("meta")).toObject();
                const qint64 size = static_cast<qint64>(meta.value(QStringLiteral("size")).toDouble());
                if (size > 0) {
                    const double sizeGiB = static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0);
                    details.push_back(QStringLiteral("%1 GiB").arg(QString::number(sizeGiB, 'f', 2)));
                }
            }

            models.push_back(ModelInfo{
                .modelId = modelId,
                .title = title,
                .status = QStringLiteral("loaded"),
                .details = details.join(QStringLiteral(" | ")),
            });
        }
        return models;
    }

    QList<ModelInfo> getModelsFromProps()
    {
        const QJsonDocument payload = getJson(QStringLiteral("/props"));
        if (!payload.isObject()) {
            throw LlamaApiError(QStringLiteral("GET /props returned unexpected payload"));
        }

        const QJsonObject object = payload.object();
        const QString modelId = firstNonEmpty({
            object.value(QStringLiteral("model_alias")).toString(),
            object.value(QStringLiteral("default_model")).toString(),
            object.value(QStringLiteral("model_path")).toString(),
            QStringLiteral("current-model"),
        });

        QString status = object.value(QStringLiteral("status")).toString(QStringLiteral("loaded"));
        if (!kActiveStatuses.contains(status)) {
            status = QStringLiteral("loaded");
        }

        const QString detailsValue = object.value(QStringLiteral("model_path")).toString().trimmed();
        const QString title = displayName(modelId);
        QStringList details;
        if (title != modelId) {
            details.push_back(modelId);
        }
        if (!detailsValue.isEmpty()) {
            details.push_back(detailsValue);
        }

        return {ModelInfo{
            .modelId = modelId,
            .title = title,
            .status = status,
            .details = details.join(QStringLiteral(" | ")),
        }};
    }

    QString firstNonEmpty(std::initializer_list<QString> values) const
    {
        for (const QString &value : values) {
            if (!value.trimmed().isEmpty()) {
                return value.trimmed();
            }
        }
        return {};
    }

    QString m_baseUrl;
};

class ModelsMenu final : public QMenu {
    Q_OBJECT

public:
    explicit ModelsMenu(QWidget *parent = nullptr)
        : QMenu(parent)
    {
        setToolTipsVisible(true);
    }

    void rebuild(const QList<ModelInfo> &loadedModels,
                 const QList<ModelInfo> &availableModels,
                 const QString &error,
                 const QString &serverLabel)
    {
        clear();

        if (!error.isEmpty()) {
            auto *header = addAction(t("menu.start_service"));
            header->setToolTip(t("tooltip.start_llama_swap"));
            connect(header, &QAction::triggered, this, &ModelsMenu::startServiceRequested);
        } else {
            auto *header = addAction(serverLabel);
            header->setToolTip(t("tooltip.open_web_ui"));
            connect(header, &QAction::triggered, this, &ModelsMenu::openUiRequested);
        }

        auto *refreshAction = addAction(t("menu.refresh"));
        connect(refreshAction, &QAction::triggered, this, &ModelsMenu::refreshRequested);

        addSeparator();

        if (!error.isEmpty()) {
            auto *hint = addAction(t("menu.start_hint"));
            hint->setEnabled(false);

            auto *errorAction = addAction(t("menu.connection_error"));
            errorAction->setEnabled(false);
            errorAction->setToolTip(error);
        } else {
            if (loadedModels.isEmpty()) {
                auto *empty = addAction(t("menu.no_loaded_models"));
                empty->setEnabled(false);
            } else {
                for (const ModelInfo &model : loadedModels) {
                    QString line = QStringLiteral("%1 [%2]").arg(model.title, model.status);
                    if (!model.details.isEmpty()) {
                        line += QStringLiteral(" | ") + model.details;
                    }
                    auto *action = addAction(line);
                    action->setToolTip(model.modelId);
                    action->setEnabled(model.status != QStringLiteral("loading"));
                    connect(action, &QAction::triggered, this, [this, modelId = model.modelId]() {
                        emit unloadRequested(modelId);
                    });
                }
            }

            if (!availableModels.isEmpty()) {
                QSet<QString> loadedIds;
                for (const ModelInfo &model : loadedModels) {
                    loadedIds.insert(model.modelId);
                }

                auto *loadMenu = addMenu(t("menu.load"));
                for (const ModelInfo &model : availableModels) {
                    auto *action = loadMenu->addAction(model.title);
                    action->setToolTip(model.details.isEmpty() ? model.modelId : model.details);
                    action->setEnabled(!loadedIds.contains(model.modelId));
                    connect(action, &QAction::triggered, this, [this, modelId = model.modelId]() {
                        emit loadRequested(modelId);
                    });
                }
            }
        }

        auto *stopAction = addAction(t("menu.stop_service"));
        stopAction->setEnabled(error.isEmpty());
        stopAction->setToolTip(t("tooltip.stop_llama_swap"));
        connect(stopAction, &QAction::triggered, this, &ModelsMenu::stopServiceRequested);

        auto *settingsMenu = addMenu(t("menu.settings"));

        auto *hostAction = settingsMenu->addAction(t("menu.host_address"));
        connect(hostAction, &QAction::triggered, this, &ModelsMenu::editHostRequested);

        auto *configAction = settingsMenu->addAction(t("menu.config_path"));
        connect(configAction, &QAction::triggered, this, &ModelsMenu::editConfigRequested);

        auto *openConfigAction = settingsMenu->addAction(t("menu.open_config"));
        connect(openConfigAction, &QAction::triggered, this, &ModelsMenu::openConfigRequested);

        auto *argsAction = settingsMenu->addAction(t("menu.launch_args"));
        connect(argsAction, &QAction::triggered, this, &ModelsMenu::editArgsRequested);

        auto *quitAction = addAction(t("menu.quit"));
        connect(quitAction, &QAction::triggered, this, &ModelsMenu::quitRequested);
    }

signals:
    void refreshRequested();
    void openUiRequested();
    void startServiceRequested();
    void stopServiceRequested();
    void editHostRequested();
    void editConfigRequested();
    void openConfigRequested();
    void editArgsRequested();
    void loadRequested(const QString &modelId);
    void unloadRequested(const QString &modelId);
    void quitRequested();
};

class TrayApp final : public QObject {
    Q_OBJECT

public:
    TrayApp(QApplication &application, LlamaApi api, int refreshIntervalMs, AppSettings initialSettings)
        : QObject(nullptr)
        , m_app(application)
        , m_api(std::move(api))
        , m_settingsStore(QStringLiteral("llama.cpp_statusbar"), QStringLiteral("tray_cpp"))
        , m_appSettings(std::move(initialSettings))
        , m_serverLabel(m_api.baseUrl())
    {
        m_app.setApplicationName(QStringLiteral("llama.cpp Tray C++"));
        m_app.setQuitOnLastWindowClosed(false);

        connect(&m_modelsMenu, &ModelsMenu::refreshRequested, this, &TrayApp::refreshModels);
        connect(&m_modelsMenu, &ModelsMenu::openUiRequested, this, &TrayApp::openUi);
        connect(&m_modelsMenu, &ModelsMenu::startServiceRequested, this, &TrayApp::startService);
        connect(&m_modelsMenu, &ModelsMenu::stopServiceRequested, this, &TrayApp::stopService);
        connect(&m_modelsMenu, &ModelsMenu::editHostRequested, this, &TrayApp::editHost);
        connect(&m_modelsMenu, &ModelsMenu::editConfigRequested, this, &TrayApp::editConfigPath);
        connect(&m_modelsMenu, &ModelsMenu::openConfigRequested, this, &TrayApp::openConfigInKate);
        connect(&m_modelsMenu, &ModelsMenu::editArgsRequested, this, &TrayApp::editCustomArgs);
        connect(&m_modelsMenu, &ModelsMenu::loadRequested, this, &TrayApp::loadModel);
        connect(&m_modelsMenu, &ModelsMenu::unloadRequested, this, &TrayApp::unloadModel);
        connect(&m_modelsMenu, &ModelsMenu::quitRequested, &m_app, &QApplication::quit);

        m_tray.setIcon(buildIcon());
        m_tray.setToolTip(t("tray.server").arg(m_serverLabel));
        m_tray.setContextMenu(&m_modelsMenu);
        connect(&m_tray, &QSystemTrayIcon::activated, this, &TrayApp::onTrayActivated);
        m_tray.show();

        m_refreshTimer.setInterval(refreshIntervalMs);
        connect(&m_refreshTimer, &QTimer::timeout, this, &TrayApp::refreshModels);
        m_refreshTimer.start();

        refreshModels();
    }

private slots:
    void editHost()
    {
        bool accepted = false;
        const QString value = QInputDialog::getText(nullptr,
                                                    t("dialog.host_title"),
                                                    t("dialog.host_prompt"),
                                                    QLineEdit::Normal,
                                                    m_appSettings.host,
                                                    &accepted);
        if (!accepted) {
            return;
        }

        const QString updated = value.trimmed();
        if (updated.isEmpty()) {
            QMessageBox::warning(nullptr, t("dialog.invalid_address_title"), t("dialog.empty_host"));
            return;
        }

        applySettings(AppSettings{
            .host = updated,
            .configPath = m_appSettings.configPath,
            .customArgs = m_appSettings.customArgs,
        });
        refreshModels();
    }

    void editConfigPath()
    {
        bool accepted = false;
        const QString value = QInputDialog::getText(nullptr,
                                                    t("dialog.config_title"),
                                                    t("dialog.config_prompt"),
                                                    QLineEdit::Normal,
                                                    m_appSettings.configPath,
                                                    &accepted);
        if (!accepted) {
            return;
        }

        const QString updated = value.trimmed();
        if (updated.isEmpty()) {
            QMessageBox::warning(nullptr, t("dialog.invalid_path_title"), t("dialog.empty_config_path"));
            return;
        }

        applySettings(AppSettings{
            .host = m_appSettings.host,
            .configPath = updated,
            .customArgs = m_appSettings.customArgs,
        });
    }

    void editCustomArgs()
    {
        bool accepted = false;
        const QString value = QInputDialog::getText(nullptr,
                                                    t("dialog.launch_args_title"),
                                                    t("dialog.launch_args_prompt"),
                                                    QLineEdit::Normal,
                                                    m_appSettings.customArgs,
                                                    &accepted);
        if (!accepted) {
            return;
        }

        applySettings(AppSettings{
            .host = m_appSettings.host,
            .configPath = m_appSettings.configPath,
            .customArgs = value.trimmed(),
        });
    }

    void openConfigInKate()
    {
        const QString configPath = m_appSettings.configPath.trimmed();
        if (configPath.isEmpty()) {
            QMessageBox::warning(nullptr, t("dialog.invalid_path_title"), t("dialog.config_path_not_set"));
            return;
        }

        if (!QFileInfo::exists(configPath)) {
            QMessageBox::warning(nullptr,
                                 t("dialog.file_not_found_title"),
                                 t("dialog.config_not_found").arg(configPath));
            return;
        }

        if (!QProcess::startDetached(QStringLiteral("kate"), {configPath})) {
            QMessageBox::critical(nullptr,
                                  t("dialog.open_config_failed_title"),
                                  t("dialog.kate_failed").arg(configPath));
        }
    }

    void refreshModels()
    {
        if (m_workerBusy) {
            m_refreshQueued = true;
            return;
        }

        m_workerBusy = true;
        m_refreshQueued = false;

        auto *watcher = new QFutureWatcher<RefreshResult>(this);
        connect(watcher, &QFutureWatcher<RefreshResult>::finished, this, [this, watcher]() {
            const RefreshResult result = watcher->result();
            watcher->deleteLater();
            m_workerBusy = false;

            if (!result.error.isEmpty()) {
                applyModelsState({}, {}, result.error);
            } else {
                applyModelsState(result.loadedModels, result.availableModels, {});
            }

            if (!m_workerBusy && m_refreshQueued) {
                refreshModels();
            }
        });

        watcher->setFuture(QtConcurrent::run([api = m_api]() mutable {
            RefreshResult result;
            try {
                result.loadedModels = api.getLoadedModels();
                result.availableModels = api.getAvailableModels();
            } catch (const LlamaApiError &error) {
                result.error = error.message();
            }
            return result;
        }));
    }

    void loadModel(const QString &modelId)
    {
        if (m_workerBusy) {
            m_refreshQueued = true;
            return;
        }

        m_pendingLoadingModelId = modelId;
        m_pendingLoadingStartedAt = QDateTime::currentDateTimeUtc();

        QList<ModelInfo> preview = m_currentLoadedModels;
        preview.push_front(ModelInfo{
            .modelId = modelId,
            .title = displayName(modelId),
            .status = QStringLiteral("loading"),
        });
        m_modelsMenu.rebuild(preview, m_currentAvailableModels, m_currentError, m_serverLabel);

        m_workerBusy = true;
        m_tray.setToolTip(t("tray.loading_model").arg(displayName(modelId)));

        auto *watcher = new QFutureWatcher<LoadResult>(this);
        connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            m_workerBusy = false;

            const LoadResult result = watcher->result();
            if (!result.error.isEmpty()) {
                m_pendingLoadingModelId.clear();
                m_pendingLoadingStartedAt = {};
                QMessageBox::critical(nullptr, t("dialog.load_model_failed_title"), result.error);
                return;
            }

            m_tray.showMessage(QStringLiteral("llama.cpp"),
                               t("notification.model_loaded").arg(displayName(result.modelId)),
                               QSystemTrayIcon::Information,
                               3000);
            refreshModels();
        });

        watcher->setFuture(QtConcurrent::run([api = m_api, modelId]() mutable -> LoadResult {
            LoadResult result;
            try {
                api.loadModel(modelId);
                result.modelId = modelId;
            } catch (const LlamaApiError &error) {
                result.error = error.message();
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
    }

    void unloadModel(const QString &modelId)
    {
        if (m_workerBusy) {
            m_refreshQueued = true;
            return;
        }

        m_workerBusy = true;
        m_tray.setToolTip(t("tray.unloading_model").arg(displayName(modelId)));

        auto *watcher = new QFutureWatcher<ActionResult>(this);
        connect(watcher, &QFutureWatcher<ActionResult>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            m_workerBusy = false;

            const ActionResult result = watcher->result();
            if (!result.error.isEmpty()) {
                QMessageBox::critical(nullptr, t("dialog.unload_model_failed_title"), result.error);
                return;
            }

            refreshModels();
        });

        watcher->setFuture(QtConcurrent::run([api = m_api, modelId]() mutable -> ActionResult {
            ActionResult result;
            try {
                api.unloadModel(modelId);
            } catch (const LlamaApiError &error) {
                result.error = error.message();
            } catch (const std::exception &error) {
                result.error = QString::fromUtf8(error.what());
            }
            return result;
        }));
    }

    void startService()
    {
        if (m_workerBusy) {
            m_refreshQueued = true;
            return;
        }

        m_workerBusy = true;
        m_tray.setToolTip(t("tray.starting_service"));

        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            m_workerBusy = false;
            const QString error = watcher->result();
            if (!error.isEmpty()) {
                QMessageBox::critical(nullptr, t("dialog.start_service_failed_title"), error);
                return;
            }

            m_currentError.clear();
            m_tray.setToolTip(t("tray.starting_service"));
            QTimer::singleShot(1500, this, &TrayApp::refreshModels);
        });

        const QString executablePath = resolveLlamaSwapExecutable();
        const QStringList command = currentStartCommandParts(executablePath);
        watcher->setFuture(QtConcurrent::run([command, executablePath]() -> QString {
            if (executablePath.isEmpty()) {
                return t("error.llama_swap_not_found");
            }

            const bool ok = QProcess::startDetached(command.first(), command.mid(1));
            if (ok) {
                return {};
            }

            return t("error.start_command_failed").arg(command.first());
        }));
    }

    void stopService()
    {
        if (m_workerBusy) {
            m_refreshQueued = true;
            return;
        }

        m_workerBusy = true;
        m_tray.setToolTip(t("tray.stopping_service"));

        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            m_workerBusy = false;

            const QString error = watcher->result();
            if (!error.isEmpty()) {
                QMessageBox::critical(nullptr, t("dialog.stop_service_failed_title"), error);
                return;
            }

            m_currentLoadedModels.clear();
            m_currentAvailableModels.clear();
            m_currentError = t("status.service_stopped");
            m_tray.setToolTip(t("tray.service_stopped").arg(m_serverLabel));
            QTimer::singleShot(500, this, &TrayApp::refreshModels);
        });

        const QString executablePath = resolveLlamaSwapExecutable();
        const QString pattern = buildLlamaSwapMatchPattern(executablePath);
        watcher->setFuture(QtConcurrent::run([pattern]() -> QString {
            QProcess process;
            process.start(QStringLiteral("pkill"), {QStringLiteral("-f"), pattern});
            if (!process.waitForFinished(10000)) {
                process.kill();
                return QStringLiteral("pkill timed out");
            }
            const int code = process.exitCode();
            if (code != 0 && code != 1) {
                const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
                return stderrText.isEmpty() ? QStringLiteral("pkill exited with code %1").arg(code) : stderrText;
            }
            return {};
        }));
    }

    void openUi()
    {
        const QUrl url(currentUiUrl());
        if (!QDesktopServices::openUrl(url)) {
            QMessageBox::critical(nullptr, t("dialog.open_ui_failed_title"), t("dialog.open_url_failed").arg(url.toString()));
        }
    }

    void onTrayActivated(QSystemTrayIcon::ActivationReason)
    {
        m_modelsMenu.rebuild(m_currentLoadedModels, m_currentAvailableModels, m_currentError, m_serverLabel);
    }

private:
    [[nodiscard]] QString resolveLlamaSwapExecutable() const
    {
        const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("llama-swap"));
        if (!fromPath.isEmpty()) {
            return fromPath;
        }

        const QString localBin = QDir::home().filePath(QStringLiteral(".local/bin/llama-swap"));
        if (QFileInfo(localBin).isExecutable()) {
            return localBin;
        }

        return {};
    }

    [[nodiscard]] QString buildLlamaSwapMatchPattern(const QString &executablePath) const
    {
        const QString executable = executablePath.isEmpty() ? QStringLiteral("llama-swap") : executablePath;
        const QString escapedExecutable = QRegularExpression::escape(executable);

        const QString configPath = m_appSettings.configPath.trimmed();
        if (configPath.isEmpty()) {
            return escapedExecutable;
        }

        const QString escapedConfig = QRegularExpression::escape(configPath);
        return QStringLiteral("%1.*--config\\s+%2").arg(escapedExecutable, escapedConfig);
    }

    [[nodiscard]] QStringList currentStartCommandParts(const QString &executablePath) const
    {
        QStringList parts = {executablePath, QStringLiteral("--config"), m_appSettings.configPath};
        parts.append(QProcess::splitCommand(m_appSettings.customArgs));
        return parts;
    }

    [[nodiscard]] QString currentUiUrl() const
    {
        return m_api.baseUrl() + QStringLiteral("/ui/models");
    }

    void saveSettings()
    {
        m_settingsStore.setValue(QStringLiteral("host"), m_appSettings.host);
        m_settingsStore.setValue(QStringLiteral("config_path"), m_appSettings.configPath);
        m_settingsStore.setValue(QStringLiteral("custom_args"), m_appSettings.customArgs);
    }

    void applySettings(const AppSettings &newSettings)
    {
        m_appSettings = newSettings;
        m_api.setBaseUrl(normalizeBaseUrl(newSettings.host));
        m_serverLabel = m_api.baseUrl();
        saveSettings();
        m_tray.setToolTip(t("tray.server").arg(m_serverLabel));
        m_modelsMenu.rebuild(m_currentLoadedModels, m_currentAvailableModels, m_currentError, m_serverLabel);
    }

    [[nodiscard]] QIcon buildIcon() const
    {
        const QIcon resourceIcon(QStringLiteral(":/icons/llama_icon.svg"));
        if (!resourceIcon.isNull()) {
            return resourceIcon;
        }

        const QStringList candidates = {
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("llama_icon.svg")),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../assets/llama_icon.svg")),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../assets/llama_icon.svg")),
        };

        for (const QString &path : candidates) {
            const QIcon icon(path);
            if (!icon.isNull()) {
                return icon;
            }
        }

        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.setBrush(QColor(QStringLiteral("#efe6d6")));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(2, 2, 28, 28);

        QPainterPath llama;
        llama.moveTo(10.0, 24.5);
        llama.lineTo(10.8, 15.0);
        llama.lineTo(8.8, 8.2);
        llama.lineTo(10.8, 6.5);
        llama.lineTo(13.5, 11.5);
        llama.lineTo(15.4, 8.0);
        llama.lineTo(18.0, 6.5);
        llama.lineTo(18.8, 13.2);
        llama.quadTo(QPointF(23.8, 14.0), QPointF(24.1, 18.0));
        llama.quadTo(QPointF(24.2, 21.0), QPointF(21.8, 22.0));
        llama.lineTo(20.2, 24.5);
        llama.lineTo(18.2, 24.5);
        llama.lineTo(18.6, 19.9);
        llama.lineTo(14.2, 19.9);
        llama.lineTo(14.0, 24.5);
        llama.closeSubpath();

        QPainterPath headCutout;
        headCutout.addEllipse(QPointF(18.3, 14.8), 1.0, 1.2);

        QPainterPath bodyCutout;
        bodyCutout.addRoundedRect(15.0, 16.2, 6.2, 2.0, 0.8, 0.8);

        painter.setBrush(QColor(QStringLiteral("#161616")));
        painter.drawPath(llama);
        painter.setBrush(QColor(QStringLiteral("#efe6d6")));
        painter.drawPath(headCutout);
        painter.drawPath(bodyCutout);
        painter.end();
        return QIcon(pixmap);
    }

    void applyModelsState(QList<ModelInfo> loadedModels, QList<ModelInfo> availableModels, const QString &error)
    {
        if (!m_pendingLoadingModelId.isEmpty()) {
            auto it = std::find_if(loadedModels.begin(), loadedModels.end(), [this](const ModelInfo &model) {
                return model.modelId == m_pendingLoadingModelId;
            });

            const bool indicatorExpired = m_pendingLoadingStartedAt.isValid()
                && m_pendingLoadingStartedAt.msecsTo(QDateTime::currentDateTimeUtc()) >= std::chrono::duration_cast<std::chrono::milliseconds>(kLoadingIndicatorTimeout).count();

            if (it == loadedModels.end()) {
                loadedModels.push_front(ModelInfo{
                    .modelId = m_pendingLoadingModelId,
                    .title = displayName(m_pendingLoadingModelId),
                    .status = QStringLiteral("loading"),
                });
            } else if (it->status == QStringLiteral("loaded") || it->status == QStringLiteral("ready")
                       || it->status == QStringLiteral("sleeping") || indicatorExpired) {
                m_pendingLoadingModelId.clear();
                m_pendingLoadingStartedAt = {};
            } else {
                it->status = QStringLiteral("loading");
                if (it->title.isEmpty()) {
                    it->title = displayName(m_pendingLoadingModelId);
                }
            }
        }

        m_currentLoadedModels = std::move(loadedModels);
        m_currentAvailableModels = std::move(availableModels);
        m_currentError = error;

        m_modelsMenu.rebuild(m_currentLoadedModels, m_currentAvailableModels, m_currentError, m_serverLabel);

        if (!error.isEmpty()) {
            m_tray.setToolTip(t("tray.connection_error").arg(m_serverLabel));
            return;
        }

        if (!m_currentLoadedModels.isEmpty()) {
            QStringList names;
            const int limit = std::min<int>(3, static_cast<int>(m_currentLoadedModels.size()));
            for (int i = 0; i < limit; ++i) {
                names.push_back(m_currentLoadedModels.at(i).title);
            }
            const QString suffix = m_currentLoadedModels.size() <= 3
                ? QString{}
                : QStringLiteral(" +%1").arg(m_currentLoadedModels.size() - 3);
            m_tray.setToolTip(t("tray.active_models")
                                  .arg(m_currentLoadedModels.size())
                                  .arg(names.join(QStringLiteral(", ")))
                                  .arg(suffix));
        } else {
            m_tray.setToolTip(t("tray.no_active_models"));
        }
    }

    QApplication &m_app;
    LlamaApi m_api;
    QSettings m_settingsStore;
    AppSettings m_appSettings;
    QString m_serverLabel;
    QList<ModelInfo> m_currentLoadedModels;
    QList<ModelInfo> m_currentAvailableModels;
    QString m_currentError;
    QString m_pendingLoadingModelId;
    QDateTime m_pendingLoadingStartedAt;
    bool m_workerBusy = false;
    bool m_refreshQueued = false;
    ModelsMenu m_modelsMenu;
    QSystemTrayIcon m_tray;
    QTimer m_refreshTimer;
};

AppSettings loadInitialSettings(const QString &urlArg)
{
    QSettings settings(QStringLiteral("llama.cpp_statusbar"), QStringLiteral("tray_cpp"));
    const QUrl url(urlArg);
    const QString defaultHost = !url.authority().isEmpty() ? url.authority() : (!url.path().isEmpty() ? url.path() : QStringLiteral("127.0.0.1:8082"));
    return AppSettings{
        .host = settings.value(QStringLiteral("host"), defaultHost).toString(),
        .configPath = settings.value(QStringLiteral("config_path"), defaultConfigPath()).toString(),
        .customArgs = settings.value(QStringLiteral("custom_args"), kDefaultCustomArgs).toString(),
    };
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Linux tray app for llama.cpp model status"));
    parser.addHelpOption();

    QCommandLineOption urlOption(QStringLiteral("url"),
                                 QStringLiteral("Base URL of llama.cpp server."),
                                 QStringLiteral("url"),
                                 QStringLiteral("http://127.0.0.1:8082"));
    QCommandLineOption refreshOption(QStringLiteral("refresh-seconds"),
                                     QStringLiteral("Refresh interval in seconds."),
                                     QStringLiteral("seconds"),
                                     QStringLiteral("5"));
    QCommandLineOption languageOption(QStringLiteral("language"),
                                      QStringLiteral("UI language code. Defaults to English."),
                                      QStringLiteral("language"),
                                      QStringLiteral("en"));
    parser.addOption(urlOption);
    parser.addOption(refreshOption);
    parser.addOption(languageOption);
    parser.process(app);

    Translations::instance().load(parser.value(languageOption));

    bool ok = false;
    const double refreshSeconds = parser.value(refreshOption).toDouble(&ok);
    const int refreshIntervalMs = ok ? std::max(1000, static_cast<int>(refreshSeconds * 1000.0)) : 5000;

    const AppSettings initialSettings = loadInitialSettings(parser.value(urlOption));
    LlamaApi api(normalizeBaseUrl(initialSettings.host));
    TrayApp trayApp(app, std::move(api), refreshIntervalMs, initialSettings);

    return app.exec();
}

#include "main.moc"
