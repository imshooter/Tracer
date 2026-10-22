#include <open.mp>

/**
 * A gamemode that gives the Tracer component a busy server to profile.
 *
 * Bots join a few at a time and walk around Los Santos airport. Each one looks
 * up the zone it stands in when it leaves the last one, picks up the coins it
 * walks over, chats and uses a command now and then. Every bot is saved to
 * `scriptfiles/stress-example.txt` every ten seconds, and one bot leaves and
 * comes back every twenty. After `STRESS_SECONDS` the server shuts down and
 * the component writes its report to `tracer/`.
 *
 * Build it with `-d3` to see the time of every function in the report. The
 * server needs `max_bots` and `max_players` of at least `STRESS_BOTS` in
 * config.json.
 */

/**
 * # Header
 */

#if !defined STRESS_BOTS
    #define STRESS_BOTS (50)
#endif

#if !defined STRESS_SECONDS
    #define STRESS_SECONDS (180)
#endif

#if !defined STRESS_ZONES_PER_SIDE
    #define STRESS_ZONES_PER_SIDE (16)
#endif

#if !defined STRESS_COINS
    #define STRESS_COINS (500)
#endif

#define STRESS_ZONES                    (STRESS_ZONES_PER_SIDE * STRESS_ZONES_PER_SIDE)
#define STRESS_CENTRE_X                 (1700.0)
#define STRESS_CENTRE_Y                 (-2500.0)
#define STRESS_CENTRE_Z                 (13.5469)
#define STRESS_RANGE                    (80)
#define STRESS_COIN_RANGE               (3.0)
#define STRESS_COIN_VALUE               (10)
#define STRESS_PULSE                    (500)
#define STRESS_SAVE_INTERVAL            (10000)
#define STRESS_CHURN_INTERVAL           (20000)
#define STRESS_AWAY_TIME                (3000)
#define STRESS_SAVE_FILE                "stress-example.txt"

#define MAX_ZONE_NAME                   (16)
#define INVALID_ZONE                    (-1)

static enum _:E_ZONE_DATA {
    E_ZONE_NAME[MAX_ZONE_NAME],
    Float:E_ZONE_MIN_X,
    Float:E_ZONE_MIN_Y,
    Float:E_ZONE_MAX_X,
    Float:E_ZONE_MAX_Y
};

static enum _:E_COIN_DATA {
    Float:E_COIN_X,
    Float:E_COIN_Y
};

/**
 * A bot slot: the player it is while connected, and when it comes back or
 * acts next.
 */
static enum _:E_BOT_DATA {
    E_BOT_PLAYER,
    E_BOT_BACK_AT,
    E_BOT_CHAT_AT,
    E_BOT_COMMAND_AT
};

static enum _:E_PLAYER_DATA {
    E_PLAYER_ZONE,
    E_PLAYER_COINS
};

static
    gZoneData[STRESS_ZONES][E_ZONE_DATA],
    gCoinData[STRESS_COINS][E_COIN_DATA],
    gBotData[STRESS_BOTS][E_BOT_DATA],
    gPlayerData[MAX_PLAYERS][E_PLAYER_DATA]
;

main() {}

/**
 * # Internal
 */

/**
 * A random point around the centre, in whole metres.
 */
static stock RandomPointInternal(&Float:x, &Float:y) {
    x = STRESS_CENTRE_X + float(random(STRESS_RANGE * 2) - STRESS_RANGE);
    y = STRESS_CENTRE_Y + float(random(STRESS_RANGE * 2) - STRESS_RANGE);
}

/**
 * Splits the area around the centre into a grid of named zones.
 */
static stock MakeZonesInternal() {
    new const
        Float:size = float(STRESS_RANGE * 2) / float(STRESS_ZONES_PER_SIDE)
    ;

    for (new i; i != STRESS_ZONES; ++i) {
        new const
            row = i / STRESS_ZONES_PER_SIDE,
            column = i % STRESS_ZONES_PER_SIDE
        ;

        gZoneData[i][E_ZONE_MIN_X] = STRESS_CENTRE_X - float(STRESS_RANGE) + size * float(column);
        gZoneData[i][E_ZONE_MIN_Y] = STRESS_CENTRE_Y - float(STRESS_RANGE) + size * float(row);
        gZoneData[i][E_ZONE_MAX_X] = gZoneData[i][E_ZONE_MIN_X] + size;
        gZoneData[i][E_ZONE_MAX_Y] = gZoneData[i][E_ZONE_MIN_Y] + size;
        format(gZoneData[i][E_ZONE_NAME], MAX_ZONE_NAME, "block %c%i", 'A' + row, column + 1);
    }
}

static stock bool:IsPointInZoneInternal(zone, Float:x, Float:y) {
    return x >= gZoneData[zone][E_ZONE_MIN_X] && x < gZoneData[zone][E_ZONE_MAX_X] && y >= gZoneData[zone][E_ZONE_MIN_Y] && y < gZoneData[zone][E_ZONE_MAX_Y];
}

/**
 * The zone that holds a point, found by trying every zone in turn.
 */
static stock FindZoneInternal(Float:x, Float:y) {
    for (new i; i != STRESS_ZONES; ++i) {
        if (IsPointInZoneInternal(i, x, y)) {
            return i;
        }
    }

    return INVALID_ZONE;
}

static stock PlaceCoinInternal(coin) {
    RandomPointInternal(gCoinData[coin][E_COIN_X], gCoinData[coin][E_COIN_Y]);
}

/**
 * Gives a player every coin in reach and moves those coins somewhere else.
 */
static stock CollectCoinsInternal(playerid) {
    for (new i; i != STRESS_COINS; ++i) {
        if (!IsPlayerInRangeOfPoint(playerid, STRESS_COIN_RANGE, gCoinData[i][E_COIN_X], gCoinData[i][E_COIN_Y], STRESS_CENTRE_Z)) {
            continue;
        }

        GivePlayerMoney(playerid, STRESS_COIN_VALUE);
        ++gPlayerData[playerid][E_PLAYER_COINS];
        PlaceCoinInternal(i);
    }
}

static stock WalkInternal(npcid) {
    new
        Float:x,
        Float:y
    ;

    RandomPointInternal(x, y);
    NPC_Move(npcid, x, y, STRESS_CENTRE_Z, NPC_MOVE_TYPE_WALK);
}

/**
 * Connects the bot of a slot and spawns it.
 */
static stock bool:CreateBotInternal(slot) {
    new
        name[MAX_PLAYER_NAME]
    ;

    format(name, sizeof (name), "StressBot_%i", slot);

    new const
        npcid = NPC_Create(name)
    ;

    if (npcid == INVALID_NPC_ID) {
        return false;
    }

    new const
        now = GetTickCount()
    ;

    gBotData[slot][E_BOT_PLAYER] = npcid;
    gBotData[slot][E_BOT_CHAT_AT] = now + random(20000);
    gBotData[slot][E_BOT_COMMAND_AT] = now + random(30000);
    NPC_Spawn(npcid);

    return true;
}

/**
 * Joins up to two bots whose time to come back has passed, and lets every
 * connected bot collect coins, chat and use a command.
 */
forward @Stress_Pulse();
public @Stress_Pulse() {
    new const
        now = GetTickCount()
    ;

    new
        created
    ;

    for (new i; i != STRESS_BOTS; ++i) {
        new const
            playerid = gBotData[i][E_BOT_PLAYER]
        ;

        if (playerid == INVALID_PLAYER_ID) {
            if (created != 2 && now >= gBotData[i][E_BOT_BACK_AT] && CreateBotInternal(i)) {
                ++created;
            }

            continue;
        }

        CollectCoinsInternal(playerid);

        if (now >= gBotData[i][E_BOT_CHAT_AT]) {
            gBotData[i][E_BOT_CHAT_AT] = now + 20000 + random(20000);
            CallLocalFunction("OnPlayerText", "is", playerid, "Hello from the stress example.");
        }

        if (now >= gBotData[i][E_BOT_COMMAND_AT]) {
            gBotData[i][E_BOT_COMMAND_AT] = now + 30000 + random(30000);
            CallLocalFunction("OnPlayerCommandText", "is", playerid, "/stats");
        }
    }

    return 1;
}

/**
 * Writes every connected bot to `STRESS_SAVE_FILE`.
 */
forward @Stress_Save();
public @Stress_Save() {
    new const
        File:file = fopen(STRESS_SAVE_FILE, io_write)
    ;

    if (!file) {
        return 1;
    }

    new
        line[128],
        name[MAX_PLAYER_NAME],
        Float:x,
        Float:y,
        Float:z
    ;

    for (new i; i != STRESS_BOTS; ++i) {
        new const
            playerid = gBotData[i][E_BOT_PLAYER]
        ;

        if (playerid == INVALID_PLAYER_ID) {
            continue;
        }

        GetPlayerName(playerid, name);
        GetPlayerPos(playerid, x, y, z);
        format(line, sizeof (line), "%s %i %i %.2f %.2f %.2f\r\n", name, GetPlayerMoney(playerid), gPlayerData[playerid][E_PLAYER_COINS], x, y, z);
        fwrite(file, line);
    }

    fclose(file);

    return 1;
}

/**
 * Sends a random bot away. It comes back after `STRESS_AWAY_TIME`.
 */
forward @Stress_Churn();
public @Stress_Churn() {
    new const
        playerid = gBotData[random(STRESS_BOTS)][E_BOT_PLAYER]
    ;

    if (playerid == INVALID_PLAYER_ID) {
        return 1;
    }

    NPC_Destroy(playerid);

    return 1;
}

forward @Stress_End();
public @Stress_End() {
    print("[stress] Done. Tracer writes its report as the server exits.");
    SendRconCommand("exit");

    return 1;
}

/**
 * # Calls
 */

public OnGameModeInit() {
    SetGameModeText("Tracer stress");
    AddPlayerClass(0, STRESS_CENTRE_X, STRESS_CENTRE_Y, STRESS_CENTRE_Z, 0.0);
    MakeZonesInternal();

    for (new i; i != STRESS_COINS; ++i) {
        PlaceCoinInternal(i);
    }

    for (new i; i != STRESS_BOTS; ++i) {
        gBotData[i][E_BOT_PLAYER] = INVALID_PLAYER_ID;
    }

    SetTimer("@Stress_Pulse", STRESS_PULSE, true);
    SetTimer("@Stress_Save", STRESS_SAVE_INTERVAL, true);
    SetTimer("@Stress_Churn", STRESS_CHURN_INTERVAL, true);
    SetTimer("@Stress_End", STRESS_SECONDS * 1000, false);
    printf("[stress] %i bots for %i seconds.", STRESS_BOTS, STRESS_SECONDS);

    return 1;
}

public OnPlayerConnect(playerid) {
    gPlayerData[playerid][E_PLAYER_ZONE] = INVALID_ZONE;
    gPlayerData[playerid][E_PLAYER_COINS] = 0;

    return 1;
}

public OnPlayerDisconnect(playerid, reason) {
    for (new i; i != STRESS_BOTS; ++i) {
        if (gBotData[i][E_BOT_PLAYER] != playerid) {
            continue;
        }

        gBotData[i][E_BOT_PLAYER] = INVALID_PLAYER_ID;
        gBotData[i][E_BOT_BACK_AT] = GetTickCount() + STRESS_AWAY_TIME;

        break;
    }

    return 1;
}

/**
 * A bot spawns where `NPC_Spawn` puts it, so it is moved to a random point
 * before it starts walking.
 */
public OnNPCSpawn(npcid) {
    new
        Float:x,
        Float:y
    ;

    RandomPointInternal(x, y);
    NPC_SetPos(npcid, x, y, STRESS_CENTRE_Z);
    WalkInternal(npcid);

    return 1;
}

public OnNPCFinishMove(npcid) {
    WalkInternal(npcid);

    return 1;
}

/**
 * Finds the zone of a player again only when it left the last one.
 */
public OnPlayerUpdate(playerid) {
    new
        Float:x,
        Float:y,
        Float:z
    ;

    GetPlayerPos(playerid, x, y, z);

    new const
        zone = gPlayerData[playerid][E_PLAYER_ZONE]
    ;

    if (zone != INVALID_ZONE && IsPointInZoneInternal(zone, x, y)) {
        return 1;
    }

    new const
        next = FindZoneInternal(x, y)
    ;

    gPlayerData[playerid][E_PLAYER_ZONE] = next;

    if (next == INVALID_ZONE) {
        return 1;
    }

    SendClientMessage(playerid, -1, "You entered %s.", gZoneData[next][E_ZONE_NAME]);

    return 1;
}

public OnPlayerText(playerid, text[]) {
    new
        name[MAX_PLAYER_NAME]
    ;

    GetPlayerName(playerid, name);

    new const
        zone = gPlayerData[playerid][E_PLAYER_ZONE]
    ;

    if (zone == INVALID_ZONE) {
        SendClientMessageToAll(-1, "%s: %s", name, text);

        return 0;
    }

    SendClientMessageToAll(-1, "[%s] %s: %s", gZoneData[zone][E_ZONE_NAME], name, text);

    return 0;
}

public OnPlayerCommandText(playerid, cmdtext[]) {
    if (strcmp(cmdtext, "/stats", true) != 0) {
        return 0;
    }

    SendClientMessage(playerid, -1, "Coins %i, money %i.", gPlayerData[playerid][E_PLAYER_COINS], GetPlayerMoney(playerid));

    return 1;
}
