#include "config_repository.h"

#include "document_config.h"
#include "format_helpers.h"
#include "logging_helpers.h"
#include "type_helpers.h"

#include <Poco/Logger.h>
#include <Poco/Util/XMLConfiguration.h>

#include <common/logger_useful.h>

#include <util/string/cast.h>

namespace NYT {
namespace NClickHouse {

namespace {

////////////////////////////////////////////////////////////////////////////////

IConfigPtr LoadXmlConfigFromContent(const std::string& content)
{
    if (!content.empty()) {
        std::stringstream in(content);
        return new Poco::Util::XMLConfiguration(in);
    }
    return nullptr;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

// Effective polling through metadata cache

class TPoller
    : public IConfigPoller
{
private:
    NInterop::IStoragePtr Storage;
    NInterop::IAuthorizationTokenPtr Token;
    std::string ConfigPath;

public:
    TPoller(NInterop::IStoragePtr storage,
                  NInterop::IAuthorizationTokenPtr token,
                  std::string configPath)
        : Storage(std::move(storage))
        , Token(std::move(token))
        , ConfigPath(std::move(configPath))
    {}

    TMaybe<NInterop::TRevision> GetRevision() const override
    {
        return Storage->GetObjectRevision(
            *Token,
            ToString(ConfigPath),
            /*throughCache=*/ true);
    }
};

////////////////////////////////////////////////////////////////////////////////

// Directory with documents/files

class TConfigRepository
    : public IConfigRepository
{
private:
    NInterop::IStoragePtr Storage;
    NInterop::IAuthorizationTokenPtr Token;
    std::string ConfigsPath;

    Poco::Logger* Logger;

public:
    TConfigRepository(NInterop::IStoragePtr storage,
                      NInterop::IAuthorizationTokenPtr token,
                      std::string configsPath);

    std::string GetAddress() const override;

    bool Exists(const std::string& name) const override;
    std::vector<std::string> List() const override;
    NInterop::TObjectAttributes GetAttributes(const std::string& name) const override;

    IConfigPtr Load(const std::string& name) const override;

    IConfigPollerPtr CreatePoller(const std::string& name) const override;

private:
    bool LooksLikeConfig(const NInterop::TObjectAttributes& attributes) const;

    IConfigPtr LoadFromFile(const std::string& path) const;
    IConfigPtr LoadFromDocument(const std::string& path) const;

    std::string GetConfigPath(const std::string& name) const;
};

////////////////////////////////////////////////////////////////////////////////

TConfigRepository::TConfigRepository(NInterop::IStoragePtr storage,
                                     NInterop::IAuthorizationTokenPtr token,
                                     std::string configsPath)
    : Storage(std::move(storage))
    , Token(std::move(token))
    , ConfigsPath(std::move(configsPath))
    , Logger(&Poco::Logger::get("ConfigRepository"))
{
    LOG_DEBUG(Logger, "Open configuration repository: " << Quoted(ConfigsPath));
}

std::string TConfigRepository::GetAddress() const
{
    return ConfigsPath;
}

bool TConfigRepository::Exists(const std::string& name) const
{
    return Storage->Exists(*Token, ToString(GetConfigPath(name)));
}

std::vector<std::string> TConfigRepository::List() const
{
    auto objects = Storage->ListObjects(*Token, ToString(ConfigsPath));

    std::vector<std::string> names;
    names.reserve(objects.size());
    for (auto object : objects) {
        if (LooksLikeConfig(object.Attributes)) {
            names.push_back(ToStdString(object.Name));
        }
    }
    return names;
}

NInterop::TObjectAttributes TConfigRepository::GetAttributes(const std::string& name) const
{
    return Storage->GetObjectAttributes(*Token, ToString(GetConfigPath(name)));
}

bool TConfigRepository::LooksLikeConfig(const NInterop::TObjectAttributes& attributes) const
{
    return attributes.Type == NInterop::EObjectType::Document ||
           attributes.Type == NInterop::EObjectType::File;
}

IConfigPtr TConfigRepository::LoadFromFile(const std::string& path) const
{
    LOG_INFO(Logger, "Loading configuration from file " << Quoted(path));

    std::string content;
    try {
        content = ToStdString(Storage->ReadFile(*Token, ToString(path)));
    } catch (...) {
        LOG_WARNING(Logger, "Cannot read configuration file " << Quoted(path) << " from storage: " << CurrentExceptionText());
        return nullptr;
    }

    try {
        return LoadXmlConfigFromContent(content);
    } catch (...) {
        LOG_WARNING(Logger, "Cannot parse content of configuration file " << Quoted(path) << ": " << CurrentExceptionText());
        return nullptr;
    }
}

IConfigPtr TConfigRepository::LoadFromDocument(const std::string& path) const
{
    LOG_INFO(Logger, "Loading configuration from document " << Quoted(path));

    NInterop::IDocumentPtr document;
    try {
        document = Storage->ReadDocument(*Token, ToString(path));
    } catch (...) {
        LOG_WARNING(Logger, "Cannot read configuration document " << Quoted(path) << " from storage: " << CurrentExceptionText());
        return nullptr;
    }
    return CreateDocumentConfig(std::move(document));
}

IConfigPtr TConfigRepository::Load(const std::string& name) const
{
    const auto path = GetConfigPath(name);

    LOG_DEBUG(Logger, "Loading configuration " << Quoted(name) << " from " << Quoted(path));

    NInterop::TObjectAttributes attributes;
    try {
        attributes = Storage->GetObjectAttributes(*Token, ToString(path));
    } catch (...) {
        LOG_WARNING(Logger, "Cannot get attributes of object " << Quoted(path) << " in storage: " << CurrentExceptionText());
        return nullptr;
    }

    switch (attributes.Type) {
        case NInterop::EObjectType::File:
            return LoadFromFile(path);
        case NInterop::EObjectType::Document:
            return LoadFromDocument(path);
        default:
            LOG_WARNING(Logger,
                "Unexpected configuration object type: " << ToStdString(::ToString(attributes.Type)));
            return nullptr;
    }

    Y_UNREACHABLE();
}

IConfigPollerPtr TConfigRepository::CreatePoller(const std::string& name) const
{
    return std::make_unique<TPoller>(Storage, Token, GetConfigPath(name));
}

std::string TConfigRepository::GetConfigPath(const std::string& name) const
{
    auto path = Storage->PathService()->Build(ToString(ConfigsPath), {ToString(name)});
    return ToStdString(path);
}

////////////////////////////////////////////////////////////////////////////////

IConfigRepositoryPtr CreateConfigRepository(
    NInterop::IStoragePtr storage,
    NInterop::IAuthorizationTokenPtr token,
    const std::string& path)
{
    return std::make_shared<TConfigRepository>(
        std::move(storage),
        std::move(token),
        path);
}

} // namespace NClickHouse
} // namespace NYT
