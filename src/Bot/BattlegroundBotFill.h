/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BATTLEGROUNDBOTFILL_H
#define PLAYERBOTS_BATTLEGROUNDBOTFILL_H

#ifdef USE_CUSTOM_CHANGES

#include "Common.h"
#include "DBCEnums.h"
#include "SharedDefines.h"

#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <tuple>

class Battleground;
class Player;

/**
 * Fills the battleground (or arena skirmish) queue a real player joins with dedicated playerbots,
 * so both teams reach their maximum size and the queue pops right away.
 *
 * The bots come from their own account pool (ACCOUNT_PREFIX, never picked by the random bot rotation,
 * which only uses the RandomBotAccountPrefix accounts): offline pool characters are logged in and set
 * to the queuing player's level, new characters are created when the pool runs short. A bot logs out
 * again once its match is over, or when the player leaves the queue before the match started.
 *
 * Active when the playerbots are enabled together with Playerbots.RandomBotJoinBG and
 * Playerbots.BattlegroundBotFill.
 */
class BattlegroundBotFill
{
public:
    static BattlegroundBotFill& instance()
    {
        static BattlegroundBotFill instance;
        return instance;
    }

    // a player joined a battleground or arena queue (called from the join hooks)
    void OnQueueJoin(Player* player);
    // world thread update
    void Update(uint32 diff);

private:
    BattlegroundBotFill();

    enum class BotState
    {
        WAIT_LOGIN,     // assigned, waiting for its login (new characters wait for their save)
        LOGGING_IN,     // login sent
        PREPARING,      // in world, gets level / gear and joins the queue
        QUEUED,         // queue join done; in queue, invited or in the match
        LEAVING         // leaves its match before the logout (a logout inside puts it back there at the next login)
    };

    struct FillKey
    {
        BattlegroundQueueTypeId queueTypeId;
        BattlegroundBracketId bracketId;

        bool operator<(FillKey const& other) const
        {
            return std::tie(queueTypeId, bracketId) < std::tie(other.queueTypeId, other.bracketId);
        }
        bool operator==(FillKey const& other) const
        {
            return queueTypeId == other.queueTypeId && bracketId == other.bracketId;
        }
    };

    // a real player in a queue
    struct FillRequest
    {
        FillKey key;
        uint32 level;
        time_t joinTime;
    };

    struct FillBot
    {
        FillKey key;
        TeamId team;
        uint32 level;
        BotState state;
        time_t stateTime;           // when the state was entered, for WAIT_LOGIN the earliest login time
    };

    struct PoolChar
    {
        uint32 account;
        TeamId team;
        uint32 level;
        bool busy;
    };

    struct CreateJob
    {
        FillKey key;
        TeamId team;
        uint32 level;
    };

    typedef std::map<FillKey, uint32> KeyLevelMap;      // key -> level the bots get
    typedef std::set<FillKey> KeySet;

    bool IsEnabled() const;
    void Initialize();

    void UpdateRequests(time_t now, KeyLevelMap& waitingKeys, KeySet& activeKeys);
    void FillQueue(FillKey const& key, uint32 level, time_t now);
    void CountQueued(FillKey const& key, bool isArena, uint32 (&count)[PVP_TEAMS_COUNT]) const;
    void AddBot(FillKey const& key, TeamId team, uint32 level, time_t now);
    uint32 PickPoolChar(TeamId team, uint32 level);

    void UpdateCreateJobs(time_t now, KeySet const& activeKeys);
    uint32 GetAccountWithFreeSlot();
    uint32 CreateBotCharacter(uint32 accountId, TeamId team);

    void UpdateBots(time_t now, KeySet const& activeKeys);
    bool PrepareBot(Player* bot, FillBot const& fillBot);
    bool JoinQueue(Player* bot, FillKey const& key);
    bool KeepQueuedBot(Player* bot, FillBot const& fillBot, bool keyActive, time_t now) const;
    void AcceptInvitation(Player* bot, BattlegroundQueueTypeId queueTypeId);
    void ReleaseBot(uint32 guid, Player* bot);
    void LogoutStrayPoolBots();

    static bool IsWaitingInQueue(Player* player, BattlegroundQueueTypeId queueTypeId);
    static bool IsInBattlegroundOfQueue(Player* player, BattlegroundQueueTypeId queueTypeId);
    static bool HasRealPlayer(Battleground* bg);
    static char const* QueueName(BattlegroundQueueTypeId queueTypeId);

    std::map<std::pair<uint32, BattlegroundQueueTypeId>, FillRequest> m_requests;  // (player guid, queue type)
    std::map<uint32, FillBot> m_bots;                                               // bot guid
    std::map<uint32, PoolChar> m_pool;                                              // character guid
    std::map<uint32, uint32> m_accounts;                                            // account id -> characters
    std::map<uint32, time_t> m_invitedSince;                                        // bot guid -> first seen invited
    std::deque<CreateJob> m_createJobs;
    std::string m_pendingAccount;                                                   // account being created
    uint32 m_nextAccountIndex;
    uint32 m_updateTimer;
    time_t m_createBlockedUntil;
    bool m_initialized;
    bool m_enabled;
};

void AddBattlegroundBotFillScripts();

#endif

#endif
