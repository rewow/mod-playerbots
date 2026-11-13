/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifdef USE_CUSTOM_CHANGES

#include "BattlegroundBotFill.h"

#include "AccountMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "World.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
    char const* const ACCOUNT_PREFIX            = "BGBOT";          // dedicated accounts, not matched by the random bot prefix
    char const* const CONFIG_ENABLED            = "Playerbots.BattlegroundBotFill";
    uint32 const FIRST_ACCOUNT_INDEX            = 1;
    uint32 const CHARS_PER_ACCOUNT              = 9;                // below the realm's 10 characters per account
    uint32 const PASSWORD_LENGTH                = 10;
    uint32 const MAX_CREATE_TRIES               = 10;               // CreateRandomBot picks the faction itself

    uint32 const UPDATE_INTERVAL                = 500;              // ms
    uint32 const MAX_LOGINS_PER_UPDATE          = 20;
    uint32 const MAX_PREPARES_PER_UPDATE        = 5;                // level / gear setup is the expensive step
    uint32 const MAX_CREATES_PER_UPDATE         = 5;

    time_t const QUEUE_JOIN_GRACE               = 10;               // s, a request whose player is not in its queue yet
    time_t const NEW_CHAR_LOGIN_DELAY           = 3;                // s, lets the character save reach the database first
    time_t const LOGIN_TIMEOUT                  = 60;               // s
    time_t const LEAVE_TIMEOUT                  = 30;               // s, for the port out of a match
    time_t const CREATE_RETRY_DELAY             = 30;               // s, after a failed character / account creation
    time_t const INVITE_ACCEPT_DELAY            = 10;               // s, the bot's own "bg status" action normally accepts first

    uint32 const SPELL_DESERTER                 = 26013;
    uint16 const BATTLEFIELD_PORT_UNK           = 0x1F90;
    uint8 const BATTLEFIELD_PORT_ENTER          = 1;
}

namespace
{
    // a player in the world or on a loading screen (ObjectAccessor::FindPlayer() misses a teleporting one: the queue
    // owner on its way into the match counted as gone, and its bots were released inside)
    Player* FindConnected(uint32 guid)
    {
        return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
    }
}

BattlegroundBotFill::BattlegroundBotFill() :
    m_nextAccountIndex(FIRST_ACCOUNT_INDEX), m_updateTimer(0), m_createBlockedUntil(0), m_initialized(false),
    m_enabled(false)
{
}

bool BattlegroundBotFill::IsEnabled() const
{
    return sPlayerbotAIConfig.Enabled && sPlayerbotAIConfig.RandomBotJoinBG && sConfigMgr->GetOption<bool>(CONFIG_ENABLED, true);
}

void BattlegroundBotFill::OnQueueJoin(Player* player)
{
    if (!player || !IsRealPlayer(player) || !IsEnabled())
        return;

    // the join handlers call this right after the queue join, every queue the player is in gets a request
    for (uint8 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
    {
        BattlegroundQueueTypeId queueTypeId = player->GetBattlegroundQueueTypeId(slot);
        if (queueTypeId == BATTLEGROUND_QUEUE_NONE || queueTypeId == BATTLEGROUND_QUEUE_RB)
            continue;

        auto key = std::make_pair(player->GetGUID().GetCounter(), queueTypeId);
        if (m_requests.count(key))
            continue;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        GroupQueueInfo ginfo;
        // rated arenas need arena teams, only battlegrounds and skirmishes are filled
        if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo) || ginfo.IsRated)
            continue;

        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BattlegroundMgr::BGTemplateId(queueTypeId));
        if (!bgTemplate)
            continue;

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
        if (!bracketEntry)
            continue;

        FillRequest& request = m_requests[key];
        request.key.queueTypeId = queueTypeId;
        request.key.bracketId = bracketEntry->GetBracketId();
        request.level = player->GetLevel();
        request.joinTime = time(nullptr);

        LOG_INFO("playerbots", "BattlegroundBotFill: {} (level {}) queued for {}, filling the queue with bots",
                 player->GetName(), request.level, QueueName(queueTypeId));
    }
}

void BattlegroundBotFill::Update(uint32 diff)
{
    m_updateTimer += diff;
    if (m_updateTimer < UPDATE_INTERVAL)
        return;
    m_updateTimer = 0;

    // read once a tick, a config reload switches it
    m_enabled = IsEnabled();
    if (!m_enabled)
    {
        m_requests.clear();
        if (m_bots.empty())
            return;
    }

    if (!m_initialized)
        Initialize();

    time_t now = time(nullptr);

    KeyLevelMap waitingKeys;
    KeySet activeKeys;
    UpdateRequests(now, waitingKeys, activeKeys);

    for (auto const& itr : waitingKeys)
        FillQueue(itr.first, itr.second, now);

    UpdateCreateJobs(now, activeKeys);
    UpdateBots(now, activeKeys);
    LogoutStrayPoolBots();
}

void BattlegroundBotFill::Initialize()
{
    m_initialized = true;

    QueryResult accounts = LoginDatabase.Query("SELECT id, username FROM account WHERE username LIKE '{}%'", ACCOUNT_PREFIX);
    if (accounts)
    {
        size_t const prefixLength = strlen(ACCOUNT_PREFIX);
        do
        {
            Field* fields = accounts->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            std::string name = fields[1].Get<std::string>();

            uint32 index = uint32(atoi(name.substr(prefixLength).c_str()));
            m_nextAccountIndex = std::max(m_nextAccountIndex, index + 1);
            m_accounts.emplace(accountId, 0);
        } while (accounts->NextRow());
    }

    if (!m_accounts.empty())
    {
        std::string accountList;
        for (auto const& itr : m_accounts)
        {
            if (!accountList.empty())
                accountList += ",";
            accountList += std::to_string(itr.first);
        }

        QueryResult chars = CharacterDatabase.Query("SELECT guid, account, race, level FROM characters WHERE account IN ({})", accountList);
        if (chars)
        {
            do
            {
                Field* fields = chars->Fetch();
                uint32 guid = fields[0].Get<uint32>();

                PoolChar& poolChar = m_pool[guid];
                poolChar.account = fields[1].Get<uint32>();
                poolChar.team = Player::TeamIdForRace(fields[2].Get<uint8>());
                poolChar.level = fields[3].Get<uint8>();
                poolChar.busy = false;

                ++m_accounts[poolChar.account];
            } while (chars->NextRow());
        }
    }

    LOG_INFO("playerbots", "BattlegroundBotFill: {} accounts with {} characters in the battleground bot pool",
             m_accounts.size(), m_pool.size());
}

void BattlegroundBotFill::UpdateRequests(time_t now, KeyLevelMap& waitingKeys, KeySet& activeKeys)
{
    for (auto itr = m_requests.begin(); itr != m_requests.end();)
    {
        Player* owner = FindConnected(itr->first.first);
        BattlegroundQueueTypeId queueTypeId = itr->first.second;
        FillRequest const& request = itr->second;

        bool inQueue = owner && owner->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
        bool joining = owner && !inQueue && now - request.joinTime < QUEUE_JOIN_GRACE;
        if (!inQueue && !joining)
        {
            // left the queue or its match: the bots follow once their own match is over
            itr = m_requests.erase(itr);
            continue;
        }

        activeKeys.insert(request.key);
        if (joining || IsWaitingInQueue(owner, queueTypeId))
            waitingKeys.emplace(request.key, request.level);

        ++itr;
    }
}

void BattlegroundBotFill::FillQueue(FillKey const& key, uint32 level, time_t now)
{
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(key.queueTypeId);
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return;

    uint8 arenaType = BattlegroundMgr::BGArenaType(key.queueTypeId);
    bool isArena = arenaType != 0;
    uint32 teamSize = isArena ? uint32(arenaType) : bgTemplate->GetMaxPlayersPerTeam();

    uint32 count[PVP_TEAMS_COUNT] = { 0, 0 };
    CountQueued(key, isArena, count);

    uint32 added[PVP_TEAMS_COUNT] = { 0, 0 };
    size_t jobsBefore = m_createJobs.size();
    for (uint8 i = 0; i < PVP_TEAMS_COUNT; ++i)
    {
        TeamId team = TeamId(i);
        for (; count[i] + added[i] < teamSize; ++added[i])
            AddBot(key, team, level, now);
    }

    if (added[TEAM_ALLIANCE] || added[TEAM_HORDE])
        LOG_INFO("playerbots", "BattlegroundBotFill: {} level {} - queued A:{} H:{} of {}, adding A:{} H:{} bots ({} new characters)",
                 QueueName(key.queueTypeId), level, count[TEAM_ALLIANCE], count[TEAM_HORDE], teamSize,
                 added[TEAM_ALLIANCE], added[TEAM_HORDE], m_createJobs.size() - jobsBefore);
}

void BattlegroundBotFill::CountQueued(FillKey const& key, bool isArena, uint32 (&count)[PVP_TEAMS_COUNT]) const
{
    BattlegroundQueueTypeId queueTypeId = key.queueTypeId;

    // own bots still on their way into the queue (and in arenas the waiting ones too)
    for (auto const& itr : m_bots)
    {
        FillBot const& fillBot = itr.second;
        if (!(fillBot.key == key))
            continue;

        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(itr.first);
        bool inQueue = fillBot.state == BotState::QUEUED && bot && bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
        if (!inQueue)
        {
            if (fillBot.state != BotState::QUEUED || (bot && !bot->InBattleground()))
                ++count[fillBot.team];
        }
        else if (isArena && IsWaitingInQueue(bot, queueTypeId))
            ++count[fillBot.team];
    }

    for (CreateJob const& job : m_createJobs)
        if (job.key == key)
            ++count[job.team];

    if (!isArena)
    {
        // everyone waiting in this battleground bracket, own bots and the random bots included
        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& itr : ObjectAccessor::GetPlayers())
            {
                Player* player = itr.second;
                if (!player || !player->IsInWorld() || !IsWaitingInQueue(player, queueTypeId))
                    continue;

                PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
                if (!bracketEntry || bracketEntry->GetBracketId() != key.bracketId)
                    continue;

                ++count[player->GetTeamId()];
            }
        }

        // requesting players whose queue join has not reached them yet
        for (auto const& itr : m_requests)
        {
            if (!(itr.second.key == key))
                continue;

            Player* owner = FindConnected(itr.first.first);
            if (owner && !owner->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
                ++count[owner->GetTeamId()];
        }
        return;
    }

    // skirmish queues hold rated teams as well, so only the requesting players and their groups count
    std::set<ObjectGuid> counted;
    for (auto const& itr : m_requests)
    {
        if (!(itr.second.key == key))
            continue;

        Player* owner = FindConnected(itr.first.first);
        if (!owner)
            continue;

        std::vector<Player*> members = { owner };
        if (Group* group = owner->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    members.push_back(member);

        for (Player* member : members)
        {
            bool joining = member == owner && !owner->InBattlegroundQueueForBattlegroundQueueType(queueTypeId);
            if ((joining || IsWaitingInQueue(member, queueTypeId)) && counted.insert(member->GetGUID()).second)
                ++count[member->GetTeamId()];
        }
    }
}

void BattlegroundBotFill::AddBot(FillKey const& key, TeamId team, uint32 level, time_t now)
{
    uint32 guid = PickPoolChar(team, level);
    if (!guid)
    {
        CreateJob job;
        job.key = key;
        job.team = team;
        job.level = level;
        m_createJobs.push_back(job);
        return;
    }

    m_pool[guid].busy = true;

    FillBot& fillBot = m_bots[guid];
    fillBot.key = key;
    fillBot.team = team;
    fillBot.level = level;
    fillBot.state = BotState::WAIT_LOGIN;
    fillBot.stateTime = now;
}

uint32 BattlegroundBotFill::PickPoolChar(TeamId team, uint32 level)
{
    // characters already at the wanted level skip the level / gear setup
    std::vector<uint32> sameLevel;
    std::vector<uint32> otherLevel;
    for (auto const& itr : m_pool)
    {
        PoolChar const& poolChar = itr.second;
        if (poolChar.busy || poolChar.team != team)
            continue;

        if (FindConnected(itr.first))
            continue;

        (poolChar.level == level ? sameLevel : otherLevel).push_back(itr.first);
    }

    std::vector<uint32> const& candidates = sameLevel.empty() ? otherLevel : sameLevel;
    if (candidates.empty())
        return 0;

    return candidates[urand(0, candidates.size() - 1)];
}

void BattlegroundBotFill::UpdateCreateJobs(time_t now, KeySet const& activeKeys)
{
    for (uint32 created = 0; created < MAX_CREATES_PER_UPDATE && !m_createJobs.empty() && now >= m_createBlockedUntil;)
    {
        CreateJob job = m_createJobs.front();
        if (!activeKeys.count(job.key))
        {
            m_createJobs.pop_front();
            continue;
        }

        uint32 accountId = GetAccountWithFreeSlot();
        if (!accountId)
            break;

        m_createJobs.pop_front();

        uint32 guid = CreateBotCharacter(accountId, job.team);
        if (!guid)
        {
            LOG_ERROR("playerbots", "BattlegroundBotFill: could not create a {} bot character on account {}",
                      job.team == TEAM_ALLIANCE ? "alliance" : "horde", accountId);
            m_createBlockedUntil = now + CREATE_RETRY_DELAY;
            break;
        }

        FillBot& fillBot = m_bots[guid];
        fillBot.key = job.key;
        fillBot.team = job.team;
        fillBot.level = job.level;
        fillBot.state = BotState::WAIT_LOGIN;
        fillBot.stateTime = now + NEW_CHAR_LOGIN_DELAY;
        ++created;
    }
}

uint32 BattlegroundBotFill::GetAccountWithFreeSlot()
{
    for (auto const& itr : m_accounts)
        if (itr.second < CHARS_PER_ACCOUNT)
            return itr.first;

    if (m_pendingAccount.empty())
    {
        m_pendingAccount = ACCOUNT_PREFIX + std::to_string(m_nextAccountIndex++);

        std::string password;
        for (uint32 i = 0; i < PASSWORD_LENGTH; ++i)
            password += char(urand('a', 'z'));

        AccountOpResult result = sAccountMgr->CreateAccount(m_pendingAccount, password);
        if (result != AOR_OK && result != AOR_NAME_ALREADY_EXIST)
        {
            LOG_ERROR("playerbots", "BattlegroundBotFill: could not create account {} (error {})", m_pendingAccount, uint32(result));
            m_pendingAccount.clear();
            m_createBlockedUntil = time(nullptr) + CREATE_RETRY_DELAY;
        }
        return 0;
    }

    // the account insert is asynchronous, wait until it can be found
    uint32 accountId = AccountMgr::GetId(m_pendingAccount);
    if (!accountId)
        return 0;

    LOG_INFO("playerbots", "BattlegroundBotFill: created account {} for battleground bots", m_pendingAccount);
    m_pendingAccount.clear();
    m_accounts.emplace(accountId, 0);
    return accountId;
}

uint32 BattlegroundBotFill::CreateBotCharacter(uint32 accountId, TeamId team)
{
    WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                                             time_t(0), LOCALE_enUS, 0, false, false, 0);
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> names;  // empty: names from the database
    RandomPlayerbotFactory factory;

    Player* created = nullptr;
    for (uint32 i = 0; i < MAX_CREATE_TRIES && !created; ++i)
    {
        uint8 cls = 0;
        while (!cls)
        {
            cls = uint8(urand(CLASS_WARRIOR, CLASS_DRUID));
            if (!((1 << (cls - 1)) & CLASSMASK_ALL_PLAYABLE) || !sChrClassesStore.LookupEntry(cls) ||
                ((1 << (cls - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK)))
                cls = 0;
        }

        Player* player = factory.CreateRandomBot(session, cls, names);
        if (!player)
            continue;

        // the factory rolls the faction itself
        if (player->GetTeamId(true) != team)
        {
            player->CleanupsBeforeDelete();
            delete player;
            continue;
        }

        created = player;
    }

    if (!created)
    {
        delete session;
        return 0;
    }

    created->SaveToDB(true, false);
    sCharacterCache->AddCharacterCacheEntry(created->GetGUID(), accountId, created->GetName(), created->getGender(),
                                            created->getRace(), created->getClass(), created->GetLevel());

    uint32 guid = created->GetGUID().GetCounter();
    PoolChar& poolChar = m_pool[guid];
    poolChar.account = accountId;
    poolChar.team = team;
    poolChar.level = created->GetLevel();
    poolChar.busy = true;
    ++m_accounts[accountId];

    LOG_DEBUG("playerbots", "BattlegroundBotFill: created bot character {} (guid {}) on account {}", created->GetName(), guid, accountId);

    created->CleanupsBeforeDelete();
    delete created;
    delete session;
    return guid;
}

void BattlegroundBotFill::UpdateBots(time_t now, KeySet const& activeKeys)
{
    uint32 logins = 0;
    uint32 prepares = 0;

    for (auto itr = m_bots.begin(); itr != m_bots.end();)
    {
        uint32 guid = itr->first;
        FillBot& fillBot = itr->second;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        bool keyActive = m_enabled && activeKeys.count(fillBot.key) != 0;
        bool keep = true;

        switch (fillBot.state)
        {
            case BotState::WAIT_LOGIN:
                if (!keyActive || (!bot && FindConnected(guid)))
                {
                    keep = false;
                    break;
                }
                if (now < fillBot.stateTime || logins >= MAX_LOGINS_PER_UPDATE)
                    break;

                if (!bot)
                {
                    sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(guid), 0);
                    ++logins;
                }
                fillBot.state = BotState::LOGGING_IN;
                fillBot.stateTime = now;
                break;
            case BotState::LOGGING_IN:
                if (bot && bot->IsInWorld() && GET_PLAYERBOT_AI(bot))
                {
                    fillBot.state = BotState::PREPARING;
                    fillBot.stateTime = now;
                }
                else if (now - fillBot.stateTime > LOGIN_TIMEOUT)
                {
                    LOG_ERROR("playerbots", "BattlegroundBotFill: bot {} did not log in", guid);
                    keep = false;
                }
                break;
            case BotState::PREPARING:
                if (!bot || !keyActive)
                {
                    keep = false;
                    break;
                }
                if (prepares >= MAX_PREPARES_PER_UPDATE || !bot->IsInWorld() || bot->IsBeingTeleported())
                    break;

                // logged in inside a match that still runs (it logged out there): leave it first
                if (bot->InBattleground())
                {
                    bot->LeaveBattleground();
                    break;
                }

                ++prepares;
                if (!PrepareBot(bot, fillBot) || !JoinQueue(bot, fillBot.key))
                {
                    keep = false;
                    break;
                }
                fillBot.state = BotState::QUEUED;
                fillBot.stateTime = now;
                break;
            case BotState::QUEUED:
                keep = bot && KeepQueuedBot(bot, fillBot, keyActive, now);
                if (keep && bot->IsInvitedForBattlegroundQueueType(fillBot.key.queueTypeId) && !bot->InBattleground())
                {
                    auto invited = m_invitedSince.emplace(guid, now).first;
                    if (now - invited->second >= INVITE_ACCEPT_DELAY)
                    {
                        AcceptInvitation(bot, fillBot.key.queueTypeId);
                        invited->second = now;
                    }
                }
                else
                    m_invitedSince.erase(guid);
                break;
            case BotState::LEAVING:
                keep = bot && now - fillBot.stateTime < LEAVE_TIMEOUT &&
                       (bot->InBattleground() || !bot->IsInWorld() || bot->IsBeingTeleported());
                break;
        }

        if (!keep && bot && bot->InBattleground() && fillBot.state != BotState::LEAVING)
        {
            bot->LeaveBattleground();
            fillBot.state = BotState::LEAVING;
            fillBot.stateTime = now;
            keep = true;
        }

        if (keep)
        {
            ++itr;
            continue;
        }

        m_invitedSince.erase(guid);
        ReleaseBot(guid, bot);
        itr = m_bots.erase(itr);
    }
}

bool BattlegroundBotFill::PrepareBot(Player* bot, FillBot const& fillBot)
{
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(fillBot.key.queueTypeId);

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || bot->InBattleground() || bot->InBattlegroundQueueForBattlegroundQueueType(fillBot.key.queueTypeId))
        return false;

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (bot->GetLevel() != fillBot.level)
    {
        PlayerbotFactory factory(bot, fillBot.level);
        factory.Randomize(false);
    }

    // revive, repair, full health / mana, consumables
    sRandomPlayerbotMgr.Refresh(bot);
    // left over from logging out of an earlier match
    bot->RemoveAurasDueToSpell(SPELL_DESERTER);

    m_pool[bot->GetGUID().GetCounter()].level = bot->GetLevel();

    if (bot->GetLevel() != fillBot.level || !bot->HasFreeBattlegroundQueueId() || !bot->GetBGAccessByLevel(bgTypeId))
    {
        LOG_ERROR("playerbots", "BattlegroundBotFill: bot {} (level {}) can not join {} for level {}", bot->GetName(),
                  bot->GetLevel(), QueueName(fillBot.key.queueTypeId), fillBot.level);
        return false;
    }

    LOG_DEBUG("playerbots", "BattlegroundBotFill: bot {} ({}, level {}) joins {}", bot->GetName(),
              bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), QueueName(fillBot.key.queueTypeId));
    return true;
}

bool BattlegroundBotFill::JoinQueue(Player* bot, FillKey const& key)
{
    // same as a solo join at a battlemaster (WorldSession::HandleBattlemasterJoinOpcode / HandleBattlemasterJoinArena)
    BattlegroundQueueTypeId queueTypeId = key.queueTypeId;
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    uint8 arenaType = BattlegroundMgr::BGArenaType(queueTypeId);
    Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
    if (!bgTemplate)
        return false;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), bot->GetLevel());
    if (!bracketEntry || bracketEntry->GetBracketId() != key.bracketId)
        return false;

    // an arena queue can not be shared with any other queue
    if ((arenaType && bot->InBattlegroundQueue()) || !bot->HasFreeBattlegroundQueueId())
        return false;

    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    GroupQueueInfo* ginfo = bgQueue.AddGroup(bot, nullptr, bgTypeId, bracketEntry, arenaType, false, false, 0, 0);
    uint32 avgWaitTime = bgQueue.GetAverageQueueWaitTime(ginfo);
    uint32 queueSlot = bot->AddBattlegroundQueueId(queueTypeId);

    WorldPacket data;
    sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, queueSlot, STATUS_WAIT_QUEUE, avgWaitTime, 0, arenaType, TEAM_NEUTRAL);
    bot->GetSession()->SendPacket(&data);

    sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, queueTypeId, bgTypeId, bracketEntry->GetBracketId());
    return true;
}

bool BattlegroundBotFill::KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const
{
    BattlegroundQueueTypeId queueTypeId = fillBot.key.queueTypeId;

    // stays for the whole match while it has real players
    if (bot->InBattleground())
    {
        Battleground* bg = bot->GetBattleground();
        return keyActive || (bg && HasRealPlayer(bg));
    }

    // the AI (or AcceptInvitation) accepts the invitation
    if (bot->IsInvitedForBattlegroundQueueType(queueTypeId))
        return true;

    if (bot->InBattlegroundQueueForBattlegroundQueueType(queueTypeId))
        return keyActive;

    // just ported in or out - otherwise the match is over
    return bot->IsBeingTeleported() || now - fillBot.stateTime < QUEUE_JOIN_GRACE;
}

void BattlegroundBotFill::AcceptInvitation(Player* bot, BattlegroundQueueTypeId queueTypeId)
{
    // the bot's "bg status" action did not take it in: send the port the client would
    BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
    GroupQueueInfo ginfo;
    if (!bgQueue.GetPlayerGroupInfoData(bot->GetGUID(), &ginfo) || !ginfo.IsInvitedToBGInstanceGUID)
        return;

    uint8 arenaType = BattlegroundMgr::BGArenaType(queueTypeId);
    BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
    // BattlegroundMgr::GetBattleground() does not find an arena by the all-arenas type
    Battleground* bg = sBattlegroundMgr->GetBattleground(ginfo.IsInvitedToBGInstanceGUID, arenaType ? BATTLEGROUND_TYPE_NONE : bgTypeId);
    if (!bg)
        return;

    LOG_INFO("playerbots", "BattlegroundBotFill: bot {} accepts its {} invitation", bot->GetName(), QueueName(queueTypeId));

    WorldPacket packet(CMSG_BATTLEFIELD_PORT, 9);
    packet << uint8(arenaType) << uint8(0) << uint32(bg->GetBgTypeID()) << BATTLEFIELD_PORT_UNK << BATTLEFIELD_PORT_ENTER;
    bot->GetSession()->HandleBattleFieldPortOpcode(packet);
}

void BattlegroundBotFill::ReleaseBot(uint32 guid, Player* bot)
{
    auto itr = m_pool.find(guid);
    if (itr != m_pool.end())
    {
        itr->second.busy = false;
        if (bot)
            itr->second.level = bot->GetLevel();
    }

    if (bot)
    {
        LOG_DEBUG("playerbots", "BattlegroundBotFill: bot {} logs out", bot->GetName());
        sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
    }
}

void BattlegroundBotFill::LogoutStrayPoolBots()
{
    // pool characters online without a job, e.g. a login that finished after its timeout
    for (auto const& itr : m_pool)
    {
        if (itr.second.busy)
            continue;

        if (Player* bot = sRandomPlayerbotMgr.GetPlayerBot(itr.first))
            if (bot->IsInWorld() && !bot->GetSession()->IsLoggingOut() && !bot->InBattleground())
                sRandomPlayerbotMgr.LogoutPlayerBot(bot->GetGUID());
    }
}

bool BattlegroundBotFill::IsWaitingInQueue(Player* player, BattlegroundQueueTypeId queueTypeId)
{
    return player->InBattlegroundQueueForBattlegroundQueueType(queueTypeId) &&
           !player->IsInvitedForBattlegroundQueueType(queueTypeId) &&
           !IsInBattlegroundOfQueue(player, queueTypeId);
}

bool BattlegroundBotFill::IsInBattlegroundOfQueue(Player* player, BattlegroundQueueTypeId queueTypeId)
{
    if (!player->InBattleground())
        return false;

    if (BattlegroundMgr::BGArenaType(queueTypeId))
        return player->InArena();

    return player->GetBattlegroundTypeId() == BattlegroundMgr::BGTemplateId(queueTypeId);
}

bool BattlegroundBotFill::HasRealPlayer(Battleground* bg)
{
    for (auto const& itr : bg->GetPlayers())
        if (IsRealPlayer(itr.second))
            return true;

    return false;
}

char const* BattlegroundBotFill::QueueName(BattlegroundQueueTypeId queueTypeId)
{
    switch (queueTypeId)
    {
        case BATTLEGROUND_QUEUE_AV:  return "Alterac Valley";
        case BATTLEGROUND_QUEUE_WS:  return "Warsong Gulch";
        case BATTLEGROUND_QUEUE_AB:  return "Arathi Basin";
        case BATTLEGROUND_QUEUE_EY:  return "Eye of the Storm";
        case BATTLEGROUND_QUEUE_SA:  return "Strand of the Ancients";
        case BATTLEGROUND_QUEUE_IC:  return "Isle of Conquest";
        case BATTLEGROUND_QUEUE_2v2: return "2v2 skirmish";
        case BATTLEGROUND_QUEUE_3v3: return "3v3 skirmish";
        case BATTLEGROUND_QUEUE_5v5: return "5v5 skirmish";
        default:                     return "unknown queue";
    }
}

class BattlegroundBotFillPlayerScript : public PlayerScript
{
public:
    BattlegroundBotFillPlayerScript() : PlayerScript("BattlegroundBotFillPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JOIN_BG,
        PLAYERHOOK_ON_PLAYER_JOIN_ARENA
    }) {}

    void OnPlayerJoinBG(Player* player) override { BattlegroundBotFill::instance().OnQueueJoin(player); }
    void OnPlayerJoinArena(Player* player) override { BattlegroundBotFill::instance().OnQueueJoin(player); }
};

class BattlegroundBotFillWorldScript : public WorldScript
{
public:
    BattlegroundBotFillWorldScript() : WorldScript("BattlegroundBotFillWorldScript", {
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnUpdate(uint32 diff) override { BattlegroundBotFill::instance().Update(diff); }
};

void AddBattlegroundBotFillScripts()
{
    new BattlegroundBotFillPlayerScript();
    new BattlegroundBotFillWorldScript();
}

#endif
