/*
 * TeamSpeak 3 demo plugin
 *
 * Copyright (c) 2008-2017 TeamSpeak Systems GmbH
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "teamspeak/public_errors.h"
#include "teamspeak/public_errors_rare.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_rare_definitions.h"
#include "teamspeak/clientlib_publicdefinitions.h"
#include "ts3_functions.h"

#include "API.h"

#ifdef _WIN32
#pragma warning (disable : 4100)  /* Disable Unreferenced parameter warning */
#include <Windows.h>
#endif
#include "plugin.h"

static struct TS3Functions ts3Functions;

#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#define snprintf sprintf_s
#else
#define _strcpy(dest, destSize, src) { strncpy(dest, src, destSize-1); (dest)[destSize-1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 24

#define PATH_BUFSIZE 512
#define COMMAND_BUFSIZE 128
#define INFODATA_BUFSIZE 128
#define SERVERINFO_BUFSIZE 256
#define CHANNELINFO_BUFSIZE 512
#define RETURNCODE_BUFSIZE 128

#define API TS3_API
#define User API::TS3User

static char* pluginID = NULL;

/*********************************** Required functions ************************************/
/*
 * If any of these required functions is not implemented, TS3 will refuse to load the plugin
 */

const char* ts3plugin_name() {			
    return "Teamspeak 3 WinAPI"; 
}
const char* ts3plugin_version() {		
    return "1";
}
int ts3plugin_apiVersion() {			
    return PLUGIN_API_VERSION; 
}
const char* ts3plugin_author() {		
    return "Jon  @ https://github.com/3vcloud"; 
}
const char* ts3plugin_description() {	
    return "This plugin creates a locally hosted websocket for applications to access teamspeak info via JSON API"; 
}
int ts3plugin_requestAutoload() {		
    return 1;	
}
/* Set TeamSpeak 3 callback functions */
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    ts3Functions = funcs;
}
// Returns server connection id on success, or 0 on fail
uint64 is_connected() {
    ConnectStatus result = ConnectStatus::STATUS_DISCONNECTED;
    int status = 0;
    uint64 conn = ts3Functions.getCurrentServerConnectionHandlerID();
    if (!conn)
        return 0;
    if (ts3Functions.getConnectionStatus(conn, &status) != ERROR_ok)
        return 0;
    if (static_cast<ConnectStatus>(status) != ConnectStatus::STATUS_CONNECTION_ESTABLISHED)
        return 0;
    return conn;
}
User* getUser(uint64 serverConnectionHandlerID, anyID clientID, bool fresh = false, bool* has_changed = nullptr) {
    if (!is_connected())
        return nullptr;
    User* user = fresh ? nullptr : API::Instance().getUser(clientID);
    if (!user) user = new User();
    // NB: No error checking here, because not much we can do about it
    int ret = ERROR_ok;
    if((ret = ts3Functions.getClientDisplayName(serverConnectionHandlerID, clientID, user->name, sizeof(user->name))) != ERROR_ok)
        API::onLogMessage("Failed to get getClientDisplayName, code 0x%04x", ret);

    uint64 status = 0;
    if((ret = ts3Functions.getClientVariableAsUInt64(serverConnectionHandlerID, clientID, CLIENT_FLAG_TALKING, &status)) == ERROR_ok)
        user->is_talking = status == 1 ? 1 : 0;
    else API::onLogMessage("Failed to get CLIENT_FLAG_TALKING, code 0x%04x", ret);

    if ((ret = ts3Functions.getClientVariableAsUInt64(serverConnectionHandlerID, clientID, CLIENT_OUTPUT_MUTED, &status)) == ERROR_ok)
        user->speakers_muted = status == 1 ? 1 : 0;
    else API::onLogMessage("Failed to get CLIENT_OUTPUT_MUTED, code 0x%04x", ret);

    if ((ret = ts3Functions.getClientVariableAsUInt64(serverConnectionHandlerID, clientID, CLIENT_INPUT_MUTED, &status)) == ERROR_ok)
        user->mic_muted = status == 1 ? 1 : 0;
    else API::onLogMessage("Failed to get CLIENT_INPUT_MUTED, code 0x%04x", ret);

    if ((ret = ts3Functions.getChannelOfClient(serverConnectionHandlerID, clientID, &user->channel_id)) != ERROR_ok)
        API::onLogMessage("Failed to get getChannelOfClient, code 0x%04x", ret);

    return TS3_API::Instance().updateUser(clientID, user, has_changed);
}
int refreshAll() {
    uint64 serverConnectionHandlerID = is_connected();
    auto& instance = API::Instance();
    auto& server = instance.server;
    bool need_to_send = false;
    int ret = ERROR_ok;
    if (!is_connected()) {
        // Not fully connected; make sure client and server vars are flushed
        if (instance.server.id) {
            instance.resetServerVariables();
            instance.sendAllInfo();
        }
        instance.server.id = 0;
        return 0;
    }
    bool new_connection = !instance.server.id;
    char* s;
    if ((ret = ts3Functions.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_NAME, &s)) != ERROR_ok) {
        API::onLogMessage("Failed to get server name, code 0x%04x", ret);
        return 1;
    }
    if (strcmp(s, server.name) != 0) {
        need_to_send = true;
        // Server has changed; update details
        strncpy_s(server.name, s, sizeof(server.name) - 1);
        ts3Functions.freeMemory(s);

        // Get server info
        if ((ret = ts3Functions.getServerConnectInfo(serverConnectionHandlerID, server.host, &server.port, server.password, sizeof(server.password))) != ERROR_ok) {
            API::onLogMessage("Failed to get getServerConnectInfo, code 0x%04x", ret);
            return 1;
        }
        if ((ret = ts3Functions.getServerVariableAsString(serverConnectionHandlerID, VIRTUALSERVER_NAME, &s)) != ERROR_ok) {
            API::onLogMessage("Failed to get server name, code 0x%04x", ret);
            return 1;
        }
        strncpy_s(server.name, s, sizeof(server.name) - 1);
        ts3Functions.freeMemory(s);
    }
    else {
        ts3Functions.freeMemory(s);
    }

    if ((ret = ts3Functions.getClientID(serverConnectionHandlerID, &server.my_client_id)) != ERROR_ok) {
        API::onLogMessage("Failed to get my id, code 0x%04x", ret);
        return 1;
    }
    // Update all connected users
    anyID* clientIDs;
    if ((ret = ts3Functions.getClientList(serverConnectionHandlerID, &clientIDs)) != ERROR_ok) {
        API::onLogMessage("Failed to get getClientList, code 0x%04x", ret);
        return 1;
    }
    bool has_changed = false;
    
    for (size_t i = 0; clientIDs[i] != 0; i++) {
        getUser(serverConnectionHandlerID, clientIDs[i], false, &has_changed);
        need_to_send |= has_changed;
    }
    ts3Functions.freeMemory(clientIDs);

    if (need_to_send) {
        instance.sendAllInfo();
    }
    return 0;
}
int ts3plugin_updateAPIServer() {
    return refreshAll();
}



/*
 * Custom code called right after loading the plugin. Returns 0 on success, 1 on failure.
 * If the function returns 1 on failure, the plugin will be unloaded again.
 */
int ts3plugin_init() { 
    TS3_API::Instance();
    return 0; 
}
/* Custom code called right before the plugin is unloaded */
void ts3plugin_shutdown() { /* Your plugin cleanup code here */ }

/****************************** Optional functions ********************************/
/*
 * Following functions are optional, if not needed you don't need to implement them.
 */

/* Client changed current server connection handler */
void ts3plugin_currentServerConnectionChanged(uint64 serverConnectionHandlerID) {
    printf("PLUGIN: currentServerConnectionChanged %llu (%llu)\n", (long long unsigned int)serverConnectionHandlerID, (long long unsigned int)ts3Functions.getCurrentServerConnectionHandlerID());
    ts3plugin_updateAPIServer();
}

/* Required to release the memory for parameter "data" allocated in ts3plugin_infoData and ts3plugin_initMenus */
void ts3plugin_freeMemory(void* data) {
	free(data);
}

/************************** TeamSpeak callbacks ***************************/
/*
 * Following functions are optional, feel free to remove unused callbacks.
 * See the clientlib documentation for details on each function.
 */

/* Clientlib */

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
    /* Some example code following to show how to use the information query functions. */
    ts3plugin_updateAPIServer();
}

void ts3plugin_onUpdateChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
}

void ts3plugin_onUpdateChannelEditedEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
}

void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
    getUser(serverConnectionHandlerID, clientID, true);
}

void ts3plugin_channeUpdate(uint64 serverConnectionHandlerID, anyID clientID, uint64 channelID) {
    User* user = getUser(serverConnectionHandlerID, clientID);
    if (!user)
        return;
    user->channel_id = channelID;
    TS3_API::Instance().sendClient(clientID);
}

void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage) {
    ts3plugin_channeUpdate(serverConnectionHandlerID, clientID, newChannelID);
}

void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
    ts3plugin_channeUpdate(serverConnectionHandlerID, clientID, newChannelID);
}

void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
    API::Instance().deleteUser(clientID);
}
void ts3plugin_onServerEditedEvent(uint64 serverConnectionHandlerID, anyID editerID, const char* editerName, const char* editerUniqueIdentifier) {
    ts3plugin_updateAPIServer();
}

void ts3plugin_onServerStopEvent(uint64 serverConnectionHandlerID, const char* shutdownMessage) {
    ts3plugin_updateAPIServer();
}

int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID, anyID fromID, const char* fromName, const char* fromUniqueIdentifier, const char* message, int ffIgnored) {
    User* from = getUser(serverConnectionHandlerID, fromID, false);
    if (!from)
        return ffIgnored;
    User* to = getUser(serverConnectionHandlerID, toID, false);
    API::Instance().onTeamspeakMessage(from, to, message);
    API::Instance().onLogMessage("PLUGIN: onTextMessageEvent %llu %d %d %s %s %d\n", (long long unsigned int)serverConnectionHandlerID, targetMode, fromID, fromName, message, ffIgnored);
    return ffIgnored;  /* 0 = handle normally, 1 = client will ignore the text message */
}

void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
	/* Demonstrate usage of getClientDisplayName */
    User* from = getUser(serverConnectionHandlerID, clientID, false);
    if (!from) return;
    from->is_talking = status == 1 ? 1 : 0;
    API::Instance().sendClient(clientID);
}

/* Clientlib rare */

void ts3plugin_onClientChannelGroupChangedEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, uint64 channelID, anyID clientID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
    getUser(serverConnectionHandlerID, clientID);
}

void ts3plugin_onServerGroupClientAddedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
    getUser(serverConnectionHandlerID, clientID);
}

void ts3plugin_onServerGroupClientDeletedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
    getUser(serverConnectionHandlerID, clientID);
}

void ts3plugin_onServerLogEvent(uint64 serverConnectionHandlerID, const char* logMsg) {
}

/* Client UI callbacks */

/*
 * Called from client when an avatar image has been downloaded to or deleted from cache.
 * This callback can be called spontaneously or in response to ts3Functions.getAvatar()
 */
void ts3plugin_onAvatarUpdated(uint64 serverConnectionHandlerID, anyID clientID, const char* avatarPath) {
    getUser(serverConnectionHandlerID, clientID);
	/* If avatarPath is NULL, the avatar got deleted */
	/* If not NULL, avatarPath contains the path to the avatar file in the TS3Client cache */
}

/* Called when client custom nickname changed */
void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID, const char* displayName, const char* uniqueClientIdentifier) {
    User* user = getUser(serverConnectionHandlerID, clientID, false);
    if (!user) return;
    strncpy_s(user->name, displayName, sizeof(user->name));
    API::Instance().sendClient(clientID);
}
