/*
 * mod-llm-chatter - Tofu Worldview addition: LLM chatter in the realm-wide
 * custom World channel. See LLMChatterTofuWorld.h for the flow.
 */

#include "LLMChatterTofuWorld.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "LLMChatterConfig.h"
#include "LLMChatterShared.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
struct TofuWorldSettings
{
    bool enable = false;
    std::string channelName = "World";
    uint32 triggerIntervalSeconds = 60;
    uint32 triggerChance = 40;
    uint32 conversationChance = 35;
    uint32 localBotChance = 15;
    uint32 replyChance = 50;
    uint32 questionReplyChance = 100;
    uint32 replyCooldownSeconds = 15;
    uint32 botCooldownSeconds = 300;
    uint32 maxCandidates = 3;
    uint32 historyLimit = 12;
};

TofuWorldSettings _settings;
std::mutex _stateMutex;
std::unordered_map<uint32, time_t> _botLastSpoke;
time_t _lastStatementReplyAt = 0;
uint32 _ambientTimerMs = 0;

uint32 ClampPercent(uint32 value)
{
    return std::min<uint32>(value, 100);
}

void LoadTofuWorldSettings()
{
    TofuWorldSettings s;
    s.enable = sConfigMgr->GetOption<bool>(
        "LLMChatter.Tofu.World.Enable", false);
    s.channelName = sConfigMgr->GetOption<std::string>(
        "LLMChatter.Tofu.World.ChannelName", "World");
    s.triggerIntervalSeconds = std::max<uint32>(10,
        sConfigMgr->GetOption<uint32>(
            "LLMChatter.Tofu.World.TriggerIntervalSeconds", 60));
    s.triggerChance = ClampPercent(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.TriggerChance", 40));
    s.conversationChance = ClampPercent(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.ConversationChance", 35));
    s.localBotChance = ClampPercent(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.LocalBotChance", 15));
    s.replyChance = ClampPercent(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.ReplyChance", 50));
    s.questionReplyChance = ClampPercent(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.QuestionReplyChance", 100));
    s.replyCooldownSeconds = sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.ReplyCooldownSeconds", 15);
    s.botCooldownSeconds = sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.BotCooldownSeconds", 300);
    s.maxCandidates = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.MaxCandidates", 3), 1, 5);
    s.historyLimit = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(
        "LLMChatter.Tofu.World.HistoryLimit", 12), 1, 50);

    if (s.channelName.empty())
        s.channelName = "World";

    std::lock_guard<std::mutex> guard(_stateMutex);
    _settings = s;
}

TofuWorldSettings CurrentSettings()
{
    std::lock_guard<std::mutex> guard(_stateMutex);
    return _settings;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return value;
}

bool IsTofuWorldChannel(Channel const* channel,
    TofuWorldSettings const& s)
{
    // The World channel is a custom channel: built-in zone
    // channels (General, Trade, ...) carry a non-zero id.
    return channel && !channel->IsConstant()
        && ToLower(channel->GetName()) == ToLower(s.channelName);
}

Channel* FindWorldChannel(TeamId team, TofuWorldSettings const& s)
{
    ChannelMgr* mgr = ChannelMgr::forTeam(team);
    if (!mgr)
        return nullptr;

    for (auto const& [key, channel] : mgr->GetChannels())
    {
        if (IsTofuWorldChannel(channel, s))
            return channel;
    }

    return nullptr;
}

struct Audience
{
    uint32 count = 0;
    std::unordered_set<uint32> zones;
};

// Real players only: bot sessions are headless and are skipped.
Audience CollectAudience()
{
    Audience audience;
    for (auto const& pair : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = pair.second
            ? pair.second->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || IsPlayerBot(player))
            continue;

        ++audience.count;
        audience.zones.insert(player->GetZoneId());
    }
    return audience;
}

// Every bot that may speak (random bots, plus alt bots when
// LLMChatter.AltBots.Enable is on), each once.
std::vector<Player*> CollectAllBots()
{
    std::vector<Player*> bots;
    std::set<ObjectGuid> seen;
    auto consider = [&](Player* bot)
    {
        if (bot && seen.insert(bot->GetGUID()).second)
            bots.push_back(bot);
    };

    for (auto const& pair : sWorldSessionMgr->GetAllSessions())
    {
        Player* player = pair.second
            ? pair.second->GetPlayer() : nullptr;
        if (player && IsPlayerBot(player))
            consider(player);
    }
    for (Player* bot : GetChatterBots())
        consider(bot);

    return bots;
}

// A bot may speak in World when it is a chatter speaker, is not
// grouped with a real player (those bots have party chat), is not
// in a battleground or arena, and is a member of the World channel.
bool IsEligibleSpeaker(Player* bot, TofuWorldSettings const& s)
{
    if (!bot || !bot->IsInWorld())
        return false;
    if (!IsChatterSpeakerAllowed(bot))
        return false;
    if (IsGroupedWithRealPlayer(bot))
        return false;

    Map* map = bot->GetMap();
    if (map && map->IsBattlegroundOrArena())
        return false;

    Channel* channel = FindWorldChannel(bot->GetTeamId(), s);
    return channel && IsPlayerInChannel(bot, channel);
}

bool IsOnBotCooldown(uint32 guid, time_t now,
    TofuWorldSettings const& s)
{
    std::lock_guard<std::mutex> guard(_stateMutex);
    auto it = _botLastSpoke.find(guid);
    return it != _botLastSpoke.end()
        && (now - it->second) < (time_t)s.botCooldownSeconds;
}

void MarkBotSpoke(uint32 guid, time_t now)
{
    std::lock_guard<std::mutex> guard(_stateMutex);
    _botLastSpoke[guid] = now;

    // Keep the map small: forget bots idle for a day.
    if (_botLastSpoke.size() > 2000)
    {
        for (auto it = _botLastSpoke.begin();
             it != _botLastSpoke.end();)
        {
            if (now - it->second > 86400)
                it = _botLastSpoke.erase(it);
            else
                ++it;
        }
    }
}

// Splits eligible bots into those in a zone with a real player
// (local: they also have General and /say) and the rest (remote).
void CollectSpeakers(Audience const& audience,
    TofuWorldSettings const& s, bool respectCooldown, time_t now,
    std::vector<Player*>& local, std::vector<Player*>& remote)
{
    for (Player* bot : CollectAllBots())
    {
        if (!IsEligibleSpeaker(bot, s))
            continue;
        if (respectCooldown && IsOnBotCooldown(
                bot->GetGUID().GetCounter(), now, s))
            continue;

        if (audience.zones.count(bot->GetZoneId()))
            local.push_back(bot);
        else
            remote.push_back(bot);
    }

    std::mt19937 rng{std::random_device{}()};
    std::shuffle(local.begin(), local.end(), rng);
    std::shuffle(remote.begin(), remote.end(), rng);
}

// Mostly a remote bot; a local one with LocalBotChance, or when
// no remote bot is available.
Player* TakeSpeaker(std::vector<Player*>& local,
    std::vector<Player*>& remote, TofuWorldSettings const& s)
{
    bool useLocal = !local.empty()
        && (remote.empty() || urand(1, 100) <= s.localBotChance);
    std::vector<Player*>& pool = useLocal ? local : remote;
    if (pool.empty())
        return nullptr;

    Player* bot = pool.back();
    pool.pop_back();
    return bot;
}

void RecordWorldHistory(Player* speaker, bool isBot,
    std::string const& text, TofuWorldSettings const& s)
{
    CharacterDatabase.Execute(
        "INSERT INTO llm_tofu_world_history "
        "(speaker_guid, speaker_name, is_bot, team, zone_id, message) "
        "VALUES ({}, '{}', {}, {}, {}, '{}')",
        speaker->GetGUID().GetCounter(),
        EscapeString(speaker->GetName()),
        isBot ? 1 : 0,
        speaker->GetTeamId() == TEAM_HORDE ? 1 : 0,
        speaker->GetZoneId(),
        EscapeString(text));

    // Keep a few windows of history for the bridge to read.
    CharacterDatabase.Execute(
        "DELETE FROM llm_tofu_world_history WHERE id NOT IN "
        "(SELECT id FROM (SELECT id FROM llm_tofu_world_history "
        "ORDER BY id DESC LIMIT {}) AS keep_rows)",
        s.historyLimit * 4);
}

std::string TeamName(Player* player)
{
    return player->GetTeamId() == TEAM_HORDE ? "Horde" : "Alliance";
}

// The bot whose name the player wrote as a whole word, if any.
Player* FindMentionedBot(std::string const& message,
    std::vector<Player*> const& bots)
{
    std::string lowered = ToLower(message);
    for (Player* bot : bots)
    {
        std::string name = ToLower(bot->GetName());
        if (name.empty())
            continue;

        size_t pos = lowered.find(name);
        while (pos != std::string::npos)
        {
            bool startOk = pos == 0
                || !std::isalpha(
                    static_cast<unsigned char>(lowered[pos - 1]));
            size_t end = pos + name.size();
            bool endOk = end >= lowered.size()
                || !std::isalpha(
                    static_cast<unsigned char>(lowered[end]));
            if (startOk && endOk)
                return bot;
            pos = lowered.find(name, pos + 1);
        }
    }
    return nullptr;
}

std::string Trim(std::string const& text)
{
    size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos)
        return "";
    size_t last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
}

void TryQueueAmbient()
{
    TofuWorldSettings s = CurrentSettings();
    Audience audience = CollectAudience();
    if (audience.count == 0)
        return;

    if (urand(1, 100) > s.triggerChance)
        return;

    time_t now = time(nullptr);
    std::vector<Player*> local;
    std::vector<Player*> remote;
    CollectSpeakers(audience, s, true, now, local, remote);

    Player* first = TakeSpeaker(local, remote, s);
    if (!first)
        return;

    Player* second = nullptr;
    if (urand(1, 100) <= s.conversationChance)
        second = TakeSpeaker(local, remote, s);

    MarkBotSpoke(first->GetGUID().GetCounter(), now);
    if (second)
        MarkBotSpoke(second->GetGUID().GetCounter(), now);

    std::string extraData = "{"
        "\"channel_name\":\""
            + ChatterJsonEscape(s.channelName) + "\","
        "\"speaker_zone_id\":"
            + std::to_string(first->GetZoneId()) + ","
        "\"speaker_area_id\":"
            + std::to_string(first->GetAreaId()) + ","
        "\"partner_zone_id\":"
            + std::to_string(second ? second->GetZoneId() : 0) + ","
        "\"conversation\":" + std::string(second ? "1" : "0") + ","
        "\"history_limit\":" + std::to_string(s.historyLimit) + "}";

    std::string const eventType = "tofu_world_ambient";
    QueueChatterEvent(
        eventType, "global", 0, 0,
        GetChatterEventPriority(eventType), "",
        first->GetGUID().GetCounter(), first->GetName(),
        second ? second->GetGUID().GetCounter() : 0,
        second ? second->GetName() : "",
        0, EscapeString(extraData),
        0, 180, true);
}

class LLMChatterTofuWorldScript : public WorldScript
{
public:
    LLMChatterTofuWorldScript()
        : WorldScript("LLMChatterTofuWorldScript",
              {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE})
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadTofuWorldSettings();
    }

    void OnUpdate(uint32 diff) override
    {
        if (!TofuWorld::IsActive())
        {
            _ambientTimerMs = 0;
            return;
        }

        _ambientTimerMs += diff;
        uint32 intervalMs =
            CurrentSettings().triggerIntervalSeconds * 1000;
        if (_ambientTimerMs < intervalMs)
            return;

        _ambientTimerMs = 0;
        TryQueueAmbient();
    }
};

class LLMChatterTofuWorldPlayerScript : public PlayerScript
{
public:
    LLMChatterTofuWorldPlayerScript()
        : PlayerScript("LLMChatterTofuWorldPlayerScript",
              {PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT})
    {
    }

    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/,
        uint32 language, std::string& msg, Channel* channel) override
    {
        if (!TofuWorld::IsActive())
            return true;

        TofuWorldSettings s = CurrentSettings();
        if (!IsTofuWorldChannel(channel, s))
            return true;
        if (!player || !player->IsInWorld() || IsPlayerBot(player))
            return true;

        // Hidden addon traffic is not player speech.
        if (language == LANG_ADDON)
        {
            LogIgnoredAddonChat(player, CHAT_MSG_CHANNEL, msg,
                "tofu_world");
            return true;
        }

        std::string text = Trim(msg);
        if (text.empty()
            || sLLMChatterConfig->IsPlayerChatPrefixIgnored(text))
            return true;

        text = NormalizeChatTextForDb(text,
            sLLMChatterConfig->_maxMessageLength);
        if (text.empty())
            return true;

        RecordWorldHistory(player, false, text, s);

        time_t now = time(nullptr);
        Audience audience = CollectAudience();
        std::vector<Player*> local;
        std::vector<Player*> remote;
        CollectSpeakers(audience, s, false, now, local, remote);
        if (local.empty() && remote.empty())
            return true;

        std::vector<Player*> all(remote);
        all.insert(all.end(), local.begin(), local.end());
        Player* mentioned = FindMentionedBot(text, all);
        bool isQuestion = text.back() == '?';

        // A bot addressed by name always answers. A question is
        // answered with QuestionReplyChance; any other line with
        // ReplyChance, at most once per ReplyCooldownSeconds.
        if (!mentioned)
        {
            if (isQuestion)
            {
                if (urand(1, 100) > s.questionReplyChance)
                    return true;
            }
            else
            {
                std::lock_guard<std::mutex> guard(_stateMutex);
                if ((now - _lastStatementReplyAt)
                        < (time_t)s.replyCooldownSeconds)
                    return true;
                if (urand(1, 100) > s.replyChance)
                    return true;
                _lastStatementReplyAt = now;
            }
        }

        std::vector<Player*> candidates;
        if (mentioned)
            candidates.push_back(mentioned);
        while (candidates.size() < s.maxCandidates)
        {
            Player* next = TakeSpeaker(local, remote, s);
            if (!next)
                break;
            if (next != mentioned)
                candidates.push_back(next);
        }

        std::string botGuids = "[";
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (i > 0)
                botGuids += ",";
            botGuids += std::to_string(
                candidates[i]->GetGUID().GetCounter());
        }
        botGuids += "]";

        std::string extraData = "{"
            "\"channel_name\":\""
                + ChatterJsonEscape(s.channelName) + "\","
            "\"player_name\":\""
                + ChatterJsonEscape(player->GetName()) + "\","
            "\"player_message\":\""
                + ChatterJsonEscape(text) + "\","
            "\"player_race\":"
                + std::to_string(player->getRace()) + ","
            "\"player_class\":"
                + std::to_string(player->getClass()) + ","
            "\"player_gender\":"
                + std::to_string(player->getGender()) + ","
            "\"player_level\":"
                + std::to_string(player->GetLevel()) + ","
            "\"player_team\":\"" + TeamName(player) + "\","
            "\"player_zone_id\":"
                + std::to_string(player->GetZoneId()) + ","
            "\"mentioned\":" + std::string(mentioned ? "1" : "0") + ","
            "\"history_limit\":" + std::to_string(s.historyLimit) + ","
            "\"bot_guids\":" + botGuids + "}";

        // Same priority as a General reply: a player is waiting.
        std::string const eventType = "tofu_world_player_msg";
        QueueChatterEvent(
            eventType, "global", 0, 0,
            GetChatterEventPriority("player_general_msg"), "",
            player->GetGUID().GetCounter(), player->GetName(),
            mentioned ? mentioned->GetGUID().GetCounter() : 0,
            mentioned ? mentioned->GetName() : "",
            0, EscapeString(extraData),
            0, 90, true);

        return true;
    }
};
} // namespace

namespace TofuWorld
{
bool IsActive()
{
    if (!sLLMChatterConfig || !sLLMChatterConfig->IsEnabled())
        return false;
    std::lock_guard<std::mutex> guard(_stateMutex);
    return _settings.enable;
}

bool Deliver(Player* bot, std::string const& text,
    char const** dropReason)
{
    auto drop = [dropReason](char const* reason)
    {
        if (dropReason)
            *dropReason = reason;
        return false;
    };

    if (!IsActive())
        return drop("tofu_world_disabled");
    if (!bot || !bot->IsInWorld())
        return drop("speaker_unavailable");
    if (text.empty())
        return drop("empty_message");

    TofuWorldSettings s = CurrentSettings();
    Channel* channel = FindWorldChannel(bot->GetTeamId(), s);
    if (!channel)
        return drop("world_channel_missing");
    if (!IsPlayerInChannel(bot, channel))
        return drop("world_not_member");

    channel->Say(bot->GetGUID(), text, LANG_UNIVERSAL);
    RecordWorldHistory(bot, true, text, s);
    MarkBotSpoke(bot->GetGUID().GetCounter(), time(nullptr));
    return true;
}
} // namespace TofuWorld

void AddLLMChatterTofuWorldScripts()
{
    new LLMChatterTofuWorldScript();
    new LLMChatterTofuWorldPlayerScript();
}
