/**
 *
 *   ██████╗   ██╗ ███████╗ ███╗   ███╗ ██╗   ██╗
 *   ██╔══██╗ ███║ ██╔════╝ ████╗ ████║ ██║   ██║
 *   ██████╔╝ ╚██║ █████╗   ██╔████╔██║ ██║   ██║
 *   ██╔══██╗  ██║ ██╔══╝   ██║╚██╔╝██║ ██║   ██║
 *   ██║  ██║  ██║ ███████╗ ██║ ╚═╝ ██║ ╚██████╔╝
 *   ╚═╝  ╚═╝  ╚═╝ ╚══════╝ ╚═╝     ╚═╝  ╚═════╝
 *
 * @license GNU GENERAL PUBLIC LICENSE - Version 2, June 1991
 *          See LICENSE file for further information
 */

#include "barrack_handler.h"
#include "barrack_builder.h"
#include "common/utils/random.h"
#include "common/packet/packet.h"
#include "common/server/worker.h"
#include "common/commander/commander.h"
#include "common/commander/inventory.h"
#include "common/item/item.h"
#include "common/packet/packet_stream.h"
#include "common/redis/fields/redis_session.h"
#include "common/redis/fields/redis_game_session.h"
#include "common/redis/fields/redis_socket_session.h"
#include "common/mysql/fields/mysql_account_session.h"
#include "common/mysql/fields/mysql_commander.h"

/** Read the passport and accepts or refuse the authentification */
static PacketHandlerState barrackHandlerLoginByPassport  (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Read the login / password and accepts or refuse the authentification */
static PacketHandlerState barrackHandlerLogin            (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Start the barrack : call other handlers that initializes the barrack */
static PacketHandlerState barrackHandlerStartBarrack     (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Once the commander list has been received, request to start the barrack */
static PacketHandlerState barrackHandlerCurrentBarrack   (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Change a barrack name */
static PacketHandlerState barrackHandlerBarrackNameChange(Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Create a commander */
static PacketHandlerState barrackHandlerCommanderCreate  (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Send a list of zone servers */
static PacketHandlerState barrackHandlerCommanderDestroy (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Change the commander position in the barrack */
static PacketHandlerState barrackHandlerCommanderMove    (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Request for the player to enter in game */
static PacketHandlerState barrackHandlerStartGame        (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);
/** Request for the player to logout */
static PacketHandlerState barrackHandlerLogout        (Worker *self, Session *session, uint8_t *packet, size_t packetSize, zmsg_t *reply);

/**
 * @brief barrackHandlers is a global table containing all the barrack handlers.
 */
const PacketHandler barrackHandlers[PACKET_TYPE_COUNT] = {
    #define REGISTER_PACKET_HANDLER(packetName, handler) \
       [packetName] = {handler, STRINGIFY(packetName)}

    REGISTER_PACKET_HANDLER(CB_LOGIN,              barrackHandlerLogin),
    REGISTER_PACKET_HANDLER(CB_LOGIN_BY_PASSPORT,  barrackHandlerLoginByPassport),
    REGISTER_PACKET_HANDLER(CB_START_BARRACK,      barrackHandlerStartBarrack),
    REGISTER_PACKET_HANDLER(CB_CURRENT_BARRACK,    barrackHandlerCurrentBarrack),
    REGISTER_PACKET_HANDLER(CB_BARRACKNAME_CHANGE, barrackHandlerBarrackNameChange),
    REGISTER_PACKET_HANDLER(CB_COMMANDER_CREATE,   barrackHandlerCommanderCreate),
    REGISTER_PACKET_HANDLER(CB_COMMANDER_DESTROY,  barrackHandlerCommanderDestroy),
    REGISTER_PACKET_HANDLER(CB_COMMANDER_MOVE,     barrackHandlerCommanderMove),
    // REGISTER_PACKET_HANDLER(CB_JUMP,               barrackHandlerJump),
    REGISTER_PACKET_HANDLER(CB_START_GAME,         barrackHandlerStartGame),
    REGISTER_PACKET_HANDLER(CB_LOGOUT,         barrackHandlerLogout),

    #undef REGISTER_PACKET_HANDLER
};

static PacketHandlerState barrackHandlerLogin(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;

    #pragma pack(push, 1)
    struct {
        uint8_t login[ACCOUNT_SESSION_LOGIN_MAXSIZE];
        uint8_t md5Password[17];
        uint8_t unk1[5]; // Game version?
    } *clientPacket = (void *) packet;
    #pragma pack(pop)

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_LOGIN);

    // Get accountData from database
    AccountSession accountSession;
    bool goodCredentials = false;

    // Initialize Account Session
    if (!(accountSessionInit(
        &accountSession,
        clientPacket->login,
        session->socket.sessionKey,
        session->game.accountSession.privilege)))
    {
        error("Cannot initialize the account session.");
        goto cleanup;
    }

    if (!(mySqlGetAccountData(
        self->sqlConn,
        clientPacket->login,
        clientPacket->md5Password,
        &accountSession,
        &goodCredentials)))
    {
        error("Cannot get SQL account data.");
        goto cleanup;
    }

    // Check if user/pass incorrect
    if (!goodCredentials) {
        barrackBuilderMessage(BC_MESSAGE_USER_PASS_INCORRECT_1, "", reply);
        status = PACKET_HANDLER_OK;
        goto cleanup;
    }

    // Check if user is banned
    if (accountSession.isBanned) {
        barrackBuilderMessage(BC_MESSAGE_ACCOUNT_BLOCKED_2, "", reply);
        status = PACKET_HANDLER_OK;
        goto cleanup;
    }

    // Check if user is already logged-in
    // TODO

    // update the session
    // authentication OK!
    session->socket.authenticated = true;
    session->socket.accountId = accountSession.accountId;
    session->game.accountSession = accountSession;

    info("AccountID %llx generated !", session->socket.accountId);

    barrackBuilderLoginOk(
        session->socket.accountId,
        session->game.accountSession.login,
        "*0FC621B82495C18DEC8D8D956C82297BEAAAA858",
        session->game.accountSession.privilege,
        reply
    );

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    return status;
}

static PacketHandlerState barrackHandlerLoginByPassport(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;

    #pragma pack(push, 1)
    struct {
        ServerPacketHeader header;
        uint32_t unk1;
        uint8_t unk2; // 08
        uint16_t unk3; // 0110
        uint8_t passport[1011];
        uint32_t unk4;
        uint16_t unk5;
        uint64_t clientId;
        uint32_t clientId2;
    } *clientPacket = (void *) packet;
    #pragma pack(pop)

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_LOGIN_BY_PASSPORT);

    // authenticate here
    // TODO

    // authentication OK!
    session->socket.authenticated = true;

    // update the session
    // ==== gives a random account ====
    session->socket.accountId = r1emuGenerateRandom64(&self->seed);

    if (!(accountSessionInit(
        &session->game.accountSession,
        session->game.accountSession.login,
        session->socket.sessionKey,
        ACCOUNT_SESSION_PRIVILEGES_ADMIN)))
    {
        error("Cannot initialize the account session.");
        goto cleanup;
    }

    snprintf(session->game.accountSession.login,
      sizeof(session->game.accountSession.login), "%llX", session->socket.accountId);
    info("Account %s generated !", session->game.accountSession.login);
    // ==================================

    barrackBuilderLoginOk(
        session->socket.accountId,
        session->game.accountSession.login,
        "*0FC621B82495C18DEC8D8D956C82297BEAAAA858",
        session->game.accountSession.privilege,
        reply
    );

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    return status;
}

static PacketHandlerState barrackHandlerStartGame(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;

    #pragma pack(push, 1)
    struct {
        uint16_t routerId;
        uint8_t commanderIndex;
    } *clientPacket = (void *) packet;
    #pragma pack(pop)

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_START_GAME);

    // Check if commanderIndex exists
    if (!session->game.accountSession.commanders[clientPacket->commanderIndex-1]) {
        error("Selected commander index doesnt exist in account");
        return PACKET_HANDLER_ERROR;
    }

    // Retrieve zone servers IPs from Redis
    // Fake IPs here until we can retrieve the IPs database
    uint32_t zoneServerIps[] = {
        *(uint32_t *)((char[]) {127, 0,   0,   1}),
        *(uint32_t *)((char[]) {46,  105, 97,  46}),
        *(uint32_t *)((char[]) {192, 168, 33,  10}),
        *(uint32_t *)((char[]) {37,  187, 102, 130}),
    };
    int maxServerCount = sizeof_array(zoneServerIps);
    if (clientPacket->routerId >= maxServerCount) {
        error("Invalid RouterId.");
        goto cleanup;
    }

    // Retrieve zone servers ports from Redis
    // Fake ports here until we can retrieve the ports database
    int zoneServerPorts[] = {
        2004, 2005, 2006, 2007
    };

    uint32_t zoneServerIp = zoneServerIps[clientPacket->routerId];
    int zoneServerPort = zoneServerPorts[clientPacket->routerId];

    // Prepare "current commander"
    CommanderSession *commanderSession = &session->game.commanderSession;
    AccountSession *accountSession = &session->game.accountSession;
    size_t commanderIndex = clientPacket->commanderIndex - 1;

    commanderPrint(accountSession->commanders[commanderIndex]);
    commanderSession->currentCommander = accountSession->commanders[commanderIndex];

    // Force update session in redis
    if (!(redisUpdateSession(self->redis, session))) {
        error("Cannot update the Redis session.");
        goto cleanup;
    }

    dbg("routerId %x", session->socket.routerId);
    dbg("mapId %x", session->socket.mapId);
    dbg("accountId %llx", session->socket.accountId);
    dbg("S PcId %x", session->game.commanderSession.currentCommander->pcId);
    dbg("S socialInfoId %llx", session->game.commanderSession.currentCommander->socialInfoId);
    dbg("S commanderId %llx", session->game.commanderSession.currentCommander->commanderId);

    // Move the GameSession to the target Zone
    RedisGameSessionKey fromKey = {
        .routerId = session->socket.routerId,
        .mapId = session->socket.mapId,
        .accountId = session->socket.accountId
    };
    RedisGameSessionKey toKey = {
        .routerId = clientPacket->routerId, // target zoneId
        .mapId = -1,
        .accountId = session->socket.accountId
    };
    if (!(redisMoveGameSession(self->redis, &fromKey, &toKey))) {
        error("Cannot move the Game session %s.", session->socket.sessionKey);
        goto cleanup;
    }

    // Update the session
    session->game.commanderSession.currentCommander = accountSession->commanders[commanderIndex];
    // session->game.commanderSession.currentCommander->mapId = commanderSession->currentCommander->mapId;

    // Build the answer packet
    barrackBuilderStartGameOk(
        self->info.routerId,
        zoneServerIp,
        zoneServerPort,
        session->game.commanderSession.currentCommander->mapId,
        clientPacket->commanderIndex,
        session->game.commanderSession.currentCommander->socialInfoId,
        false,
        reply
    );

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    return status;
}

static PacketHandlerState
barrackHandlerCommanderMove(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    #pragma pack(push, 1)
    struct {
        uint8_t commanderListId;
        PositionXYZ position;
        float angleDestX, angleDestY;
    } *clientPacket = (void *) packet;
    #pragma pack(pop)

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_COMMANDER_MOVE);

    Commander *commander = session->game.commanderSession.currentCommander;

    // TODO : Check position of the client

    // Update session
    memcpy(&commander->pos, &clientPacket->position, sizeof(PositionXZ));

    // Build packet
    barrackBuilderCommanderMoveOk(
        session->socket.accountId,
        clientPacket->commanderListId,
        &commander->pos,
        reply
    );

    return PACKET_HANDLER_UPDATE_SESSION;
}

static PacketHandlerState
barrackHandlerStartBarrack(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;

    // CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_START_BARRACK);

    // IES Modify List
    /*
    BarrackBuilder_iesModifyList(
        reply
    );
    */

    // ??
    /*
    BarrackBuilder_normalUnk1(
        session->socket.accountId,
        reply
    );
    */

    // Get list of Commanders for this AccountId
    size_t commandersCount;

    if (!(mySqlRequestCommandersByAccountId(self->sqlConn, session->socket.accountId, &commandersCount))) {
        error("Cannot request commanders by accountId = %llx", session->socket.accountId);
        goto cleanup;
    }

    {
        Commander commanders[commandersCount];
        session->game.accountSession.commandersCount = commandersCount;
        if (!(mySqlGetCommanders(self->sqlConn, commanders))) {
            error("Cannot get commanders by accountId = %llx", session->socket.accountId);
            goto cleanup;
        }

        // Add commander to session.
        for (int i = 0; i < commandersCount; i++) {
            session->game.accountSession.commanders[i] = malloc(sizeof(Commander));
            memcpy(session->game.accountSession.commanders[i], &commanders[i], sizeof(Commander));
            memcpy(session->game.accountSession.commanders[i]->appearance.familyName, session->game.accountSession.familyName, sizeof(session->game.accountSession.familyName));
        }

        // Send the commander list
        barrackBuilderCommanderList(
            session->socket.accountId,
            &session->game,
            commanders,
            commandersCount,
            reply
        );
    }

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    return status;
}

static PacketHandlerState barrackHandlerCurrentBarrack(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    // CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_CURRENT_BARRACK);

    //  [CLIENT SEND] Packet type : <CB_CURRENT_BARRACK>
    //   =================================================
    //    4E00 03000000 F7030000 D1A8014400000000 03000068 42F0968F 41000070 4111E334 3FCF2635 BF
    //    size pktType  checksum     accountId               float    float    float    float

    barrackBuilderPetInformation(reply);
    barrackBuilderZoneTraffics(1002, reply);

    return PACKET_HANDLER_OK;
}

static PacketHandlerState barrackHandlerBarrackNameChange(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;
    BarrackNameChangeStatus changeStatus = BC_BARRACKNAME_CHANGE_ERROR;

    #pragma pack(push, 1)
    struct{
        uint8_t barrackName[64];
    } *clientPacket = (void *) packet;
    #pragma pack(pop)

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_BARRACKNAME_CHANGE);

    CommanderAppearance *commanderAppearance = &session->game.commanderSession.currentCommander->appearance;

    // Check if the barrack name is not empty and contains only ASCII characters
    size_t barrackNameLen = strlen(clientPacket->barrackName);

    if (barrackNameLen == 0) {
        error("Empty barrack name");
        goto cleanup;
    }

    for (size_t i = 0; i < barrackNameLen; i++) {
         if (!isprint(clientPacket->barrackName[i])) {
            error("Wrong barrack name character in BC_BARRACKNAME_CHANGE");
            goto cleanup;
         }
    }

    // Try to perform the change
    if ((changeStatus = mySqlSetFamilyName(
            self->sqlConn,
            &session->game.accountSession,
            clientPacket->barrackName) != BC_BARRACKNAME_CHANGE_OK))
    {
        error("Cannot change the family name '%s' to '%s'.",
            session->game.accountSession.familyName, clientPacket->barrackName);
        goto cleanup;
    }

    // Update the session
    strncpy(commanderAppearance->familyName, clientPacket->barrackName, sizeof(commanderAppearance->familyName));
    strncpy(session->game.accountSession.familyName,
        clientPacket->barrackName, sizeof(session->game.accountSession.familyName));

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    // Build the reply packet
    barrackBuilderBarrackNameChange(changeStatus, commanderAppearance->familyName, reply);

    if (changeStatus != BC_BARRACKNAME_CHANGE_OK) {
        // The error is displayed to the client, don't update the session though
        status = PACKET_HANDLER_OK;
    }

    return status;
}

static PacketHandlerState barrackHandlerCommanderDestroy(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    #pragma pack(push, 1)
    struct {
        uint8_t commanderIndex;
    }  *clientPacket = (void *) packet;
    #pragma pack(pop)

    dbg("clientPacket->commanderIndex %d", clientPacket->commanderIndex);
    dbg("session->game.accountSession.commandersCount %d", session->game.accountSession.commandersCount);

    // For future reference, clientPacket->commanderIndex 0xFF removes all characters.

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_COMMANDER_DESTROY);

    Commander *commanderToDelete;

    // Update session
    commanderToDelete = session->game.accountSession.commanders[clientPacket->commanderIndex - 1];

    dbg("commanderToDelete->commanderId %d", commanderToDelete->commanderId);

    if (commanderToDelete) {

        // Remove commander from MySQL (or mark to remove?)
        if (MySqlCommanderDelete(self->sqlConn, commanderToDelete->commanderId)) {
            // Update the commanders count
            if (session->game.accountSession.commandersCount > 0) {
                session->game.accountSession.commandersCount -= 1;
            }

            commanderDestroy(&commanderToDelete);
            session->game.accountSession.commanders[clientPacket->commanderIndex - 1] = NULL;

        } else {
            dbg("Problem removing commander from MySQL");
            barrackBuilderMessage(BC_MESSAGE_CUSTOM_MSG, "There was a problem while deleting your Character. Please try again.", reply);
            return PACKET_HANDLER_OK;
        }
    }


    // Build the reply packet
    barrackBuilderCommanderDestroy(clientPacket->commanderIndex, reply);

    return PACKET_HANDLER_UPDATE_SESSION;
}

static PacketHandlerState barrackHandlerCommanderCreate(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    PacketHandlerState status = PACKET_HANDLER_ERROR;
    BcMessageType msgType = BC_MESSAGE_NO_MSG;

    #pragma pack(push, 1)
    struct {
        uint8_t commanderIndex;
        uint8_t commanderName[COMMANDER_NAME_SIZE+1];
        uint16_t jobId;
        uint8_t gender;
        float positionX;
        float positionY;
        float positionZ;
        uint8_t hairId;
    }  *clientPacket = (void *) packet;
    #pragma pack(pop)

    dbg("clientPacket->commanderIndex %d", clientPacket->commanderIndex);
    dbg("session->game.accountSession.commandersCount %d", session->game.accountSession.commandersCount);

    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_COMMANDER_CREATE);

    Commander newCommander;
    commanderInit(&newCommander);
    newCommander.mapId = 1002;

    CommanderAppearance *commanderAppearance = &newCommander.appearance;

    // Validate all parameters

    // Check name
    size_t commanderNameLen = strlen(clientPacket->commanderName);

    if (commanderNameLen == 0) {
        error("Empty commander name");
        msgType = BC_MESSAGE_COMMANDER_NAME_TOO_SHORT;
        goto cleanup;
    }

    for (size_t i = 0; i < commanderNameLen; i++) {
         if (!isprint(clientPacket->commanderName[i])) {
            error("Wrong commander name character in Commander");
            msgType = BC_MESSAGE_NAME_ALREADY_EXIST;
            goto cleanup;
         }
    }

    // Check valid hairId
    /// TODO

    // Check JobID
    switch (clientPacket->jobId) {

        default:
            error("Invalid commander Job ID(%x)", clientPacket->jobId);
            msgType = BC_MESSAGE_CREATE_COMMANDER_FAIL;
            goto cleanup;
            break;

        case COMMANDER_JOB_WARRIOR:
            commanderAppearance->classId = COMMANDER_CLASS_WARRIOR;
            break;

        case COMMANDER_JOB_ARCHER:
            commanderAppearance->classId = COMMANDER_CLASS_ARCHER;
            break;

        case COMMANDER_JOB_WIZARD:
            commanderAppearance->classId = COMMANDER_CLASS_WIZARD;
            break;

        case COMMANDER_JOB_CLERIC:
            commanderAppearance->classId = COMMANDER_CLASS_CLERIC;
            break;
    }

    commanderAppearance->jobId = clientPacket->jobId;

    // Gender
    switch (clientPacket->gender) {
        case COMMANDER_GENDER_MALE:
        case COMMANDER_GENDER_FEMALE:
            commanderAppearance->gender = clientPacket->gender;
            break;

        case COMMANDER_GENDER_BOTH:
        default:
            error("Invalid gender(%d)", clientPacket->gender);
            msgType = BC_MESSAGE_CREATE_COMMANDER_FAIL;
            goto cleanup;
            break;
    }

    int commandersCount = 0;

    /// FIXME : Should check for "max commanders for this current barrack" MAX_COMMANDERS_PER_ACCOUNT is the maximum possible (no matter the barrack player has)
    for (int i = 0; i < MAX_COMMANDERS_PER_ACCOUNT; i++) {
        if (session->game.accountSession.commanders[i] != NULL) {
            commandersCount++;
            if (clientPacket->commanderIndex-1 == i) {
                error("Client sent a malformed commanderIndex. Slot is not empty");
                msgType = BC_MESSAGE_CREATE_COMMANDER_FAIL;
                goto cleanup;
            }
        }
    }

    // At this point, we know that commanderIndex is "at least" a free slot.
    // Check if commanderIndex is in valid boundaries for account and barrack type

    // Character position
    /*
    if (clientPacket->commanderIndex <= accountSession.maxCountCommandersInThisBarrack) { /// TODO :
        error("Client sent a malformed commanderIndex.");
        msgType = BC_MESSAGE_CREATE_COMMANDER_FAIL;
        goto cleanup;
    }
    */

    // CharName
    strncpy(commanderAppearance->commanderName, clientPacket->commanderName, sizeof(commanderAppearance->commanderName));

    // AccountID
    commanderAppearance->accountId = session->socket.accountId;

    // Hair type
    commanderAppearance->hairId = clientPacket->hairId;

    // PCID
    // TODO : check for unicity of the generated pcId
    newCommander.pcId = r1emuGenerateRandom(&self->seed);

    // SocialInfoID
    // TODO : MySQL should generate this ID
    newCommander.socialInfoId = r1emuGenerateRandom64(&self->seed);

    // Position : Center of the barrack
    newCommander.pos = PositionXYZ_decl(19.0, 28.0, 29.0);

    if (!mySqlCommanderInsert(self->sqlConn, session->socket.accountId, &newCommander)) {
        error("Cannot create the commander in the SQL.");
        goto cleanup;
    }

    info("New Commander Created!");
    info("PCID generated : %x", newCommander.pcId);
    info("SocialInfoID generated : %llx", newCommander.socialInfoId);
    info("accountId %llx", commanderAppearance->accountId);

    // Update the session
    Commander *dupCommander = commanderDup(&newCommander);
    session->game.accountSession.commanders[clientPacket->commanderIndex-1] = dupCommander;
    session->game.accountSession.commandersCount++;

    barrackBuilderCommanderCreate(dupCommander, clientPacket->commanderIndex, reply);

    status = PACKET_HANDLER_UPDATE_SESSION;

cleanup:
    if (msgType != BC_MESSAGE_NO_MSG) {
        // The error is handled correctly, reply back to the client but don't update the session.
        barrackBuilderMessage(msgType, "", reply);
        status = PACKET_HANDLER_OK;
    }

    return status;
}

static PacketHandlerState barrackHandlerLogout(
    Worker *self,
    Session *session,
    uint8_t *packet,
    size_t packetSize,
    zmsg_t *reply)
{
    /// TODO

    /*
    CHECK_CLIENT_PACKET_SIZE(*clientPacket, packetSize, CB_LOGOUT);
    */


    barrackBuilderLogoutOk(
        reply
    );

    return PACKET_HANDLER_UPDATE_SESSION;
}
